/*
 * Copyright 6WIND 2012-2013, All rights reserved.
 * Copyright Mellanox 2012, All rights reserved.
 */

/*
 * Known limitations:
 * - Multiple RX VLAN filters can be configured, but only the first one
 *   works properly.
 * - RSS hash key and options cannot be modified.
 * - Hardware counters aren't implemented.
 */

/* System headers. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

/* DPDK headers don't like -pedantic. */
#ifdef PEDANTIC
#pragma GCC diagnostic ignored "-pedantic"
#endif
#include <rte_config.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <rte_prefetch.h>
#include <rte_malloc.h>
#include <rte_spinlock.h>
#include <rte_atomic.h>
#ifdef PEDANTIC
#pragma GCC diagnostic error "-pedantic"
#endif

/* Verbs header. */
/* ISO C doesn't support unnamed structs/unions, disabling -pedantic. */
#ifdef PEDANTIC
#pragma GCC diagnostic ignored "-pedantic"
#endif
#include <infiniband/verbs.h>
#ifdef PEDANTIC
#pragma GCC diagnostic error "-pedantic"
#endif

/* System configuration header. */
#include "config.h"

/* PMD header. */
#include "mlx4.h"

#ifndef MLX4_PMD_SOFT_COUNTERS
#error Hardware counters not implemented, MLX4_PMD_SOFT_COUNTERS is required.
#endif

/* If raw send operations are available, use them since they are faster. */
#ifdef SEND_RAW_WR_SUPPORT
typedef struct ibv_send_wr_raw mlx4_send_wr_t;
#define mlx4_post_send ibv_post_send_raw
#else
typedef struct ibv_send_wr mlx4_send_wr_t;
#define mlx4_post_send ibv_post_send
#endif

struct mlx4_rxq_stats {
	unsigned int idx; /**< Mapping index. */
	uint64_t ipackets;  /**< Total of successfully received packets. */
	uint64_t ibytes;    /**< Total of successfully received bytes. */
	uint64_t idropped;  /**< Total of packets dropped when RX ring full. */
	uint64_t rx_nombuf; /**< Total of RX mbuf allocation failures. */
};

struct mlx4_txq_stats {
	unsigned int idx; /**< Mapping index. */
	uint64_t opackets; /**< Total of successfully sent packets. */
	uint64_t obytes;   /**< Total of successfully sent bytes. */
	uint64_t odropped; /**< Total of packets not sent when TX ring full. */
};

/* Helper to get the size of a memory pool. */
static size_t mp_total_size(struct rte_mempool *mp)
{
	size_t ret;

	/* The same calculation is used in rte_mempool_create(). */
	ret = (mp->header_size + mp->elt_size + mp->trailer_size);
	ret *= mp->size;
	ret += sizeof(*mp);
	ret += mp->private_data_size;
	return ret;
}

/* RX element (scattered packets). */
struct rxq_elt_sp {
	struct ibv_recv_wr wr; /* Work Request. */
	struct ibv_sge sges[MLX4_PMD_SGE_WR_N]; /* Scatter/Gather Elements. */
	struct rte_mbuf *bufs[MLX4_PMD_SGE_WR_N]; /* SGEs buffers. */
};

/* RX element. */
struct rxq_elt {
	struct ibv_recv_wr wr; /* Work Request. */
	struct ibv_sge sge; /* Scatter/Gather Element. */
	/* SGE buffer is stored as the work request ID to reduce size. */
};

/* RX queue descriptor. */
struct rxq {
	struct priv *priv; /* Back pointer to private data. */
	struct rte_mempool *mp; /* Memory Pool for allocations. */
	size_t mp_size; /* mp size in bytes. */
	struct ibv_mr *mr; /* Memory Region (for mp). */
	struct ibv_cq *cq; /* Completion Queue. */
	struct ibv_qp *qp; /* Queue Pair. */
	/*
	 * There is exactly one flow configured per MAC address. Each flow
	 * may contain several specifications, one per configured VLAN ID.
	 */
	BITFIELD_DECLARE(mac_configured, uint32_t, MLX4_MAX_MAC_ADDRESSES);
	struct ibv_flow *mac_flow[MLX4_MAX_MAC_ADDRESSES];
	struct ibv_flow *promisc_flow; /* Promiscuous flow. */
	struct ibv_flow *allmulti_flow; /* Multicast flow. */
	unsigned int port_id; /* Port ID for incoming packets. */
	unsigned int elts_n; /* (*elts)[] length. */
	union {
		struct rxq_elt_sp (*sp)[]; /* Scattered RX elements. */
		struct rxq_elt (*no_sp)[]; /* RX elements. */
	} elts;
	unsigned int sp:1; /* Use scattered RX elements. */
	uint32_t mb_len; /* Length of a mp-issued mbuf. */
	struct mlx4_rxq_stats stats; /* RX queue counters. */
	unsigned int socket; /* CPU socket ID for allocations. */
};

/* TX Work Request element. */
struct txq_wr {
	mlx4_send_wr_t wr; /* Work Request. */
	struct ibv_sge sges[MLX4_PMD_SGE_WR_N]; /* Scatter/Gather Elements. */
};

/* TX buffers for a Work Request element. */
struct txq_buf {
	struct rte_mbuf *bufs[MLX4_PMD_SGE_WR_N]; /* SGEs buffers. */
	struct txq_buf *comp; /* Elements with the same completion event. */
};

/* TX queue descriptor. */
struct txq {
	struct priv *priv; /* Back pointer to private data. */
	struct {
		struct rte_mempool *mp; /* Cached Memory Pool. */
		size_t mp_size; /* mp size in bytes. */
		struct ibv_mr *mr; /* Memory Region (for mp). */
		uint32_t lkey; /* mr->lkey */
	} mp2mr[MLX4_PMD_TX_MP_CACHE]; /* MP to MR translation table. */
	struct ibv_cq *cq; /* Completion Queue. */
	struct ibv_qp *qp; /* Queue Pair. */
#if MLX4_PMD_MAX_INLINE > 0
	uint32_t max_inline; /* Max inline send size <= MLX4_PMD_MAX_INLINE. */
#endif
	unsigned int elts_n; /* (*elts_*)[] length. */
	struct txq_wr (*elts_wr)[]; /* Work Requests elements. */
	struct txq_buf (*elts_buf)[]; /* WRs buffers. */
	unsigned int elts_cur; /* Current index in (*elts)[]. */
	unsigned int elts_comp; /* Number of WRs waiting for completion. */
	unsigned int elts_used; /* Number of used WRs (including elts_comp). */
	unsigned int elts_free; /* Number of free WRs. */
	struct txq_buf *lost_comp; /* Elements without a completion event. */
	struct mlx4_txq_stats stats; /* TX queue counters. */
	unsigned int socket; /* CPU socket ID for allocations. */
};

struct priv {
	struct rte_eth_dev *dev; /* Ethernet device. */
	struct ibv_context *ctx; /* Verbs context. */
	struct ibv_device_attr device_attr; /* Device properties. */
	struct ibv_port_attr port_attr; /* Physical port properties. */
	struct ibv_pd *pd; /* Protection Domain. */
	/*
	 * MAC addresses array and configuration bit-field.
	 * An extra entry that cannot be modified by the DPDK is reserved
	 * for broadcast frames (destination MAC address ff:ff:ff:ff:ff:ff).
	 */
	struct ether_addr mac[MLX4_MAX_MAC_ADDRESSES];
	BITFIELD_DECLARE(mac_configured, uint32_t, MLX4_MAX_MAC_ADDRESSES);
	/* VLAN filters. */
	struct {
		unsigned int enabled:1; /* If enabled. */
		unsigned int id:12; /* VLAN ID (0-4095). */
	} vlan_filter[MLX4_MAX_VLAN_IDS]; /* VLAN filters table. */
	/* Device properties. */
	uint16_t mtu; /* Configured MTU. */
	uint8_t port; /* Physical port number. */
	unsigned int started:1; /* Device started, flows enabled. */
	unsigned int promisc:1; /* Device in promiscuous mode. */
	unsigned int allmulti:1; /* Device receives all multicast packets. */
	unsigned int hw_qpg:1; /* QP groups are supported. */
	unsigned int hw_tss:1; /* TSS is supported. */
	unsigned int hw_rss:1; /* RSS is supported. */
	unsigned int rss:1; /* RSS is enabled. */
#ifdef MLX4_COMPAT_VMWARE
	unsigned int vmware:1; /* Use VMware compatibility. */
#endif
	unsigned int vf:1; /* This is a VF device. */
	unsigned int max_rss_tbl_sz; /* Maximum number of RSS queues. */
	/* RX/TX queues. */
	struct rxq rxq_parent; /* Parent queue when RSS is enabled. */
	unsigned int rxqs_n; /* RX queues array size. */
	unsigned int txqs_n; /* TX queues array size. */
	struct rxq *(*rxqs)[]; /* RX queues. */
	struct txq *(*txqs)[]; /* TX queues. */
	rte_spinlock_t lock; /* Lock for control functions. */
};

static void
priv_lock(struct priv *priv)
{
	rte_spinlock_lock(&priv->lock);
}

static void
priv_unlock(struct priv *priv)
{
	rte_spinlock_unlock(&priv->lock);
}

/* Allocate a buffer on the stack and fill it with a printf format string. */
#define MKSTR(name, ...) \
	char name[snprintf(NULL, 0, __VA_ARGS__) + 1]; \
	\
	snprintf(name, sizeof(name), __VA_ARGS__)

/* Get interface name from priv. */
static int
priv_get_ifname(const struct priv *priv, char (*ifname)[IF_NAMESIZE])
{
	int ret = -1;
	DIR *dir;
	struct dirent *dent;

	{
		MKSTR(path, "%s/device/net", priv->ctx->device->ibdev_path);

		dir = opendir(path);
		if (dir == NULL)
			return -1;
	}
	while ((dent = readdir(dir)) != NULL) {
		char *name = dent->d_name;
		FILE *file;
		unsigned int dev_id;
		int r;

		if ((name[0] == '.') &&
		    ((name[1] == '\0') ||
		     ((name[1] == '.') && (name[2] == '\0'))))
			continue;

		MKSTR(path, "%s/device/net/%s/dev_id",
		      priv->ctx->device->ibdev_path, name);

		file = fopen(path, "rb");
		if (file == NULL)
			continue;
		r = fscanf(file, "%x", &dev_id);
		fclose(file);
		if ((r == 1) && (dev_id == (priv->port - 1u))) {
			snprintf(*ifname, sizeof(*ifname), "%s", name);
			ret = 0;
			break;
		}
	}
	closedir(dir);
	return ret;
}

/* Read from sysfs entry. */
static int
priv_sysfs_read(const struct priv *priv, const char *entry,
		char *buf, size_t size)
{
	char ifname[IF_NAMESIZE];
	FILE *file;
	int ret;
	int err;

	if (priv_get_ifname(priv, &ifname))
		return -1;

	MKSTR(path, "%s/device/net/%s/%s", priv->ctx->device->ibdev_path,
	      ifname, entry);

	file = fopen(path, "rb");
	if (file == NULL)
		return -1;
	ret = fread(buf, 1, size, file);
	err = errno;
	if (((size_t)ret < size) && (ferror(file)))
		ret = -1;
	else
		ret = size;
	fclose(file);
	errno = err;
	return ret;
}

/* Write to sysfs entry. */
static int
priv_sysfs_write(const struct priv *priv, const char *entry,
		 char *buf, size_t size)
{
	char ifname[IF_NAMESIZE];
	FILE *file;
	int ret;
	int err;

	if (priv_get_ifname(priv, &ifname))
		return -1;

	MKSTR(path, "%s/device/net/%s/%s", priv->ctx->device->ibdev_path,
	      ifname, entry);

	file = fopen(path, "wb");
	if (file == NULL)
		return -1;
	ret = fwrite(buf, 1, size, file);
	err = errno;
	if (((size_t)ret < size) || (ferror(file)))
		ret = -1;
	else
		ret = size;
	fclose(file);
	errno = err;
	return ret;
}

/* Get unsigned long sysfs property. */
static int
priv_get_sysfs_ulong(struct priv *priv, const char *name, unsigned long *value)
{
	int ret;
	unsigned long value_ret;
	char value_str[32];

	ret = priv_sysfs_read(priv, name, value_str, (sizeof(value_str) - 1));
	if (ret == -1) {
		DEBUG("cannot read %s value from sysfs: %s",
		      name, strerror(errno));
		return -1;
	}
	value_str[ret] = '\0';
	errno = 0;
	value_ret = strtoul(value_str, NULL, 0);
	if (errno) {
		DEBUG("invalid %s value `%s': %s", name, value_str,
		      strerror(errno));
		return -1;
	}
	*value = value_ret;
	return 0;
}

/* Set unsigned long sysfs property. */
static int
priv_set_sysfs_ulong(struct priv *priv, const char *name, unsigned long value)
{
	int ret;
	MKSTR(value_str, "%lu", value);

	ret = priv_sysfs_write(priv, name, value_str, (sizeof(value_str) - 1));
	if (ret == -1) {
		DEBUG("cannot write %s `%s' (%lu) to sysfs: %s",
		      name, value_str, value, strerror(errno));
		return -1;
	}
	return 0;
}

/* Perform ifreq ioctl() on associated Ethernet device. */
static int
priv_ifreq(const struct priv *priv, int req, struct ifreq *ifr)
{
	int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	int ret = -1;

	if (sock == -1)
		return ret;
	if (priv_get_ifname(priv, &ifr->ifr_name) == 0)
		ret = ioctl(sock, req, ifr);
	close(sock);
	return ret;
}

/* Get device MTU. */
static int
priv_get_mtu(struct priv *priv, uint16_t *mtu)
{
	unsigned long ulong_mtu;

	if (priv_get_sysfs_ulong(priv, "mtu", &ulong_mtu) == -1)
		return -1;
	*mtu = ulong_mtu;
	return 0;
}

/* Set device MTU. */
static int
priv_set_mtu(struct priv *priv, uint16_t mtu)
{
	return priv_set_sysfs_ulong(priv, "mtu", mtu);
}

/* Device configuration. */

static int
rxq_setup(struct rte_eth_dev *dev, struct rxq *rxq, uint16_t desc,
	  unsigned int socket, const struct rte_eth_rxconf *conf,
	  struct rte_mempool *mp);

static void
rxq_cleanup(struct rxq *rxq);

static int
dev_configure(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int rxqs_n = dev->data->nb_rx_queues;
	unsigned int txqs_n = dev->data->nb_tx_queues;
	unsigned int tmp;
	int ret;

	priv->rxqs = (void *)dev->data->rx_queues;
	priv->txqs = (void *)dev->data->tx_queues;
	if (txqs_n != priv->txqs_n) {
		DEBUG("%p: TX queues number update: %u -> %u",
		      (void *)dev, priv->txqs_n, txqs_n);
		priv->txqs_n = txqs_n;
	}
	if (rxqs_n == priv->rxqs_n)
		return 0;
	DEBUG("%p: RX queues number update: %u -> %u",
	      (void *)dev, priv->rxqs_n, rxqs_n);
	/* If RSS is enabled, disable it first. */
	if (priv->rss) {
		unsigned int i;

		/* Only if there are no remaining child RX queues. */
		for (i = 0; (i != priv->rxqs_n); ++i)
			if ((*priv->rxqs)[i] != NULL)
				return -EINVAL;
		rxq_cleanup(&priv->rxq_parent);
		priv->rss = 0;
		priv->rxqs_n = 0;
	}
	if (rxqs_n <= 1) {
		/* Nothing else to do. */
		priv->rxqs_n = rxqs_n;
		return 0;
	}
	/* Allocate a new RSS parent queue if supported by hardware. */
	if (!priv->hw_rss) {
		DEBUG("%p: only a single RX queue can be configured when"
		      " hardware doesn't support RSS",
		      (void *)dev);
		return -EINVAL;
	}
	/* Fail if hardware doesn't support that many RSS queues. */
	if (rxqs_n >= priv->max_rss_tbl_sz) {
		DEBUG("%p: only %u RX queues can be configured for RSS",
		      (void *)dev, priv->max_rss_tbl_sz);
		return -EINVAL;
	}
	priv->rss = 1;
	tmp = priv->rxqs_n;
	priv->rxqs_n = rxqs_n;
	ret = rxq_setup(dev, &priv->rxq_parent, 0, 0, NULL, NULL);
	if (!ret)
		return 0;
	/* Failure, rollback. */
	priv->rss = 0;
	priv->rxqs_n = tmp;
	return ret;
}

static int
mlx4_dev_configure(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	ret = dev_configure(dev);
	priv_unlock(priv);
	return ret;
}

/* TX queues handling. */

static int
txq_alloc_elts(struct txq *txq, unsigned int elts_n)
{
	unsigned int i;
	struct txq_wr (*elts_wr)[elts_n] =
		rte_calloc_socket("TXQ WRs", 1, sizeof(*elts_wr), 0,
				  txq->socket);
	struct txq_buf (*elts_buf)[elts_n] =
		rte_calloc_socket("TXQ buffers", 1, sizeof(*elts_buf), 0,
				  txq->socket);
	int ret = 0;

	if ((elts_wr == NULL) || (elts_buf == NULL)) {
		DEBUG("%p: can't allocate packets array", (void *)txq);
		ret = ENOMEM;
		goto error;
	}
	for (i = 0; (i != elts_n); ++i) {
		struct txq_wr *elt_wr = &(*elts_wr)[i];
		struct txq_buf *elt_buf = &(*elts_buf)[i];
		mlx4_send_wr_t *wr = &elt_wr->wr;
		struct ibv_sge (*sges)[elemof(elt_wr->sges)] = &elt_wr->sges;

		/* These two arrays must have the same size. */
		assert(elemof(elt_wr->sges) == elemof(elt_buf->bufs));
		/* Configure WR. */
		wr->wr_id = i;
		wr->sg_list = &(*sges)[0];
		wr->opcode = IBV_WR_SEND;
		/* The following fields will be updated during each TX
		 * operation. Initializing them is pointless. */
		wr->next = NULL;
		wr->num_sge = elemof(*sges);
		wr->send_flags = IBV_SEND_SIGNALED;
		/* The same applies to elt_wr->sges and elt_buf. */
		(void)elt_buf;
	}
	DEBUG("%p: allocated and configured %u WRs (%zu segments)",
	      (void *)txq, elts_n, (elts_n * elemof((*elts_wr)[0].sges)));
	txq->elts_n = elts_n;
	txq->elts_wr = elts_wr;
	txq->elts_buf = elts_buf;
	txq->elts_cur = 0;
	txq->elts_comp = 0;
	txq->elts_used = 0;
	txq->elts_free = elts_n;
	assert(ret == 0);
	return 0;
error:
	if (elts_wr != NULL)
		rte_free(elts_wr);
	if (elts_buf != NULL)
		rte_free(elts_buf);
	DEBUG("%p: failed, freed everything", (void *)txq);
	assert(ret != 0);
	return ret;
}

static void
txq_free_elts(struct txq *txq)
{
	unsigned int i;
	unsigned int elts_n = txq->elts_n;
	struct txq_wr (*elts_wr)[elts_n] = txq->elts_wr;
	struct txq_buf (*elts_buf)[elts_n] = txq->elts_buf;

	DEBUG("%p: freeing WRs", (void *)txq);
	txq->elts_n = 0;
	txq->elts_wr = NULL;
	txq->elts_buf = NULL;
	if (elts_wr != NULL)
		rte_free(elts_wr);
	if (elts_buf == NULL)
		return;
	for (i = 0; (i != elemof(*elts_buf)); ++i) {
		unsigned int j;
		struct txq_buf *elt_buf = &(*elts_buf)[i];

		for (j = 0; (j != elemof(elt_buf->bufs)); ++j) {
			struct rte_mbuf *buf = elt_buf->bufs[j];

			if (buf != NULL)
				rte_pktmbuf_free_seg(buf);
		}
	}
	rte_free(elts_buf);
}

static void
txq_cleanup(struct txq *txq)
{
	size_t i;

	DEBUG("cleaning up %p", (void *)txq);
	txq_free_elts(txq);
	if (txq->qp != NULL)
		claim_zero(ibv_destroy_qp(txq->qp));
	if (txq->cq != NULL)
		claim_zero(ibv_destroy_cq(txq->cq));
	for (i = 0; (i != elemof(txq->mp2mr)); ++i) {
		if (txq->mp2mr[i].mp == NULL)
			break;
		assert(txq->mp2mr[i].mr != NULL);
		claim_zero(ibv_dereg_mr(txq->mp2mr[i].mr));
	}
	memset(txq, 0, sizeof(*txq));
}

static int
txq_complete(struct txq *txq)
{
	unsigned int elts_comp = txq->elts_comp;
	unsigned int elts_used = txq->elts_used;
	unsigned int elts_free = txq->elts_free;
	int wcs_n;
	int i;
	int ret = 0;

	assert((elts_used + elts_free) == txq->elts_n);
	if (unlikely(elts_comp == 0))
		return 0;
#ifdef DEBUG_SEND
	DEBUG("%p: processing %u work requests completions",
	      (void *)txq, elts_comp);
#endif
	assert(elts_comp <= elts_used);

	/* XXX careful with stack overflows. */
	struct ibv_wc wcs[elts_comp];

	wcs_n = ibv_poll_cq(txq->cq, elts_comp, wcs);
	if (unlikely(wcs_n == 0))
		return 0;
	if (unlikely(wcs_n < 0)) {
		DEBUG("txq=%p, ibv_poll_cq() failed (wc_n=%d)",
		      (void *)txq, wcs_n);
		return -1;
	}
	/* Clear lost completion events. */
	while (unlikely(txq->lost_comp != NULL)) {
		unsigned int j = 0;
		struct txq_buf *elt_buf = txq->lost_comp;
		struct rte_mbuf *(*bufs)[elemof(elt_buf->bufs)] =
			&elt_buf->bufs;

		txq->lost_comp = elt_buf->comp;
		assert(elts_used != 0);
		assert(elts_free != txq->elts_n);
		/* There's at least one valid buffer. */
		assert((*bufs)[0] != NULL);
		do {
			struct rte_mbuf *buf = (*bufs)[j];

			/* Buffer pointer must be NULL because we don't use
			 * wr->num_sge to check how many packets are actually
			 * there. */
			(*bufs)[j] = NULL;
			rte_pktmbuf_free_seg(buf);
			++j;
		}
		while ((j < elemof(*bufs)) && ((*bufs)[j] != NULL));
		--elts_used;
		++elts_free;
	}
	for (i = 0; (i != wcs_n); ++i) {
		struct ibv_wc *wc = &wcs[i];
		uint64_t wr_id = wc->wr_id;
		struct txq_wr *elt_wr = &(*txq->elts_wr)[wr_id];
		struct txq_buf *elt_buf = &(*txq->elts_buf)[wr_id];

		assert(wr_id < txq->elts_n);
		if (unlikely(wc->status != IBV_WC_SUCCESS)) {
			DEBUG("txq=%p, wr_id=%" PRIu64 ": bad work completion"
			      " status (%d): %s",
			      (void *)txq, wc->wr_id, wc->status,
			      ibv_wc_status_str(wc->status));
			/* We can't do much about this. */
			ret = -1;
#ifdef MLX4_PMD_SOFT_COUNTERS
			/* Increase dropped packets counter. */
			/* XXX we don't know how many packets were actually
			 * dropped. */
			++txq->stats.odropped;
#endif
		}
		else
			assert(wc->opcode == IBV_WC_SEND);
		/*
		 * XXX check number of bytes transferred, but for some reason,
		 * wc->byte_len is always 0.
		 */
		/* Free all buffers associated to this completion event. */
		do {
			unsigned int j = 0;
			struct rte_mbuf *(*bufs)[elemof(elt_buf->bufs)] =
				&elt_buf->bufs;

			assert(elts_used != 0);
			assert(elts_free != txq->elts_n);
			/* There's at least one valid buffer. */
			assert((*bufs)[0] != NULL);
			do {
				struct rte_mbuf *buf = (*bufs)[j];

				/* Buffer pointer must be NULL. */
				(*bufs)[j] = NULL;
#ifndef NDEBUG
				/* Make sure this segment is unlinked. */
				buf->next = NULL;
				/* SGE poisoning shouldn't hurt. */
				memset(&elt_wr->sges[j], 0x44,
				       sizeof(elt_wr->sges[j]));
#else
				(void)elt_wr;
#endif
				rte_pktmbuf_free_seg(buf);
				++j;
			}
			while ((j < elemof(*bufs)) && ((*bufs)[j] != NULL));
			--elts_used;
			++elts_free;
			elt_buf = elt_buf->comp;
		}
		while (unlikely(elt_buf != NULL));
		assert(elts_comp != 0);
		--elts_comp;
	}
	assert(elts_comp <= elts_used);
	assert((elts_used + elts_free) == txq->elts_n);
	txq->elts_comp = elts_comp;
	txq->elts_used = elts_used;
	txq->elts_free = elts_free;
	return ret;
}

/*
 * Get Memory Region (MR) <-> Memory Pool (MP) association from txq->mp2mr[].
 * Add MP to txq->mp2mr[] if it's not registered yet. If mp2mr[] is full,
 * remove an entry first.
 *
 * Return mr->lkey on success, (uint32_t)-1 on failure.
 */
static uint32_t
txq_mp2mr(struct txq *txq, struct rte_mempool *mp)
{
	unsigned int i;
	size_t mp_size;
	struct ibv_mr *mr;

	for (i = 0; (i != elemof(txq->mp2mr)); ++i) {
		if (unlikely(txq->mp2mr[i].mp == NULL)) {
			/* Unknown MP, add a new MR for it. */
			break;
		}
		if (txq->mp2mr[i].mp == mp) {
			assert(txq->mp2mr[i].lkey != (uint32_t)-1);
			assert(txq->mp2mr[i].mr->lkey == txq->mp2mr[i].lkey);
			assert(txq->mp2mr[i].mp_size != 0);
			return txq->mp2mr[i].lkey;
		}
	}
	/* Add a new entry, register MR first. */
	DEBUG("%p: discovered new memory pool %p", (void *)txq, (void *)mp);
	mp_size = mp_total_size(mp);
	mr = ibv_reg_mr(txq->priv->pd, mp, mp_size,
			(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
	if (unlikely(mr == NULL)) {
		DEBUG("%p: unable to configure MR, ibv_reg_mr() failed.",
		      (void *)txq);
		return (uint32_t)-1;
	}
	if (unlikely(i == elemof(txq->mp2mr))) {
		/* Table is full, remove oldest entry. */
		DEBUG("%p: MR <-> MP table full, dropping oldest entry.",
		      (void *)txq);
		--i;
		claim_zero(ibv_dereg_mr(txq->mp2mr[i].mr));
		memmove(&txq->mp2mr[0], &txq->mp2mr[1],
			(sizeof(txq->mp2mr) - sizeof(txq->mp2mr[0])));
	}
	/* Store the new entry. */
	txq->mp2mr[i].mp = mp;
	txq->mp2mr[i].mp_size = mp_size;
	txq->mp2mr[i].mr = mr;
	txq->mp2mr[i].lkey = mr->lkey;
	DEBUG("%p: new MR lkey for MP %p: 0x%08" PRIu32,
	      (void *)txq, (void *)mp, txq->mp2mr[i].lkey);
	return txq->mp2mr[i].lkey;
}

static uint16_t
mlx4_tx_burst(void *dpdk_txq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct txq *txq = (struct txq *)dpdk_txq;
	mlx4_send_wr_t head;
	mlx4_send_wr_t **wr_next = &head.next;
	struct txq_buf *elt_prev = NULL;
	struct txq_buf (*elts_buf)[txq->elts_n] = txq->elts_buf;
	struct txq_wr (*elts_wr)[txq->elts_n] = txq->elts_wr;
	mlx4_send_wr_t *bad_wr;
	unsigned int elts_cur = txq->elts_cur;
	unsigned int i;
	unsigned int max;
	unsigned int dropped;
	int err;

	txq_complete(txq);
	if (unlikely(pkts_n == 0))
		return 0;
	max = txq->elts_free;
	if (max == 0) {
#ifdef DEBUG_SEND
		DEBUG("%p: can't send %u packet(s), no WR(s) available"
		      " (%u waiting for completion)",
		      (void *)txq, pkts_n, txq->elts_used);
#endif
		return 0;
	}
	if (max > pkts_n)
		max = pkts_n;
	dropped = 0;
	for (i = 0; (i != max); ++i) {
		struct txq_buf *elt_buf = &(*elts_buf)[elts_cur];
		struct txq_wr *elt_wr = &(*elts_wr)[elts_cur];
		mlx4_send_wr_t *wr = &elt_wr->wr;
		struct rte_mbuf *buf;
		unsigned int seg_n = 0;
		unsigned int sent_size = 0;

		assert(elts_cur < txq->elts_n);
		assert(wr->wr_id == elts_cur);
		assert(wr->sg_list == elt_wr->sges);
		assert(wr->opcode == IBV_WR_SEND);
		/* Link WRs together for ibv_post_send(). */
		*wr_next = wr;
		wr_next = &wr->next;
		/* Link elements together in order to register a single
		 * completion event per burst of packets. */
		wr->send_flags &= ~IBV_SEND_SIGNALED;
		elt_buf->comp = elt_prev;
		elt_prev = elt_buf;
		/* Register each segment as SGEs. */
		for (buf = pkts[i];
		     ((buf != NULL) && (seg_n != elemof(elt_wr->sges)));
		     buf = buf->next) {
			struct ibv_sge *sge = &elt_wr->sges[seg_n];
			uint32_t lkey;

			/* Retrieve Memory Region key for this memory pool. */
			lkey = txq_mp2mr(txq, buf->pool);
			if (unlikely(lkey == (uint32_t)-1)) {
				/* MR doesn't exist, stop here. */
				DEBUG("unable to get MP <-> MR association");
				break;
			}
			/* Ignore empty segments (except the first one). */
			if (unlikely((buf->data_len == 0) &&
				     (buf != pkts[i])))
				continue;
			/* Update SGE. */
			elt_buf->bufs[seg_n] = buf;
			sge->addr = (uintptr_t)rte_pktmbuf_mtod(buf, char *);
			if (txq->priv->vf)
				rte_prefetch0((volatile void *)sge->addr);
			sge->length = buf->data_len;
			sge->lkey = lkey;
			sent_size += sge->length;
			/* sent_size may be unused, silence warning. */
			(void)sent_size;
			/* Increase number of segments (SGEs). */
			++seg_n;
		}
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Increment sent bytes counter. */
		txq->stats.obytes += sent_size;
#endif
		if (unlikely(buf != NULL)) {
			DEBUG("too many segments for packet (maximum is %zu)",
			      elemof(elt_wr->sges));
			/* Ignore this packet. */
			++dropped;
			rte_pktmbuf_free(pkts[i]);
			/* Use invalid value for safe rollback. */
			wr->num_sge = 0;
			goto next;
		}
		/* Update WR. */
		wr->num_sge = seg_n;
#if MLX4_PMD_MAX_INLINE > 0
		if (sent_size <= txq->max_inline)
			wr->send_flags |= IBV_SEND_INLINE;
		else
			wr->send_flags &= ~IBV_SEND_INLINE;
#endif
		/* Update WR index. */
		if (unlikely(++elts_cur == txq->elts_n))
			elts_cur = 0;
	next:
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Increase sent packets counter. */
		++txq->stats.opackets;
#endif
		continue;
	}
	/* Take a shortcut if everything was dropped. */
	if (unlikely(dropped == max))
		return 0;
	*wr_next = NULL;
	/* The last WR is the only one asking for a completion event. */
	containerof(wr_next, mlx4_send_wr_t, next)->
		send_flags |= IBV_SEND_SIGNALED;
	/* Make sure all packets have been processed in the previous loop. */
	assert(i == max);
	err = mlx4_post_send(txq->qp, head.next, &bad_wr);
	if (unlikely(err)) {
		unsigned int bad = (containerof(bad_wr, struct txq_wr, wr) -
				    &(*txq->elts_wr)[0]);
		struct txq_wr *bad_elt_wr = &(*txq->elts_wr)[bad];
		struct txq_wr *elt_wr = bad_elt_wr;
		struct txq_buf *bad_elt_buf = &(*txq->elts_buf)[bad];
		struct txq_buf *elt_buf = bad_elt_buf;

		/* Recalculate the number of empty packets dropped from the
		 * previous loop and rewind elt_buf and elt_wr at the same
		 * time. */
		dropped = 0;
		for (i = 0; (elt_buf->comp != NULL); ++i) {
			if (elt_wr->wr.num_sge == 0)
				++dropped;
			elt_buf = elt_buf->comp;
			elt_wr = &(*txq->elts_wr)
				[elt_buf - &(*txq->elts_buf)[0]];
		}
		assert(i < max);
		DEBUG("%p: mlx4_post_send(): failed for WR %p"
		      " (only %u (%u dropped) out of %u WR(s) posted): %s",
		      (void *)txq->priv, (void *)bad_wr, i, dropped, max,
		      ((err <= -1) ? "Internal error" : strerror(err)));
		/* Rollback elts_cur. */
		elts_cur -= (max - i - dropped);
		elts_cur %= txq->elts_n;
		/* Completion event has been lost. Link these elements to the
		 * list of those without a completion event. They will be
		 * processed the next time a completion event is received. */
		assert(elt_buf->comp == NULL);
		if (bad_elt_buf->comp != NULL) {
			elt_buf->comp = txq->lost_comp;
			txq->lost_comp = bad_elt_buf->comp;
		}
		else
			assert(bad_elt_buf == elt_buf);
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Decrement packets and bytes counters for each element that
		 * won't be sent. */
		while (1) {
			--txq->stats.opackets;
			assert(bad_elt_buf->bufs[0] != NULL);
			txq->stats.obytes -= bad_elt_buf->bufs[0]->pkt_len;
			if (bad_elt_wr->wr.next == NULL)
				break;
			bad_elt_wr = containerof(bad_elt_wr->wr.next,
						 struct txq_wr, wr);
			bad_elt_buf = &(*txq->elts_buf)
				[bad_elt_wr - &(*txq->elts_wr)[0]];
		}
#endif
	}
	else
		++txq->elts_comp;
	txq->elts_used += (i - dropped);
	txq->elts_free -= (i - dropped);
	txq->elts_cur = elts_cur;
	txq->stats.odropped += dropped;
	/* Sanity checks. */
	assert(txq->elts_comp <= txq->elts_used);
	assert(txq->elts_used <= txq->elts_n);
	assert(txq->elts_free <= txq->elts_n);
	return i;
}

static int
txq_setup(struct rte_eth_dev *dev, struct txq *txq, uint16_t desc,
	  unsigned int socket, const struct rte_eth_txconf *conf)
{
	struct priv *priv = dev->data->dev_private;
	struct txq tmpl = {
		.priv = priv,
		.socket = socket
	};
	union {
		struct ibv_qp_init_attr init;
		struct ibv_qp_attr mod;
	} attr;
	int ret = 0;

	(void)conf; /* Thresholds configuration (ignored). */
	if ((desc == 0) || (desc % MLX4_PMD_SGE_WR_N)) {
		DEBUG("%p: invalid number of TX descriptors (must be a"
		      " multiple of %d)", (void *)dev, desc);
		return -EINVAL;
	}
	desc /= MLX4_PMD_SGE_WR_N;
	/* MRs will be registered in mp2mr[] later. */
	tmpl.cq = ibv_create_cq(priv->ctx, desc, NULL, NULL, 0);
	if (tmpl.cq == NULL) {
		ret = ENOMEM;
		DEBUG("%p: CQ creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	DEBUG("priv->device_attr.max_qp_wr is %d",
	      priv->device_attr.max_qp_wr);
	DEBUG("priv->device_attr.max_sge is %d",
	      priv->device_attr.max_sge);
	attr.init = (struct ibv_qp_init_attr){
		/* CQ to be associated with the send queue. */
		.send_cq = tmpl.cq,
		/* CQ to be associated with the receive queue. */
		.recv_cq = tmpl.cq,
		.cap = {
			/* Max number of outstanding WRs. */
			.max_send_wr = ((priv->device_attr.max_qp_wr < desc) ?
					priv->device_attr.max_qp_wr :
					desc),
			/* Max number of scatter/gather elements in a WR. */
			.max_send_sge = ((priv->device_attr.max_sge <
					  MLX4_PMD_SGE_WR_N) ?
					 priv->device_attr.max_sge :
					 MLX4_PMD_SGE_WR_N),
#if MLX4_PMD_MAX_INLINE > 0
			.max_inline_data = MLX4_PMD_MAX_INLINE,
#endif
		},
		.qp_type = IBV_QPT_RAW_PACKET,
		/* Do *NOT* enable this, completions events are managed per
		 * TX burst. */
		.sq_sig_all = 0
	};
	tmpl.qp = ibv_create_qp(priv->pd, &attr.init);
	if (tmpl.qp == NULL) {
		ret = errno;
		DEBUG("%p: QP creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
#if MLX4_PMD_MAX_INLINE > 0
	/* ibv_create_qp() updates this value. */
	tmpl.max_inline = attr.init.cap.max_inline_data;
#endif
	attr.mod = (struct ibv_qp_attr){
		/* Move the QP to this state. */
		.qp_state = IBV_QPS_INIT,
		/* Primary port number. */
		.port_num = priv->port
	};
	if ((ret = ibv_modify_qp(tmpl.qp, &attr.mod,
				 (IBV_QP_STATE | IBV_QP_PORT)))) {
		DEBUG("%p: QP state to IBV_QPS_INIT failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	if ((ret = txq_alloc_elts(&tmpl, desc))) {
		DEBUG("%p: TXQ allocation failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.mod = (struct ibv_qp_attr){
		.qp_state = IBV_QPS_RTR
	};
	if ((ret = ibv_modify_qp(tmpl.qp, &attr.mod, IBV_QP_STATE))) {
		DEBUG("%p: QP state to IBV_QPS_RTR failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.mod.qp_state = IBV_QPS_RTS;
	if ((ret = ibv_modify_qp(tmpl.qp, &attr.mod, IBV_QP_STATE))) {
		DEBUG("%p: QP state to IBV_QPS_RTS failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	/* Clean up txq in case we're reinitializing it. */
	DEBUG("%p: cleaning-up old txq just in case", (void *)txq);
	txq_cleanup(txq);
	*txq = tmpl;
	DEBUG("%p: txq updated with %p", (void *)txq, (void *)&tmpl);
	assert(ret == 0);
	return 0;
error:
	txq_cleanup(&tmpl);
	assert(ret != 0);
	return -ret; /* Negative errno value. */
}

static int
mlx4_tx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_txconf *conf)
{
	struct priv *priv = dev->data->dev_private;
	struct txq *txq = (*priv->txqs)[idx];
	int ret;

	priv_lock(priv);
	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if (idx >= priv->txqs_n) {
		DEBUG("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->txqs_n);
		priv_unlock(priv);
		return -EOVERFLOW;
	}
	if (txq != NULL) {
		DEBUG("%p: reusing already allocated queue index %u (%p)",
		      (void *)dev, idx, (void *)txq);
		if (priv->started) {
			priv_unlock(priv);
			return -EEXIST;
		}
		(*priv->txqs)[idx] = NULL;
		txq_cleanup(txq);
	}
	else {
		txq = rte_calloc_socket("TXQ", 1, sizeof(*txq), 0, socket);
		if (txq == NULL) {
			DEBUG("%p: unable to allocate queue index %u: %s",
			      (void *)dev, idx, strerror(errno));
			priv_unlock(priv);
			return -errno;
		}
	}
	ret = txq_setup(dev, txq, desc, socket, conf);
	if (ret)
		rte_free(txq);
	else {
		txq->stats.idx = idx;
		DEBUG("%p: adding TX queue %p to list",
		      (void *)dev, (void *)txq);
		(*priv->txqs)[idx] = txq;
		/* Update send callback. */
		dev->tx_pkt_burst = mlx4_tx_burst;
	}
	priv_unlock(priv);
	return ret;
}

static void
mlx4_tx_queue_release(void *dpdk_txq)
{
	struct txq *txq = (struct txq *)dpdk_txq;
	struct priv *priv;
	unsigned int i;

	if (txq == NULL)
		return;
	priv = txq->priv;
	priv_lock(priv);
	for (i = 0; (i != priv->txqs_n); ++i)
		if ((*priv->txqs)[i] == txq) {
			DEBUG("%p: removing TX queue %p from list",
			      (void *)priv->dev, (void *)txq);
			(*priv->txqs)[i] = NULL;
			break;
		}
	txq_cleanup(txq);
	rte_free(txq);
	priv_unlock(priv);
}

/* RX queues handling. */

static int
rxq_alloc_elts_sp(struct rxq *rxq, unsigned int elts_n)
{
	unsigned int i;
	struct rxq_elt_sp (*elts)[elts_n] =
		rte_calloc_socket("RXQ elements", 1, sizeof(*elts), 0,
				  rxq->socket);
	int ret = 0;

	if (elts == NULL) {
		DEBUG("%p: can't allocate packets array", (void *)rxq);
		ret = ENOMEM;
		goto error;
	}
	/* For each WR (packet). */
	for (i = 0; (i != elts_n); ++i) {
		unsigned int j;
		struct rxq_elt_sp *elt = &(*elts)[i];
		struct ibv_recv_wr *wr = &elt->wr;
		struct ibv_sge (*sges)[(elemof(elt->sges))] = &elt->sges;

		/* These two arrays must have the same size. */
		assert(elemof(elt->sges) == elemof(elt->bufs));
		/* Configure WR. */
		wr->wr_id = i;
		wr->next = &(*elts)[(i + 1)].wr;
		wr->sg_list = &(*sges)[0];
		wr->num_sge = elemof(*sges);
		/* For each SGE (segment). */
		for (j = 0; (j != elemof(elt->bufs)); ++j) {
			struct ibv_sge *sge = &(*sges)[j];
			struct rte_mbuf *buf = rte_pktmbuf_alloc(rxq->mp);

			if (buf == NULL) {
				DEBUG("%p: empty mbuf pool", (void *)rxq);
				ret = ENOMEM;
				goto error;
			}
			elt->bufs[j] = buf;
			/* Headroom is reserved by rte_pktmbuf_alloc(). */
			assert(buf->data_off == RTE_PKTMBUF_HEADROOM);
			/* Buffer is supposed to be empty. */
			assert(rte_pktmbuf_data_len(buf) == 0);
			assert(rte_pktmbuf_pkt_len(buf) == 0);
			/* sge->addr must be able to store a pointer. */
			assert(sizeof(sge->addr) >= sizeof(uintptr_t));
			if (j == 0) {
				/* The first SGE keeps its headroom. */
				sge->addr = (uintptr_t)rte_pktmbuf_mtod(buf,
									char *);
				sge->length = (buf->buf_len -
					       RTE_PKTMBUF_HEADROOM);
			}
			else {
				/* Subsequent SGEs lose theirs. */
				assert(buf->data_off == RTE_PKTMBUF_HEADROOM);
				buf->data_off = 0;
				sge->addr = (uintptr_t)buf->buf_addr;
				sge->length = buf->buf_len;
			}
			sge->lkey = rxq->mr->lkey;
			/* Redundant check for tailroom. */
			assert(sge->length == rte_pktmbuf_tailroom(buf));
		}
	}
	/* The last WR pointer must be NULL. */
	(*elts)[(i - 1)].wr.next = NULL;
	DEBUG("%p: allocated and configured %u WRs (%zu segments)",
	      (void *)rxq, elts_n, (elts_n * elemof((*elts)[0].sges)));
	rxq->elts_n = elts_n;
	rxq->elts.sp = elts;
	assert(ret == 0);
	return 0;
error:
	if (elts != NULL) {
		for (i = 0; (i != elemof(*elts)); ++i) {
			unsigned int j;
			struct rxq_elt_sp *elt = &(*elts)[i];

			for (j = 0; (j != elemof(elt->bufs)); ++j) {
				struct rte_mbuf *buf = elt->bufs[j];

				if (buf != NULL)
					rte_pktmbuf_free_seg(buf);
			}
		}
		rte_free(elts);
	}
	DEBUG("%p: failed, freed everything", (void *)rxq);
	assert(ret != 0);
	return ret;
}

static void
rxq_free_elts_sp(struct rxq *rxq)
{
	unsigned int i;
	unsigned int elts_n = rxq->elts_n;
	struct rxq_elt_sp (*elts)[elts_n] = rxq->elts.sp;

	DEBUG("%p: freeing WRs", (void *)rxq);
	rxq->elts_n = 0;
	rxq->elts.sp = NULL;
	if (elts == NULL)
		return;
	for (i = 0; (i != elemof(*elts)); ++i) {
		unsigned int j;
		struct rxq_elt_sp *elt = &(*elts)[i];

		for (j = 0; (j != elemof(elt->bufs)); ++j) {
			struct rte_mbuf *buf = elt->bufs[j];

			if (buf != NULL)
				rte_pktmbuf_free_seg(buf);
		}
	}
	rte_free(elts);
}

static int
rxq_alloc_elts(struct rxq *rxq, unsigned int elts_n)
{
	unsigned int i;
	struct rxq_elt (*elts)[elts_n] =
		rte_calloc_socket("RXQ elements", 1, sizeof(*elts), 0,
				  rxq->socket);
	int ret = 0;

	if (elts == NULL) {
		DEBUG("%p: can't allocate packets array", (void *)rxq);
		ret = ENOMEM;
		goto error;
	}
	/* For each WR (packet). */
	for (i = 0; (i != elts_n); ++i) {
		struct rxq_elt *elt = &(*elts)[i];
		struct ibv_recv_wr *wr = &elt->wr;
		struct ibv_sge *sge = &(*elts)[i].sge;
		struct rte_mbuf *buf = rte_pktmbuf_alloc(rxq->mp);

		if (buf == NULL) {
			DEBUG("%p: empty mbuf pool", (void *)rxq);
			ret = ENOMEM;
			goto error;
		}
		/* Configure WR. */
		wr->wr_id = (uint64_t)buf;
		wr->next = &(*elts)[(i + 1)].wr;
		wr->sg_list = sge;
		wr->num_sge = 1;
		/* Headroom is reserved by rte_pktmbuf_alloc(). */
		assert(buf->data_off == RTE_PKTMBUF_HEADROOM);
		/* Buffer is supposed to be empty. */
		assert(rte_pktmbuf_data_len(buf) == 0);
		assert(rte_pktmbuf_pkt_len(buf) == 0);
		/* sge->addr must be able to store a pointer. */
		assert(sizeof(sge->addr) >= sizeof(uintptr_t));
		/* SGE keeps its headroom. */
		sge->addr = (uintptr_t)rte_pktmbuf_mtod(buf, char *);
		sge->length = (buf->buf_len - RTE_PKTMBUF_HEADROOM);
		sge->lkey = rxq->mr->lkey;
		/* Redundant check for tailroom. */
		assert(sge->length == rte_pktmbuf_tailroom(buf));
	}
	/* The last WR pointer must be NULL. */
	(*elts)[(i - 1)].wr.next = NULL;
	DEBUG("%p: allocated and configured %u single-segment WRs",
	      (void *)rxq, elts_n);
	rxq->elts_n = elts_n;
	rxq->elts.no_sp = elts;
	assert(ret == 0);
	return 0;
error:
	if (elts != NULL) {
		for (i = 0; (i != elemof(*elts)); ++i) {
			struct rxq_elt *elt = &(*elts)[i];

			if ((struct rte_mbuf *)elt->wr.wr_id != NULL)
				rte_pktmbuf_free_seg((struct rte_mbuf *)
						     elt->wr.wr_id);
		}
		rte_free(elts);
	}
	DEBUG("%p: failed, freed everything", (void *)rxq);
	assert(ret != 0);
	return ret;
}

static void
rxq_free_elts(struct rxq *rxq)
{
	unsigned int i;
	unsigned int elts_n = rxq->elts_n;
	struct rxq_elt (*elts)[elts_n] = rxq->elts.no_sp;

	DEBUG("%p: freeing WRs", (void *)rxq);
	rxq->elts_n = 0;
	rxq->elts.no_sp = NULL;
	if (elts == NULL)
		return;
	for (i = 0; (i != elemof(*elts)); ++i) {
		struct rxq_elt *elt = &(*elts)[i];

		if ((struct rte_mbuf *)elt->wr.wr_id != NULL)
			rte_pktmbuf_free_seg((struct rte_mbuf *)
					     elt->wr.wr_id);
	}
	rte_free(elts);
}

static void
rxq_mac_addr_del(struct rxq *rxq, unsigned int mac_index)
{
#if defined(NDEBUG) || defined(MLX4_COMPAT_VMWARE)
	struct priv *priv = rxq->priv;
	const uint8_t (*mac)[ETHER_ADDR_LEN] =
		(const uint8_t (*)[ETHER_ADDR_LEN])
		priv->mac[mac_index].addr_bytes;
#endif

	assert(mac_index < elemof(priv->mac));
	if (!BITFIELD_ISSET(rxq->mac_configured, mac_index)) {
		assert(rxq->mac_flow[mac_index] == NULL);
		return;
	}
	DEBUG("%p: removing MAC address %02x:%02x:%02x:%02x:%02x:%02x"
	      " index %u",
	      (void *)rxq,
	      (*mac)[0], (*mac)[1], (*mac)[2], (*mac)[3], (*mac)[4], (*mac)[5],
	      mac_index);
#ifdef MLX4_COMPAT_VMWARE
	if (priv->vmware) {
		union ibv_gid gid = { .raw = { 0 } };

		memcpy(&gid.raw[10], *mac, sizeof(*mac));
		claim_zero(ibv_detach_mcast(rxq->qp, &gid, 0));
		BITFIELD_RESET(rxq->mac_configured, mac_index);
		return;
	}
#endif
	assert(rxq->mac_flow[mac_index] != NULL);
	claim_zero(ibv_destroy_flow(rxq->mac_flow[mac_index]));
	rxq->mac_flow[mac_index] = NULL;
	BITFIELD_RESET(rxq->mac_configured, mac_index);
}

static void
rxq_mac_addrs_del(struct rxq *rxq)
{
	struct priv *priv = rxq->priv;
	unsigned int i;

	for (i = 0; (i != elemof(priv->mac)); ++i)
		rxq_mac_addr_del(rxq, i);
}

static int
rxq_mac_addr_add(struct rxq *rxq, unsigned int mac_index)
{
	struct priv *priv = rxq->priv;
	const uint8_t (*mac)[ETHER_ADDR_LEN] =
		(const uint8_t (*)[ETHER_ADDR_LEN])
		priv->mac[mac_index].addr_bytes;
	unsigned int vlans = 0;
	unsigned int specs = 0;
	unsigned int i, j;
	struct ibv_flow *flow;

	assert(mac_index < elemof(priv->mac));
	if (BITFIELD_ISSET(rxq->mac_configured, mac_index))
		rxq_mac_addr_del(rxq, mac_index);
	/* Number of configured VLANs. */
	for (i = 0; (i != elemof(priv->vlan_filter)); ++i)
		if (priv->vlan_filter[i].enabled)
			++vlans;
	specs = (vlans ? vlans : 1);

	/* Allocate flow specification on the stack. */
	struct ibv_flow_attr data[1 +
				  (sizeof(struct ibv_flow_spec_eth[specs]) /
				   sizeof(struct ibv_flow_attr)) +
				  !!(sizeof(struct ibv_flow_spec_eth[specs]) %
				     sizeof(struct ibv_flow_attr))];
	struct ibv_flow_attr *attr = (void *)&data[0];
	struct ibv_flow_spec_eth *spec = (void *)&data[1];

	/*
	 * No padding must be inserted by the compiler between attr and spec.
	 * This layout is expected by libibverbs.
	 */
	assert(((uint8_t *)attr + sizeof(*attr)) == (uint8_t *)spec);
	*attr = (struct ibv_flow_attr){
		.type = IBV_FLOW_ATTR_NORMAL,
		.num_of_specs = specs,
		.port = priv->port,
		.flags = 0
	};
	*spec = (struct ibv_flow_spec_eth){
		.type = IBV_FLOW_SPEC_ETH,
		.size = sizeof(*spec),
		.val = {
			.dst_mac = {
				(*mac)[0], (*mac)[1], (*mac)[2],
				(*mac)[3], (*mac)[4], (*mac)[5]
			}
		},
		.mask = {
			.dst_mac = "\xff\xff\xff\xff\xff\xff",
			.vlan_tag = (vlans ? 0xfff : 0)
		}
	};
	/* Fill VLAN specifications. */
	for (i = 0, j = 0; (i != elemof(priv->vlan_filter)); ++i) {
		if (!priv->vlan_filter[i].enabled)
			continue;
		assert(j != vlans);
		if (j)
			spec[j] = spec[0];
		spec[j].val.vlan_tag = priv->vlan_filter[i].id;
		++j;
	}
	DEBUG("%p: adding MAC address %02x:%02x:%02x:%02x:%02x:%02x index %u"
	      " (%u VLAN(s) configured)",
	      (void *)rxq,
	      (*mac)[0], (*mac)[1], (*mac)[2], (*mac)[3], (*mac)[4], (*mac)[5],
	      mac_index,
	      vlans);
#ifdef MLX4_COMPAT_VMWARE
	if (priv->vmware) {
		union ibv_gid gid = { .raw = { 0 } };

		/* Call multicast attach with unicast mac to get traffic. */
		memcpy(&gid.raw[10], *mac, sizeof(*mac));
		errno = 0;
		if (ibv_attach_mcast(rxq->qp, &gid, 0)) {
			if (errno)
				return errno;
			return EINVAL;
		}
		BITFIELD_SET(rxq->mac_configured, mac_index);
		return 0;
	}
#endif
	/* Create related flow. */
	errno = 0;
	if ((flow = ibv_create_flow(rxq->qp, attr)) == NULL) {
		/* It's not clear whether errno is always set in this case. */
		DEBUG("%p: flow configuration failed, errno=%d: %s",
		      (void *)rxq, errno,
		      (errno ? strerror(errno) : "Unknown error"));
		if (errno)
			return errno;
		return EINVAL;
	}
	assert(rxq->mac_flow[mac_index] == NULL);
	rxq->mac_flow[mac_index] = flow;
	BITFIELD_SET(rxq->mac_configured, mac_index);
	return 0;
}

static int
rxq_mac_addrs_add(struct rxq *rxq)
{
	struct priv *priv = rxq->priv;
	unsigned int i;
	int ret;

	for (i = 0; (i != elemof(priv->mac)); ++i) {
		if (!BITFIELD_ISSET(priv->mac_configured, i))
			continue;
		ret = rxq_mac_addr_add(rxq, i);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			rxq_mac_addr_del(rxq, --i);
		return ret;
	}
	return 0;
}

static void
priv_mac_addr_del(struct priv *priv, unsigned int mac_index)
{
	unsigned int i;

	assert(mac_index < elemof(priv->mac));
	if (!BITFIELD_ISSET(priv->mac_configured, mac_index))
		return;
	if (priv->rss) {
		rxq_mac_addr_del(&priv->rxq_parent, mac_index);
		goto end;
	}
	for (i = 0; (i != priv->dev->data->nb_rx_queues); ++i)
		rxq_mac_addr_del((*priv->rxqs)[i], mac_index);
end:
	BITFIELD_RESET(priv->mac_configured, mac_index);
}

static int
priv_mac_addr_add(struct priv *priv, unsigned int mac_index,
		  const uint8_t (*mac)[ETHER_ADDR_LEN])
{
	unsigned int i;
	int ret;

	assert(mac_index < elemof(priv->mac));
	/* First, make sure this address isn't already configured. */
	for (i = 0; (i != elemof(priv->mac)); ++i) {
		/* Skip this index, it's going to be reconfigured. */
		if (i == mac_index)
			continue;
		if (!BITFIELD_ISSET(priv->mac_configured, i))
			continue;
		if (memcmp(priv->mac[i].addr_bytes, *mac, sizeof(*mac)))
			continue;
		/* Address already configured elsewhere, return with error. */
		return EADDRINUSE;
	}
	if (BITFIELD_ISSET(priv->mac_configured, mac_index))
		priv_mac_addr_del(priv, mac_index);
	priv->mac[mac_index] = (struct ether_addr){
		{
			(*mac)[0], (*mac)[1], (*mac)[2],
			(*mac)[3], (*mac)[4], (*mac)[5]
		}
	};
	/* If device isn't started, this is all we need to do. */
	if (!priv->started) {
#ifndef NDEBUG
		/* Verify that all queues have this index disabled. */
		for (i = 0; (i != priv->rxqs_n); ++i) {
			if ((*priv->rxqs)[i] == NULL)
				continue;
			assert(!BITFIELD_ISSET
			       ((*priv->rxqs)[i]->mac_configured, mac_index));
		}
#endif
		goto end;
	}
	if (priv->rss) {
		ret = rxq_mac_addr_add(&priv->rxq_parent, mac_index);
		if (ret)
			return ret;
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		ret = rxq_mac_addr_add((*priv->rxqs)[i], mac_index);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			if ((*priv->rxqs)[(--i)] != NULL)
				rxq_mac_addr_del((*priv->rxqs)[i], mac_index);
		return ret;
	}
end:
	BITFIELD_SET(priv->mac_configured, mac_index);
	return 0;
}

static int
rxq_allmulticast_enable(struct rxq *rxq)
{
	struct ibv_flow *flow;
	struct ibv_flow_attr attr = {
		.type = IBV_FLOW_ATTR_MC_DEFAULT,
		.num_of_specs = 0,
		.port = rxq->priv->port,
		.flags = 0
	};

#ifdef MLX4_COMPAT_VMWARE
	if (rxq->priv->vmware) {
		DEBUG("%p: allmulticast mode is not supported in VMware",
		      (void *)rxq);
		return EINVAL;
	}
#endif
	DEBUG("%p: enabling allmulticast mode", (void *)rxq);
	if (rxq->allmulti_flow != NULL)
		return EBUSY;
	errno = 0;
	if ((flow = ibv_create_flow(rxq->qp, &attr)) == NULL) {
		/* It's not clear whether errno is always set in this case. */
		DEBUG("%p: flow configuration failed, errno=%d: %s",
		      (void *)rxq, errno,
		      (errno ? strerror(errno) : "Unknown error"));
		if (errno)
			return errno;
		return EINVAL;
	}
	rxq->allmulti_flow = flow;
	DEBUG("%p: allmulticast mode enabled", (void *)rxq);
	return 0;
}

static void
rxq_allmulticast_disable(struct rxq *rxq)
{
#ifdef MLX4_COMPAT_VMWARE
	if (rxq->priv->vmware) {
		DEBUG("%p: allmulticast mode is not supported in VMware",
		      (void *)rxq);
		return;
	}
#endif
	DEBUG("%p: disabling allmulticast mode", (void *)rxq);
	if (rxq->allmulti_flow == NULL)
		return;
	claim_zero(ibv_destroy_flow(rxq->allmulti_flow));
	rxq->allmulti_flow = NULL;
	DEBUG("%p: allmulticast mode disabled", (void *)rxq);
}

static int
rxq_promiscuous_enable(struct rxq *rxq)
{
	struct ibv_flow *flow;
	struct ibv_flow_attr attr = {
		/*
		 * XXX IBV_FLOW_ATTR_ALL_DEFAULT is used in place of
		 * IBV_FLOW_ATTR_SNIFFER because the latter doesn't work
		 * and triggers kernel Oopses in this version:
		 * mlnx-ofa_kernel-2.0-OFED.2.0.0.2.1.ga62cf7e
		 */
		.type = IBV_FLOW_ATTR_ALL_DEFAULT,
		.num_of_specs = 0,
		.port = rxq->priv->port,
		.flags = 0
	};

#ifdef MLX4_COMPAT_VMWARE
	if (rxq->priv->vmware) {
		DEBUG("%p: promiscuous mode is not supported in VMware",
		      (void *)rxq);
		return EINVAL;
	}
#endif
	DEBUG("%p: enabling promiscuous mode", (void *)rxq);
	if (rxq->promisc_flow != NULL)
		return EBUSY;
	errno = 0;
	if ((flow = ibv_create_flow(rxq->qp, &attr)) == NULL) {
		/* It's not clear whether errno is always set in this case. */
		DEBUG("%p: flow configuration failed, errno=%d: %s",
		      (void *)rxq, errno,
		      (errno ? strerror(errno) : "Unknown error"));
		if (errno)
			return errno;
		return EINVAL;
	}
	rxq->promisc_flow = flow;
	DEBUG("%p: promiscuous mode enabled", (void *)rxq);
	return 0;
}

static void
rxq_promiscuous_disable(struct rxq *rxq)
{
#ifdef MLX4_COMPAT_VMWARE
	if (rxq->priv->vmware) {
		DEBUG("%p: promiscuous mode is not supported in VMware",
		      (void *)rxq);
		return;
	}
#endif
	DEBUG("%p: disabling promiscuous mode", (void *)rxq);
	if (rxq->promisc_flow == NULL)
		return;
	claim_zero(ibv_destroy_flow(rxq->promisc_flow));
	rxq->promisc_flow = NULL;
	DEBUG("%p: promiscuous mode disabled", (void *)rxq);
}

static void
rxq_cleanup(struct rxq *rxq)
{
	DEBUG("cleaning up %p", (void *)rxq);
	if (rxq->sp)
		rxq_free_elts_sp(rxq);
	else
		rxq_free_elts(rxq);
	if (rxq->qp != NULL) {
		rxq_promiscuous_disable(rxq);
		rxq_allmulticast_disable(rxq);
		rxq_mac_addrs_del(rxq);
		claim_zero(ibv_destroy_qp(rxq->qp));
	}
	if (rxq->cq != NULL)
		claim_zero(ibv_destroy_cq(rxq->cq));
	if (rxq->mr != NULL)
		claim_zero(ibv_dereg_mr(rxq->mr));
	memset(rxq, 0, sizeof(*rxq));
}

static uint16_t
mlx4_rx_burst(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n);

static uint16_t
mlx4_rx_burst_sp(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct rxq_elt_sp (*elts)[rxq->elts_n] = rxq->elts.sp;
	struct ibv_wc wcs[pkts_n];
	struct ibv_recv_wr head;
	struct ibv_recv_wr **next = &head.next;
	struct ibv_recv_wr *bad_wr;
	int ret = 0;
	int wcs_n;
	int i;

	if (unlikely(!rxq->sp))
		return mlx4_rx_burst(dpdk_rxq, pkts, pkts_n);
	if (unlikely(elts == NULL)) /* See RTE_DEV_CMD_SET_MTU. */
		return 0;
	wcs_n = ibv_poll_cq(rxq->cq, pkts_n, wcs);
	if (unlikely(wcs_n == 0))
		return 0;
	if (unlikely(wcs_n < 0)) {
		DEBUG("rxq=%p, ibv_poll_cq() failed (wc_n=%d)",
		      (void *)rxq, wcs_n);
		return -1;
	}
	assert(wcs_n <= (int)pkts_n);
	/* For each work completion. */
	for (i = 0; (i != wcs_n); ++i) {
		struct ibv_wc *wc = &wcs[i];
		uint64_t wr_id = wc->wr_id;
		uint32_t len = wc->byte_len;
		struct rxq_elt_sp *elt = &(*elts)[wr_id];
		struct ibv_recv_wr *wr = &elt->wr;
		struct rte_mbuf *pkt_buf = NULL; /* Buffer returned in pkts. */
		struct rte_mbuf **pkt_buf_next = &pkt_buf;
		unsigned int j = 0;

		/* Sanity checks. */
		assert(wr_id < rxq->elts_n);
		assert(wr_id == wr->wr_id);
		assert(wr->sg_list == elt->sges);
		assert(wr->num_sge == elemof(elt->sges));
		/* Link completed WRs together for repost. */
		*next = wr;
		next = &wr->next;
		if (unlikely(wc->status != IBV_WC_SUCCESS)) {
			/* Whatever, just repost the offending WR. */
			DEBUG("rxq=%p, wr_id=%" PRIu64 ": bad work completion"
			      " status (%d): %s",
			      (void *)rxq, wc->wr_id, wc->status,
			      ibv_wc_status_str(wc->status));
#ifdef MLX4_PMD_SOFT_COUNTERS
			/* Increase dropped packets counter. */
			++rxq->stats.idropped;
#endif
			goto repost;
		}
		/*
		 * Replace spent segments with new ones, concatenate and
		 * return them as pkt_buf.
		 */
		while (1) {
			struct ibv_sge *sge = &elt->sges[j];
			struct rte_mbuf *seg = elt->bufs[j];
			struct rte_mbuf *rep;
			uint32_t seg_headroom;
			uint32_t seg_tailroom;

			/*
			 * Fetch initial bytes of packet descriptor into a
			 * cacheline while allocating rep.
			 */
			rte_prefetch0(seg);
			rep = __rte_mbuf_raw_alloc(rxq->mp);
			if (unlikely(rep == NULL)) {
				/*
				 * Unable to allocate a replacement mbuf,
				 * repost WR.
				 */
				DEBUG("rxq=%p, wr_id=%" PRIu64 ":"
				      " can't allocate a new mbuf",
				      (void *)rxq, wr_id);
				if (pkt_buf != NULL)
					rte_pktmbuf_free(pkt_buf);
				/* Increase out of memory counters. */
				++rxq->stats.rx_nombuf;
				++rxq->priv->dev->data->rx_mbuf_alloc_failed;
				goto repost;
			}
#ifndef NDEBUG
			else {
				/* assert() checks below need this. */
				rep->data_len = 0;
			}
#endif
			seg_headroom = seg->data_off;
			seg_tailroom = (seg->buf_len - seg_headroom);
			assert(seg_tailroom == rte_pktmbuf_tailroom(seg));
			/* Only the first segment comes with headroom. */
			assert((j == 0) ?
			       (seg_headroom == RTE_PKTMBUF_HEADROOM) :
			       (seg_headroom == 0));
			rep->data_off = seg_headroom;
			assert(rte_pktmbuf_headroom(rep) == seg_headroom);
			assert(rte_pktmbuf_tailroom(rep) == seg_tailroom);
			/* Reconfigure sge to use rep instead of seg. */
			sge->addr = (uintptr_t)rte_pktmbuf_mtod(rep, char *);
			assert(sge->length == seg_tailroom);
			assert(sge->lkey == rxq->mr->lkey);
			elt->bufs[j] = rep;
			++j;
			/* Update pkt_buf if it's the first segment, or link
			 * seg to the previous one and update pkt_buf_next. */
			*pkt_buf_next = seg;
			pkt_buf_next = &seg->next;
			/* Update seg information. */
			seg->nb_segs = 1;
			seg->in_port = rxq->port_id;
			if (likely(len <= seg_tailroom)) {
				/* Last segment. */
				seg->data_len = len;
				seg->pkt_len = len;
				break;
			}
			seg->data_len = seg_tailroom;
			seg->pkt_len = seg_tailroom;
			len -= seg_tailroom;
		}
		/* Update head and tail segments. */
		*pkt_buf_next = NULL;
		assert(pkt_buf != NULL);
		assert(j != 0);
		pkt_buf->nb_segs = j;
		pkt_buf->pkt_len = wc->byte_len;
		pkt_buf->ol_flags = 0;

		/* Return packet. */
		*(pkts++) = pkt_buf;
		++ret;
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Increase bytes counter. */
		rxq->stats.ibytes += wc->byte_len;
#endif
	repost:
		continue;
	}
	*next = NULL;
	/* Repost WRs. */
#ifdef DEBUG_RECV
	DEBUG("%p: reposting %d WRs starting from %" PRIu64 " (%p)",
	      (void *)rxq, wcs_n, wcs[0].wr_id, (void *)head.next);
#endif
	i = ibv_post_recv(rxq->qp, head.next, &bad_wr);
	if (unlikely(i)) {
		/* Inability to repost WRs is fatal. */
		DEBUG("%p: ibv_post_recv(): failed for WR %p: %s",
		      (void *)rxq->priv,
		      (void *)bad_wr,
		      strerror(i));
		abort();
	}
#ifdef MLX4_PMD_SOFT_COUNTERS
	/* Increase packets counter. */
	rxq->stats.ipackets += ret;
#endif
	return ret;
}

/*
 * The following function is the same as mlx4_rx_burst_sp(), except it doesn't
 * manage scattered packets. Improves performance when MRU is lower than the
 * size of the first segment.
 */
static uint16_t
mlx4_rx_burst(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct rxq_elt (*elts)[rxq->elts_n] = rxq->elts.no_sp;
	struct ibv_wc wcs[pkts_n];
	struct ibv_recv_wr head;
	struct ibv_recv_wr **next = &head.next;
	struct ibv_recv_wr *bad_wr;
	int ret = 0;
	int wcs_n;
	int i;

	if (unlikely(rxq->sp))
		return mlx4_rx_burst_sp(dpdk_rxq, pkts, pkts_n);
	wcs_n = ibv_poll_cq(rxq->cq, pkts_n, wcs);
	if (unlikely(wcs_n == 0))
		return 0;
	if (unlikely(wcs_n < 0)) {
		DEBUG("rxq=%p, ibv_poll_cq() failed (wc_n=%d)",
		      (void *)rxq, wcs_n);
		return -1;
	}
	assert(wcs_n <= (int)pkts_n);
	/* For each work completion. */
	for (i = 0; (i != wcs_n); ++i) {
		struct ibv_wc *wc = &wcs[i];
		uint32_t len = wc->byte_len;
		struct rxq_elt *elt = &(*elts)[i];
		struct ibv_recv_wr *wr = &elt->wr;
		struct rte_mbuf *seg = (struct rte_mbuf *)wc->wr_id;
		struct rte_mbuf *rep;

		/* Sanity checks. */
		assert(wr->sg_list == &elt->sge);
		assert(wr->num_sge == 1);
		/* Link completed WRs together for repost. */
		*next = wr;
		next = &wr->next;
		if (unlikely(wc->status != IBV_WC_SUCCESS)) {
			/* Whatever, just repost the offending WR. */
			DEBUG("rxq=%p, wr_id=%" PRIu64 ": bad work completion"
			      " status (%d): %s",
			      (void *)rxq, wc->wr_id, wc->status,
			      ibv_wc_status_str(wc->status));
#ifdef MLX4_PMD_SOFT_COUNTERS
			/* Increase dropped packets counter. */
			++rxq->stats.idropped;
#endif
			goto repost;
		}
		/*
		 * Fetch initial bytes of packet descriptor into a
		 * cacheline while allocating rep.
		 */
		rte_prefetch0(seg);
		rep = __rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(rep == NULL)) {
			/*
			 * Unable to allocate a replacement mbuf,
			 * repost WR.
			 */
			DEBUG("rxq=%p, wr_id=%" PRIu64 ":"
			      " can't allocate a new mbuf",
			      (void *)rxq, wc->wr_id);
			/* Increase out of memory counters. */
			++rxq->stats.rx_nombuf;
			++rxq->priv->dev->data->rx_mbuf_alloc_failed;
			goto repost;
		}

		/* Reconfigure sge to use rep instead of seg. */
		elt->sge.addr = (uintptr_t)rep->buf_addr + RTE_PKTMBUF_HEADROOM;
		assert(elt->sge.lkey == rxq->mr->lkey);
		wr->wr_id = (uint64_t)rep;

		/* Update seg information. */
		seg->data_off = RTE_PKTMBUF_HEADROOM;
		seg->nb_segs = 1;
		seg->in_port = rxq->port_id;
		seg->next = NULL;
		seg->pkt_len = len;
		seg->data_len = len;
		seg->ol_flags = 0;

		/* Return packet. */
		*(pkts++) = seg;
		++ret;
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Increase bytes counter. */
		rxq->stats.ibytes += wc->byte_len;
#endif
	repost:
		continue;
	}
	*next = NULL;
	/* Repost WRs. */
#ifdef DEBUG_RECV
	DEBUG("%p: reposting %d WRs starting from %" PRIu64 " (%p)",
	      (void *)rxq, wcs_n, wcs[0].wr_id, (void *)head.next);
#endif
	i = ibv_post_recv(rxq->qp, head.next, &bad_wr);
	if (unlikely(i)) {
		/* Inability to repost WRs is fatal. */
		DEBUG("%p: ibv_post_recv(): failed for WR %p: %s",
		      (void *)rxq->priv,
		      (void *)bad_wr,
		      strerror(i));
		abort();
	}
#ifdef MLX4_PMD_SOFT_COUNTERS
	/* Increase packets counter. */
	rxq->stats.ipackets += ret;
#endif
	return ret;
}

static struct ibv_qp *
rxq_setup_qp(struct priv *priv, struct ibv_cq *cq, uint16_t desc)
{
	struct ibv_qp_init_attr attr = {
		/* CQ to be associated with the send queue. */
		.send_cq = cq,
		/* CQ to be associated with the receive queue. */
		.recv_cq = cq,
		.cap = {
			/* Max number of outstanding WRs. */
			.max_recv_wr = ((priv->device_attr.max_qp_wr < desc) ?
					priv->device_attr.max_qp_wr :
					desc),
			/* Max number of scatter/gather elements in a WR. */
			.max_recv_sge = ((priv->device_attr.max_sge <
					  MLX4_PMD_SGE_WR_N) ?
					 priv->device_attr.max_sge :
					 MLX4_PMD_SGE_WR_N),
		},
		.qp_type = IBV_QPT_RAW_PACKET
	};

	return ibv_create_qp(priv->pd, &attr);
}

#if RSS_SUPPORT

static struct ibv_qp *
rxq_setup_qp_rss(struct priv *priv, struct ibv_cq *cq, uint16_t desc,
		 int parent)
{
	struct ibv_exp_qp_init_attr attr = {
		/* CQ to be associated with the send queue. */
		.send_cq = cq,
		/* CQ to be associated with the receive queue. */
		.recv_cq = cq,
		.cap = {
			/* Max number of outstanding WRs. */
			.max_recv_wr = ((priv->device_attr.max_qp_wr < desc) ?
					priv->device_attr.max_qp_wr :
					desc),
			/* Max number of scatter/gather elements in a WR. */
			.max_recv_sge = ((priv->device_attr.max_sge <
					  MLX4_PMD_SGE_WR_N) ?
					 priv->device_attr.max_sge :
					 MLX4_PMD_SGE_WR_N),
		},
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask = (IBV_EXP_QP_INIT_ATTR_PD |
			      IBV_EXP_QP_INIT_ATTR_QPG),
		.pd = priv->pd
	};

	if (parent) {
		attr.qpg.qpg_type = IBV_QPG_PARENT;
		/* TSS isn't necessary. */
		attr.qpg.parent_attrib.tss_child_count = 0;
		attr.qpg.parent_attrib.rss_child_count = priv->rxqs_n;
		DEBUG("initializing parent RSS queue");
	}
	else {
		attr.qpg.qpg_type = IBV_QPG_CHILD_RX;
		attr.qpg.qpg_parent = priv->rxq_parent.qp;
		DEBUG("initializing child RSS queue");
	}
	return ibv_exp_create_qp(priv->ctx, &attr);
}

#endif /* RSS_SUPPORT */

static int
rxq_setup(struct rte_eth_dev *dev, struct rxq *rxq, uint16_t desc,
	  unsigned int socket, const struct rte_eth_rxconf *conf,
	  struct rte_mempool *mp)
{
	struct priv *priv = dev->data->dev_private;
	struct rxq tmpl = {
		.priv = priv,
		.mp = mp,
		.socket = socket
	};
	struct ibv_qp_attr mod;
	struct ibv_recv_wr *bad_wr;
	struct rte_mbuf *buf;
	int ret = 0;
	int parent = (rxq == &priv->rxq_parent);

	(void)conf; /* Thresholds configuration (ignored). */
	/*
	 * If this is a parent queue, hardware must support RSS and
	 * RSS must be enabled.
	 */
	assert((!parent) || ((priv->hw_rss) && (priv->rss)));
	if (parent) {
		/* Even if unused, ibv_create_cq() requires at least one
		 * descriptor. */
		desc = 1;
		goto skip_mr;
	}
	if ((desc == 0) || (desc % MLX4_PMD_SGE_WR_N)) {
		DEBUG("%p: invalid number of RX descriptors (must be a"
		      " multiple of %d)", (void *)dev, desc);
		return -EINVAL;
	}
	/* Get mbuf length. */
	buf = rte_pktmbuf_alloc(mp);
	if (buf == NULL) {
		DEBUG("%p: unable to allocate mbuf", (void *)dev);
		return -ENOMEM;
	}
	tmpl.mb_len = buf->buf_len;
	assert((rte_pktmbuf_headroom(buf) +
		rte_pktmbuf_tailroom(buf)) == tmpl.mb_len);
	assert(rte_pktmbuf_headroom(buf) == RTE_PKTMBUF_HEADROOM);
	rte_pktmbuf_free(buf);
	/*
	 * Depending on mb_len, jumbo frames support and MRU, enable scattered
	 * packets support for this queue.
	 */
	if ((dev->data->dev_conf.rxmode.jumbo_frame) &&
	    (dev->data->dev_conf.rxmode.max_rx_pkt_len >
	     (tmpl.mb_len - RTE_PKTMBUF_HEADROOM))) {
		tmpl.sp = 1;
		desc /= MLX4_PMD_SGE_WR_N;
	}
	/* Try to increase MTU if lower than desired MRU. */
	if (priv->mtu < dev->data->dev_conf.rxmode.max_rx_pkt_len) {
		uint16_t mtu = dev->data->dev_conf.rxmode.max_rx_pkt_len;

		if (priv_set_mtu(priv, mtu) == 0) {
			DEBUG("adapter port %u MTU increased to %u",
			      priv->port, mtu);
			priv->mtu = mtu;
		}
		else
			DEBUG("unable to set port %u MTU to %u: %s",
			      priv->port, mtu, strerror(errno));
	}
	DEBUG("%p: %s scattered packets support (%u WRs)",
	      (void *)dev, (tmpl.sp ? "enabling" : "disabling"), desc);
	/* Get mempool size. */
	tmpl.mp_size = mp_total_size(mp);
	/* Use the entire RX mempool as the memory region. */
	tmpl.mr = ibv_reg_mr(priv->pd, mp, tmpl.mp_size,
			     (IBV_ACCESS_LOCAL_WRITE |
			      IBV_ACCESS_REMOTE_WRITE));
	if (tmpl.mr == NULL) {
		ret = ENOMEM;
		DEBUG("%p: MR creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
skip_mr:
	tmpl.cq = ibv_create_cq(priv->ctx, desc, NULL, NULL, 0);
	if (tmpl.cq == NULL) {
		ret = ENOMEM;
		DEBUG("%p: CQ creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	DEBUG("priv->device_attr.max_qp_wr is %d",
	      priv->device_attr.max_qp_wr);
	DEBUG("priv->device_attr.max_sge is %d",
	      priv->device_attr.max_sge);
#if RSS_SUPPORT
	if (priv->rss)
		tmpl.qp = rxq_setup_qp_rss(priv, tmpl.cq, desc, parent);
	else
#endif /* RSS_SUPPORT */
		tmpl.qp = rxq_setup_qp(priv, tmpl.cq, desc);
	if (tmpl.qp == NULL) {
		ret = errno;
		DEBUG("%p: QP creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	mod = (struct ibv_qp_attr){
		/* Move the QP to this state. */
		.qp_state = IBV_QPS_INIT,
		/* Primary port number. */
		.port_num = priv->port
	};
	if ((ret = ibv_modify_qp(tmpl.qp, &mod,
				 (IBV_QP_STATE |
#if RSS_SUPPORT
				  (parent ? IBV_QP_GROUP_RSS : 0) |
#endif /* RSS_SUPPORT */
				  IBV_QP_PORT)))) {
		DEBUG("%p: QP state to IBV_QPS_INIT failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	if ((parent) || (!priv->rss))  {
		/* Configure MAC and broadcast addresses. */
		if ((ret = rxq_mac_addrs_add(&tmpl))) {
			DEBUG("%p: QP flow attachment failed: %s",
			      (void *)dev, strerror(ret));
			goto error;
		}
	}
	/* Allocate descriptors for RX queues, except for the RSS parent. */
	if (parent)
		goto skip_alloc;
	if (tmpl.sp)
		ret = rxq_alloc_elts_sp(&tmpl, desc);
	else
		ret = rxq_alloc_elts(&tmpl, desc);
	if (ret) {
		DEBUG("%p: RXQ allocation failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	if ((ret = ibv_post_recv(tmpl.qp,
				 (tmpl.sp ?
				  &(*tmpl.elts.sp)[0].wr :
				  &(*tmpl.elts.no_sp)[0].wr),
				 &bad_wr))) {
		DEBUG("%p: ibv_post_recv() failed for WR %p: %s",
		      (void *)dev,
		      (void *)bad_wr,
		      strerror(ret));
		goto error;
	}
skip_alloc:
	mod = (struct ibv_qp_attr){
		.qp_state = IBV_QPS_RTR
	};
	if ((ret = ibv_modify_qp(tmpl.qp, &mod, IBV_QP_STATE))) {
		DEBUG("%p: QP state to IBV_QPS_RTR failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	/* Save port ID. */
	tmpl.port_id = dev->data->port_id;
	DEBUG("%p: RTE port ID: %u", (void *)rxq, tmpl.port_id);
	/* Clean up rxq in case we're reinitializing it. */
	DEBUG("%p: cleaning-up old rxq just in case", (void *)rxq);
	rxq_cleanup(rxq);
	*rxq = tmpl;
	DEBUG("%p: rxq updated with %p", (void *)rxq, (void *)&tmpl);
	assert(ret == 0);
	return 0;
error:
	rxq_cleanup(&tmpl);
	assert(ret != 0);
	return -ret; /* Negative errno value. */
}

static int
mlx4_rx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_rxconf *conf,
		    struct rte_mempool *mp)
{
	struct priv *priv = dev->data->dev_private;
	struct rxq *rxq = (*priv->rxqs)[idx];
	int ret;

	priv_lock(priv);
	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if (idx >= priv->rxqs_n) {
		DEBUG("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->rxqs_n);
		priv_unlock(priv);
		return -EOVERFLOW;
	}
	if (rxq != NULL) {
		DEBUG("%p: reusing already allocated queue index %u (%p)",
		      (void *)dev, idx, (void *)rxq);
		if (priv->started) {
			priv_unlock(priv);
			return -EEXIST;
		}
		(*priv->rxqs)[idx] = NULL;
		rxq_cleanup(rxq);
	}
	else {
		rxq = rte_calloc_socket("RXQ", 1, sizeof(*rxq), 0, socket);
		if (rxq == NULL) {
			DEBUG("%p: unable to allocate queue index %u: %s",
			      (void *)dev, idx, strerror(errno));
			priv_unlock(priv);
			return -errno;
		}
	}
	ret = rxq_setup(dev, rxq, desc, socket, conf, mp);
	if (ret)
		rte_free(rxq);
	else {
		rxq->stats.idx = idx;
		DEBUG("%p: adding RX queue %p to list",
		      (void *)dev, (void *)rxq);
		(*priv->rxqs)[idx] = rxq;
		/* Update receive callback. */
		if (rxq->sp)
			dev->rx_pkt_burst = mlx4_rx_burst_sp;
		else
			dev->rx_pkt_burst = mlx4_rx_burst;
	}
	priv_unlock(priv);
	return ret;
}

static void
mlx4_rx_queue_release(void *dpdk_rxq)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct priv *priv;
	unsigned int i;

	if (rxq == NULL)
		return;
	priv = rxq->priv;
	priv_lock(priv);
	assert(rxq != &priv->rxq_parent);
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] == rxq) {
			DEBUG("%p: removing RX queue %p from list",
			      (void *)priv->dev, (void *)rxq);
			(*priv->rxqs)[i] = NULL;
			break;
		}
	rxq_cleanup(rxq);
	rte_free(rxq);
	priv_unlock(priv);
}

/* Simulate device start by attaching all configured flows. */
static int
mlx4_dev_start(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i = 0;
	unsigned int r;
	struct rxq *rxq;

	priv_lock(priv);
	if (priv->started) {
		priv_unlock(priv);
		return 0;
	}
	DEBUG("%p: attaching configured flows to all RX queues", (void *)dev);
	priv->started = 1;
	if (priv->rss) {
		rxq = &priv->rxq_parent;
		r = 1;
	}
	else {
		rxq = (*priv->rxqs)[0];
		r = priv->rxqs_n;
	}
	/* Iterate only once when RSS is enabled. */
	do {
		int ret;

		/* Ignore nonexistent RX queues. */
		if (rxq == NULL)
			continue;
		if (((ret = rxq_mac_addrs_add(rxq)) == 0) &&
		    ((!priv->promisc) ||
		     ((ret = rxq_promiscuous_enable(rxq)) == 0)) &&
		    ((!priv->allmulti) ||
		     ((ret = rxq_allmulticast_enable(rxq)) == 0)))
			continue;
		DEBUG("%p: QP flow attachment failed: %s",
		      (void *)dev, strerror(ret));
		/* Rollback. */
		while (i != 0)
			if ((rxq = (*priv->rxqs)[--i]) != NULL) {
				rxq_allmulticast_disable(rxq);
				rxq_promiscuous_disable(rxq);
				rxq_mac_addrs_del(rxq);
			}
		priv->started = 0;
		return -1;
	}
	while ((--r) && ((rxq = (*priv->rxqs)[++i]), i));
	priv_unlock(priv);
	return 0;
}

/* Simulate device stop by detaching all configured flows. */
static void
mlx4_dev_stop(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i = 0;
	unsigned int r;
	struct rxq *rxq;

	priv_lock(priv);
	if (!priv->started) {
		priv_unlock(priv);
		return;
	}
	DEBUG("%p: detaching flows from all RX queues", (void *)dev);
	priv->started = 0;
	if (priv->rss) {
		rxq = &priv->rxq_parent;
		r = 1;
	}
	else {
		rxq = (*priv->rxqs)[0];
		r = priv->rxqs_n;
	}
	/* Iterate only once when RSS is enabled. */
	do {
		/* Ignore nonexistent RX queues. */
		if (rxq == NULL)
			continue;
		rxq_allmulticast_disable(rxq);
		rxq_promiscuous_disable(rxq);
		rxq_mac_addrs_del(rxq);
	}
	while ((--r) && ((rxq = (*priv->rxqs)[++i]), i));
	priv_unlock(priv);
}

static uint16_t
removed_tx_burst(void *dpdk_txq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	(void)dpdk_txq;
	(void)pkts;
	(void)pkts_n;
	return 0;
}

static uint16_t
removed_rx_burst(void *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	(void)dpdk_rxq;
	(void)pkts;
	(void)pkts_n;
	return 0;
}

static void
mlx4_dev_close(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	void *tmp;
	unsigned int i;

	priv_lock(priv);
	DEBUG("%p: closing device \"%s\"",
	      (void *)dev,
	      ((priv->ctx != NULL) ? priv->ctx->device->name : ""));
	/* Prevent crashes when queues are still in use. This is unfortunately
	 * still required for DPDK 1.3 because some programs (such as testpmd)
	 * never release them before closing the device. */
	dev->rx_pkt_burst = removed_rx_burst;
	dev->tx_pkt_burst = removed_tx_burst;
	if (priv->rxqs != NULL) {
		/* XXX race condition if mlx4_rx_burst() is still running. */
		usleep(1000);
		for (i = 0; (i != priv->rxqs_n); ++i) {
			tmp = (*priv->rxqs)[i];
			if (tmp == NULL)
				continue;
			(*priv->rxqs)[i] = NULL;
			rxq_cleanup(tmp);
			rte_free(tmp);
		}
		priv->rxqs_n = 0;
		priv->rxqs = NULL;
	}
	if (priv->txqs != NULL) {
		/* XXX race condition if mlx4_tx_burst() is still running. */
		usleep(1000);
		for (i = 0; (i != priv->txqs_n); ++i) {
			tmp = (*priv->txqs)[i];
			if (tmp == NULL)
				continue;
			(*priv->txqs)[i] = NULL;
			txq_cleanup(tmp);
			rte_free(tmp);
		}
		priv->txqs_n = 0;
		priv->txqs = NULL;
	}
	if (priv->rss)
		rxq_cleanup(&priv->rxq_parent);
	if (priv->pd != NULL) {
		assert(priv->ctx != NULL);
		claim_zero(ibv_dealloc_pd(priv->pd));
		claim_zero(ibv_close_device(priv->ctx));
	}
	else
		assert(priv->ctx == NULL);
	priv_unlock(priv);
	memset(priv, 0, sizeof(*priv));
}

static void
mlx4_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *info)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int max;

	priv_lock(priv);
	/* FIXME: we should ask the device for these values. */
	info->min_rx_bufsize = 32;
	info->max_rx_pktlen = 65536;
	/*
	 * Since we need one CQ per QP, the limit is the minimum number
	 * between the two values.
	 */
	max = ((priv->device_attr.max_cq > priv->device_attr.max_qp) ?
	       priv->device_attr.max_qp : priv->device_attr.max_cq);
	/* If max >= 65535 then max = 0, max_rx_queues is uint16_t. */
	if (max >= 65535) {
		max = 65535;
	}
	info->max_rx_queues = max;
	info->max_tx_queues = max;
	info->max_mac_addrs = elemof(priv->mac);
	priv_unlock(priv);
}

static void
mlx4_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct priv *priv = dev->data->dev_private;
	struct rte_eth_stats tmp = { .ipackets = 0 };
	unsigned int i;
	unsigned int idx;

	priv_lock(priv);
	/* Add software counters. */
	for (i = 0; (i != priv->rxqs_n); ++i) {
		struct rxq *rxq = (*priv->rxqs)[i];

		if (rxq == NULL)
			continue;
		idx = rxq->stats.idx;
		if (idx < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			tmp.q_ipackets[idx] += rxq->stats.ipackets;
			tmp.q_ibytes[idx] += rxq->stats.ibytes;
			tmp.q_errors[idx] += (rxq->stats.idropped +
					      rxq->stats.rx_nombuf);
		}
		tmp.ipackets += rxq->stats.ipackets;
		tmp.ibytes += rxq->stats.ibytes;
		tmp.ierrors += rxq->stats.idropped;
		tmp.rx_nombuf += rxq->stats.rx_nombuf;
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		struct txq *txq = (*priv->txqs)[i];

		if (txq == NULL)
			continue;
		idx = txq->stats.idx;
		if (idx < RTE_ETHDEV_QUEUE_STAT_CNTRS) {
			tmp.q_opackets[idx] += txq->stats.opackets;
			tmp.q_obytes[idx] += txq->stats.obytes;
			tmp.q_errors[idx] += txq->stats.odropped;
		}
		tmp.opackets += txq->stats.opackets;
		tmp.obytes += txq->stats.obytes;
		tmp.oerrors += txq->stats.odropped;
	}
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: retrieve and add hardware counters. */
#endif
	*stats = tmp;
	priv_unlock(priv);
}

static void
mlx4_stats_reset(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	unsigned int idx;

	priv_lock(priv);
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		idx = (*priv->rxqs)[i]->stats.idx;
		(*priv->rxqs)[i]->stats =
			(struct mlx4_rxq_stats){ .idx = idx };
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		if ((*priv->txqs)[i] == NULL)
			continue;
		idx = (*priv->rxqs)[i]->stats.idx;
		(*priv->txqs)[i]->stats =
			(struct mlx4_txq_stats){ .idx = idx };
	}
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: reset hardware counters. */
#endif
	priv_unlock(priv);
}

static void
mlx4_mac_addr_remove(struct rte_eth_dev *dev, uint32_t index)
{
	struct priv *priv = dev->data->dev_private;

	priv_lock(priv);
	DEBUG("%p: removing MAC address from index %" PRIu32,
	      (void *)dev, index);
	if (index >= MLX4_MAX_MAC_ADDRESSES)
		goto end;
	/* Refuse to remove the broadcast address, this one is special. */
	if (!memcmp(priv->mac[index].addr_bytes, "\xff\xff\xff\xff\xff\xff",
		    ETHER_ADDR_LEN))
		goto end;
	priv_mac_addr_del(priv, index);
end:
	priv_unlock(priv);
}

static void
mlx4_mac_addr_add(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
		  uint32_t index, uint32_t vmdq)
{
	struct priv *priv = dev->data->dev_private;

	(void)vmdq;
	priv_lock(priv);
	DEBUG("%p: adding MAC address at index %" PRIu32,
	      (void *)dev, index);
	if (index >= MLX4_MAX_MAC_ADDRESSES)
		goto end;
	/* Refuse to add the broadcast address, this one is special. */
	if (!memcmp(mac_addr->addr_bytes, "\xff\xff\xff\xff\xff\xff",
		    ETHER_ADDR_LEN))
		goto end;
	priv_mac_addr_add(priv, index,
			  (const uint8_t (*)[ETHER_ADDR_LEN])
			  mac_addr->addr_bytes);
end:
	priv_unlock(priv);
}

static void
mlx4_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	int ret;

	priv_lock(priv);
	if (priv->promisc) {
		priv_unlock(priv);
		return;
	}
	/* If device isn't started, this is all we need to do. */
	if (!priv->started)
		goto end;
	if (priv->rss) {
		ret = rxq_promiscuous_enable(&priv->rxq_parent);
		if (ret) {
			priv_unlock(priv);
			return;
		}
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		ret = rxq_promiscuous_enable((*priv->rxqs)[i]);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			if ((*priv->rxqs)[--i] != NULL)
				rxq_promiscuous_disable((*priv->rxqs)[i]);
		priv_unlock(priv);
		return;
	}
end:
	priv->promisc = 1;
	priv_unlock(priv);
}

static void
mlx4_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;

	priv_lock(priv);
	if (!priv->promisc) {
		priv_unlock(priv);
		return;
	}
	if (priv->rss) {
		rxq_promiscuous_disable(&priv->rxq_parent);
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] != NULL)
			rxq_promiscuous_disable((*priv->rxqs)[i]);
end:
	priv->promisc = 0;
	priv_unlock(priv);
}

static void
mlx4_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	int ret;

	priv_lock(priv);
	if (priv->allmulti) {
		priv_unlock(priv);
		return;
	}
	/* If device isn't started, this is all we need to do. */
	if (!priv->started)
		goto end;
	if (priv->rss) {
		ret = rxq_allmulticast_enable(&priv->rxq_parent);
		if (ret) {
			priv_unlock(priv);
			return;
		}
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		ret = rxq_allmulticast_enable((*priv->rxqs)[i]);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			if ((*priv->rxqs)[--i] != NULL)
				rxq_allmulticast_disable((*priv->rxqs)[i]);
		priv_unlock(priv);
		return;
	}
end:
	priv->allmulti = 1;
	priv_unlock(priv);
}

static void
mlx4_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;

	priv_lock(priv);
	if (!priv->allmulti) {
		priv_unlock(priv);
		return;
	}
	if (priv->rss) {
		rxq_allmulticast_disable(&priv->rxq_parent);
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] != NULL)
			rxq_allmulticast_disable((*priv->rxqs)[i]);
end:
	priv->allmulti = 0;
	priv_unlock(priv);
}

static int
mlx4_link_update_unlocked(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct priv *priv = dev->data->dev_private;
	struct ibv_port_attr port_attr;
	static const uint8_t width_mult[] = {
		/* Multiplier values taken from devinfo.c in libibverbs. */
		0, 1, 4, 0, 8, 0, 0, 0, 12, 0
	};

	(void)wait_to_complete;
	if ((errno = ibv_query_port(priv->ctx, priv->port, &port_attr))) {
		DEBUG("port query failed: %s", strerror(errno));
		return -1;
	}
	dev->data->dev_link = (struct rte_eth_link){
		.link_speed = (ibv_rate_to_mbps(mult_to_ibv_rate
						(port_attr.active_speed)) *
			       width_mult[(port_attr.active_width %
					   sizeof(width_mult))]),
		.link_duplex = ETH_LINK_FULL_DUPLEX,
		.link_status = (port_attr.state == IBV_PORT_ACTIVE)
	};
	if (memcmp(&port_attr, &priv->port_attr, sizeof(port_attr))) {
		/* Link status changed. */
		priv->port_attr = port_attr;
		return 0;
	}
	/* Link status is still the same. */
	return -1;
}

static int
mlx4_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	ret = mlx4_link_update_unlocked(dev, wait_to_complete);
	priv_unlock(priv);
	return ret;
}

static int
mlx4_dev_get_mtu(struct rte_eth_dev *dev, uint16_t *mtu)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	if (priv_get_mtu(priv, mtu)) {
		ret = errno;
		goto out;
	}
	priv->mtu = *mtu;
	ret = 0;
out:
	priv_unlock(priv);
	return ret;
}

static int
mlx4_dev_set_mtu(struct rte_eth_dev *dev, uint16_t *mtu)
{
	struct priv *priv = dev->data->dev_private;
	int ret;
	unsigned int i;
	int ok;

	priv_lock(priv);
	if (priv_set_mtu(priv, *mtu)) {
		ret = errno;
		goto out;
	}
	dev->rx_pkt_burst = removed_rx_burst;
	dev->data->dev_conf.rxmode.jumbo_frame = (*mtu > ETHER_MAX_LEN);
	dev->data->dev_conf.rxmode.max_rx_pkt_len = *mtu;
	/* Make sure everyone has left mlx4_rx_burst(). */
	rte_wmb();
	usleep(1000);
	/* Reconfigure each RX queue. */
	ok = 0;
	for (i = 0; (i != priv->rxqs_n); ++i) {
		struct rxq *rxq = (*priv->rxqs)[i];
		uint16_t desc;
		unsigned int socket;
		struct rte_eth_rxconf *conf;
		struct rte_mempool *mp;

		if (rxq == NULL)
			continue;
		desc = (rxq->elts_n *
			(rxq->sp ? MLX4_PMD_SGE_WR_N : 1));
		socket = rxq->socket;
		conf = NULL; /* Currently unused. */
		mp = rxq->mp;
		rxq_cleanup(rxq);
		if (rxq_setup(dev, rxq, desc, socket, conf, mp)) {
			/* This queue is now dead, with no way to
			 * recover. Just prevent mlx4_rx_burst() from
			 * crashing during the next call by enabling
			 * SP mode. mlx4_rx_burst_sp() has an
			 * additional check for this case. */
			rxq->sp = 1;
			continue;
		}
		/* Reenable non-RSS queue attributes. No need to check
		 * for errors at this stage. */
		if (!priv->rss) {
			rxq_mac_addrs_add(rxq);
			if (priv->promisc)
				rxq_promiscuous_enable(rxq);
			if (priv->allmulti)
				rxq_allmulticast_enable(rxq);
		}
		if (!rxq->sp) {
			if (*mtu <= (rxq->mb_len - RTE_PKTMBUF_HEADROOM))
				ok |= 1;
		}
		else {
			if (*mtu <= ((rxq->mb_len * MLX4_PMD_SGE_WR_N) -
				     RTE_PKTMBUF_HEADROOM))
				ok |= 2;
		}
	}
	/* Burst functions can now be called again. */
	if (ok & 2)
		dev->rx_pkt_burst = mlx4_rx_burst_sp;
	else
		dev->rx_pkt_burst = mlx4_rx_burst;
	if (!ok) {
		ret = EINVAL;
		goto out;
	}
	priv->mtu = *mtu;
	ret = 0;

out:
	priv_unlock(priv);
	return ret;
}

static int
mlx4_dev_get_flow_ctrl(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct ifreq ifr;
	struct ethtool_pauseparam ethpause;
	int ret;

	ifr.ifr_data = &ethpause;
	ethpause.cmd = ETHTOOL_GPAUSEPARAM;
	priv_lock(priv);
	if (priv_ifreq(priv, SIOCETHTOOL, &ifr)) {
		ret = errno;
		DEBUG("ioctl(SIOCETHTOOL, ETHTOOL_GPAUSEPARAM)"
		      " failed: %s",
		      strerror(errno));
		goto out;
	}

	fc_conf->autoneg = ethpause.autoneg;
	if (ethpause.rx_pause && ethpause.tx_pause)
		fc_conf->mode = RTE_FC_FULL;
	else if (ethpause.rx_pause)
		fc_conf->mode = RTE_FC_RX_PAUSE;
	else if (ethpause.tx_pause)
		fc_conf->mode = RTE_FC_TX_PAUSE;
	else
		fc_conf->mode = RTE_FC_NONE;
	ret = 0;

out:
	priv_unlock(priv);
	return ret;
}

static int
mlx4_dev_set_flow_ctrl(struct rte_eth_dev *dev, struct rte_eth_fc_conf *fc_conf)
{
	struct priv *priv = dev->data->dev_private;
	struct ifreq ifr;
	struct ethtool_pauseparam ethpause;
	int ret;

	ifr.ifr_data = &ethpause;
	ethpause.autoneg = fc_conf->autoneg;

	if ((fc_conf->mode & RTE_FC_FULL) || (fc_conf->mode & RTE_FC_RX_PAUSE))
		ethpause.rx_pause = 1;
	else
		ethpause.rx_pause = 0;

	if ((fc_conf->mode & RTE_FC_FULL) || (fc_conf->mode & RTE_FC_TX_PAUSE))
		ethpause.tx_pause = 1;
	else
		ethpause.tx_pause = 0;

	priv_lock(priv);
	if (priv_ifreq(priv, SIOCETHTOOL, &ifr)) {
		ret = errno;
		DEBUG("ioctl(SIOCETHTOOL, ETHTOOL_SPAUSEPARAM)"
		      " failed: %s",
		      strerror(errno));
		goto out;
	}
	ret = 0;

out:
	priv_unlock(priv);
	return ret;
}

static int
vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	unsigned int j = -1;

	DEBUG("%p: %s VLAN filter ID %" PRIu16,
	      (void *)dev, (on ? "enable" : "disable"), vlan_id);
	for (i = 0; (i != elemof(priv->vlan_filter)); ++i) {
		if (!priv->vlan_filter[i].enabled) {
			/* Unused index, remember it. */
			j = i;
			continue;
		}
		if (priv->vlan_filter[i].id != vlan_id)
			continue;
		/* This VLAN ID is already known, use its index. */
		j = i;
		break;
	}
	/* Check if there's room for another VLAN filter. */
	if (j == (unsigned int)-1)
		return -ENOMEM;
	/*
	 * VLAN filters apply to all configured MAC addresses, flow
	 * specifications must be reconfigured accordingly.
	 */
	priv->vlan_filter[j].id = vlan_id;
	if ((on) && (!priv->vlan_filter[j].enabled)) {
		/*
		 * Filter is disabled, enable it.
		 * Rehashing flows in all RX queues is necessary.
		 */
		if (priv->rss)
			rxq_mac_addrs_del(&priv->rxq_parent);
		else
			for (i = 0; (i != priv->rxqs_n); ++i)
				if ((*priv->rxqs)[i] != NULL)
					rxq_mac_addrs_del((*priv->rxqs)[i]);
		priv->vlan_filter[j].enabled = 1;
		if (priv->started) {
			if (priv->rss)
				rxq_mac_addrs_add(&priv->rxq_parent);
			else
				for (i = 0; (i != priv->rxqs_n); ++i) {
					if ((*priv->rxqs)[i] == NULL)
						continue;
					rxq_mac_addrs_add((*priv->rxqs)[i]);
				}
		}
	}
	else if ((!on) && (priv->vlan_filter[j].enabled)) {
		/*
		 * Filter is enabled, disable it.
		 * Rehashing flows in all RX queues is necessary.
		 */
		if (priv->rss)
			rxq_mac_addrs_del(&priv->rxq_parent);
		else
			for (i = 0; (i != priv->rxqs_n); ++i)
				if ((*priv->rxqs)[i] != NULL)
					rxq_mac_addrs_del((*priv->rxqs)[i]);
		priv->vlan_filter[j].enabled = 0;
		if (priv->started) {
			if (priv->rss)
				rxq_mac_addrs_add(&priv->rxq_parent);
			else
				for (i = 0; (i != priv->rxqs_n); ++i) {
					if ((*priv->rxqs)[i] == NULL)
						continue;
					rxq_mac_addrs_add((*priv->rxqs)[i]);
				}
		}
	}
	return 0;
}

static int
mlx4_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	struct priv *priv = dev->data->dev_private;
	int ret;

	priv_lock(priv);
	ret = vlan_filter_set(dev, vlan_id, on);
	priv_unlock(priv);
	return ret;
}

static struct eth_dev_ops mlx4_dev_ops = {
	.dev_configure = mlx4_dev_configure,
	.dev_start = mlx4_dev_start,
	.dev_stop = mlx4_dev_stop,
	.dev_close = mlx4_dev_close,
	.promiscuous_enable = mlx4_promiscuous_enable,
	.promiscuous_disable = mlx4_promiscuous_disable,
	.allmulticast_enable = mlx4_allmulticast_enable,
	.allmulticast_disable = mlx4_allmulticast_disable,
	.link_update = mlx4_link_update,
	.stats_get = mlx4_stats_get,
	.stats_reset = mlx4_stats_reset,
	.queue_stats_mapping_set = NULL,
	.dev_infos_get = mlx4_dev_infos_get,
	.vlan_filter_set = mlx4_vlan_filter_set,
	.vlan_tpid_set = NULL,
	.vlan_strip_queue_set = NULL,
	.vlan_offload_set = NULL,
	.rx_queue_setup = mlx4_rx_queue_setup,
	.tx_queue_setup = mlx4_tx_queue_setup,
	.rx_queue_release = mlx4_rx_queue_release,
	.tx_queue_release = mlx4_tx_queue_release,
	.dev_led_on = NULL,
	.dev_led_off = NULL,
	.flow_ctrl_get = mlx4_dev_get_flow_ctrl,
	.flow_ctrl_set = mlx4_dev_set_flow_ctrl,
	.priority_flow_ctrl_set = NULL,
	.mac_addr_remove = mlx4_mac_addr_remove,
	.mac_addr_add = mlx4_mac_addr_add,
	.mtu_get = mlx4_dev_get_mtu,
	.mtu_set = mlx4_dev_set_mtu,
	.fdir_add_signature_filter = NULL,
	.fdir_update_signature_filter = NULL,
	.fdir_remove_signature_filter = NULL,
	.fdir_add_perfect_filter = NULL,
	.fdir_update_perfect_filter = NULL,
	.fdir_remove_perfect_filter = NULL,
	.fdir_set_masks = NULL
};

/* Get PCI information from struct ibv_device, return nonzero on error. */
static int
mlx4_ibv_device_to_pci_addr(const struct ibv_device *device,
			    struct rte_pci_addr *pci_addr)
{
	FILE *file = NULL;
	size_t len = 0;
	int ret;
	char line[32];

	do {
		char path[len + 1];

		ret = snprintf(path, (len + 1), "%s/device/uevent",
			       device->ibdev_path);
		if (ret <= 0)
			return errno;
		if (len == 0) {
			len = ret;
			continue;
		}
		file = fopen(path, "rb");
		if (file == NULL)
			return errno;
		break;
	}
	while (1);
	while (fgets(line, sizeof(line), file) == line) {
		/* Truncate long lines. */
		len = strlen(line);
		if (len == (sizeof(line) - 1))
			while (line[(len - 1)] != '\n') {
				ret = fgetc(file);
				if (ret == EOF)
					break;
				line[(len - 1)] = ret;
			}
		/* Extract information. */
		if (sscanf(line,
			   "PCI_SLOT_NAME="
			   "%" SCNx16 ":%" SCNx8 ":%" SCNx8 ".%" SCNx8 "\n",
			   &pci_addr->domain,
			   &pci_addr->bus,
			   &pci_addr->devid,
			   &pci_addr->function) == 4) {
			ret = 0;
			break;
		}
		ret = ENODEV;
	}
	fclose(file);
	return -ret;
}

/* Derive MAC address from port GID. */
static void
mac_from_gid(uint8_t (*mac)[ETHER_ADDR_LEN], uint32_t port, uint8_t *gid)
{
	memcpy(&(*mac)[0], gid + 8, 3);
	memcpy(&(*mac)[3], gid + 13, 3);
	if (port == 1)
		(*mac)[0] ^= 2;
}

/* Support up to 32 adapters. */
static struct {
	struct rte_pci_addr pci_addr; /* associated PCI address */
	uint32_t ports; /* physical ports bitfield. */
} mlx4_dev[32];

/* Return mlx4_dev[] index, or -1 on error. */
static int
mlx4_dev_idx(struct rte_pci_addr *pci_addr)
{
	unsigned int i;
	int ret = -1;

	assert(pci_addr != NULL);
	for (i = 0; (i != elemof(mlx4_dev)); ++i) {
		if ((mlx4_dev[i].pci_addr.domain == pci_addr->domain) &&
		    (mlx4_dev[i].pci_addr.bus == pci_addr->bus) &&
		    (mlx4_dev[i].pci_addr.devid == pci_addr->devid) &&
		    (mlx4_dev[i].pci_addr.function == pci_addr->function))
			return i;
		if ((mlx4_dev[i].ports == 0) && (ret == -1))
			ret = i;
	}
	return ret;
}

static int
mlx4_getenv_int(const char *name)
{
	const char *val = getenv(name);

	if (val == NULL)
		return 0;
	return atoi(val);
}

static struct eth_driver mlx4_driver;

static int
mlx4_pci_devinit(struct rte_pci_driver *pci_drv, struct rte_pci_device *pci_dev)
{
	struct ibv_device **list;
	struct ibv_device *ibv_dev;
	int err = errno;
	struct ibv_context *attr_ctx = NULL;
	struct ibv_device_attr device_attr;
	unsigned int vf;
	int idx;
	int i;

	(void)pci_drv;
	assert(pci_drv == &mlx4_driver.pci_drv);
	/* Get mlx4_dev[] index. */
	idx = mlx4_dev_idx(&pci_dev->addr);
	if (idx == -1) {
		DEBUG("this driver cannot support any more adapters");
		return -ENOMEM;
	}
	DEBUG("using driver device index %d", idx);

	/* Save PCI address. */
	mlx4_dev[idx].pci_addr = pci_dev->addr;
	list = ibv_get_device_list(&i);
	if (list == NULL) {
		assert(errno);
		return -errno;
	}
	assert(i >= 0);
	/*
	 * For each listed device, check related sysfs entry against
	 * the provided PCI ID.
	 */
	while (i != 0) {
		struct rte_pci_addr pci_addr;

		--i;
		DEBUG("checking device \"%s\"", list[i]->name);
		if (mlx4_ibv_device_to_pci_addr(list[i], &pci_addr))
			continue;
		if ((pci_dev->addr.domain != pci_addr.domain) ||
		    (pci_dev->addr.bus != pci_addr.bus) ||
		    (pci_dev->addr.devid != pci_addr.devid) ||
		    (pci_dev->addr.function != pci_addr.function))
			continue;
		vf = (pci_dev->id.device_id ==
		      PCI_DEVICE_ID_MELLANOX_CONNECTX3VF);
		DEBUG("PCI information matches, using device \"%s\" (VF: %s)",
		      list[i]->name, (vf ? "true" : "false"));
		attr_ctx = ibv_open_device(list[i]);
		err = errno;
		break;
	}
	if (attr_ctx == NULL) {
		ibv_free_device_list(list);
		if (err == 0)
			err = ENODEV;
		errno = err;
		return -err;
	}
	ibv_dev = list[i];

	DEBUG("device opened");
	if (ibv_query_device(attr_ctx, &device_attr))
		goto error;
	DEBUG("%u port(s) detected", device_attr.phys_port_cnt);

	for (i = 0; i < device_attr.phys_port_cnt; i++) {
		uint32_t port = i + 1; /* ports are indexed from one */
		uint32_t test = (1 << i);
		struct ibv_context *ctx = NULL;
		struct ibv_port_attr port_attr;
		struct ibv_pd *pd = NULL;
		struct priv *priv = NULL;
		struct rte_eth_dev *eth_dev;
#if RSS_SUPPORT
		struct ibv_exp_device_attr exp_device_attr = {
			.comp_mask = (IBV_EXP_DEVICE_ATTR_FLAGS2 |
				      IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ)
		};
#endif /* RSS_SUPPORT */
		struct ether_addr mac;
		union ibv_gid temp_gid;

		DEBUG("using port %u (%08" PRIx32 ")", port, test);

		ctx = ibv_open_device(ibv_dev);
		if (ctx == NULL)
			goto port_error;

		/* Check port status. */
		if ((errno = ibv_query_port(ctx, port, &port_attr))) {
			DEBUG("port query failed: %s", strerror(errno));
			goto port_error;
		}
		if (port_attr.state != IBV_PORT_ACTIVE)
			DEBUG("bad state for port %d: \"%s\" (%d)",
			      port, ibv_port_state_str(port_attr.state),
			      port_attr.state);

		/* Allocate protection domain. */
		pd = ibv_alloc_pd(ctx);
		if (pd == NULL) {
			DEBUG("PD allocation failure");
			errno = ENOMEM;
			goto port_error;
		}

		mlx4_dev[idx].ports |= test;

		/* from rte_ethdev.c */
		priv = rte_zmalloc("ethdev private structure",
		                   sizeof(*priv),
		                   CACHE_LINE_SIZE);
		if (priv == NULL) {
			DEBUG("priv allocation failure");
			errno = ENOMEM;
			goto port_error;
		}

		priv->ctx = ctx;
		priv->device_attr = device_attr;
		priv->port_attr = port_attr;
		priv->port = port;
		priv->pd = pd;
		priv->mtu = ETHER_MTU;
#if RSS_SUPPORT
		if (ibv_exp_query_device(ctx, &exp_device_attr)) {
			DEBUG("experimental ibv_exp_query_device");
			goto port_error;
		}
		if ((exp_device_attr.device_cap_flags2 & IBV_EXP_DEVICE_QPG) &&
		    (exp_device_attr.device_cap_flags2 & IBV_EXP_DEVICE_UD_RSS) &&
		    (exp_device_attr.comp_mask & IBV_EXP_DEVICE_ATTR_RSS_TBL_SZ) &&
		    (exp_device_attr.max_rss_tbl_sz > 0)) {
			priv->hw_qpg = 1;
			priv->hw_rss = 1;
			priv->max_rss_tbl_sz = exp_device_attr.max_rss_tbl_sz;
		}
		else {
			priv->hw_qpg = 0;
			priv->hw_rss = 0;
			priv->max_rss_tbl_sz = 0;
		}
		priv->hw_tss = !!(exp_device_attr.device_cap_flags2 &
				  IBV_EXP_DEVICE_UD_TSS);
		DEBUG("device flags: %s%s%s",
		      (priv->hw_qpg ? "IBV_DEVICE_QPG " : ""),
		      (priv->hw_tss ? "IBV_DEVICE_TSS " : ""),
		      (priv->hw_rss ? "IBV_DEVICE_RSS " : ""));
		if (priv->hw_rss)
			DEBUG("maximum RSS indirection table size: %u",
			      exp_device_attr.max_rss_tbl_sz);
#endif /* RSS_SUPPORT */
#ifdef MLX4_COMPAT_VMWARE
		if (mlx4_getenv_int("MLX4_COMPAT_VMWARE"))
			priv->vmware = 1;
#else /* MLX4_COMPAT_VMWARE */
		(void)mlx4_getenv_int;
#endif /* MLX4_COMPAT_VMWARE */
		priv->vf = vf;
		if (ibv_query_gid(ctx, port, 0, &temp_gid)) {
			DEBUG("ibv_query_gid() failure");
			goto port_error;
		}
		/* Configure the first MAC address by default. */
		mac_from_gid(&mac.addr_bytes, port, temp_gid.raw);
		DEBUG("port %u MAC address is %02x:%02x:%02x:%02x:%02x:%02x",
		      priv->port,
		      mac.addr_bytes[0], mac.addr_bytes[1],
		      mac.addr_bytes[2], mac.addr_bytes[3],
		      mac.addr_bytes[4], mac.addr_bytes[5]);
		/* Register MAC and broadcast addresses. */
		claim_zero(priv_mac_addr_add(priv, 0,
					     (const uint8_t (*)[ETHER_ADDR_LEN])
					     mac.addr_bytes));
		claim_zero(priv_mac_addr_add(priv, 1,
					     &(const uint8_t [ETHER_ADDR_LEN])
					     { "\xff\xff\xff\xff\xff\xff" }));
#ifndef NDEBUG
		{
			char ifname[IF_NAMESIZE];

			if (priv_get_ifname(priv, &ifname) == 0)
				DEBUG("port %u ifname is \"%s\"", priv->port, ifname);
			else
				DEBUG("port %u ifname is unknown", priv->port);
		}
#endif
		/* Get actual MTU if possible. */
		priv_get_mtu(priv, &priv->mtu);
		DEBUG("port %u MTU is %u", priv->port, priv->mtu);

		/* from rte_ethdev.c */
		eth_dev = rte_eth_dev_allocate();
		if (eth_dev == NULL) {
			DEBUG("can not allocate rte ethdev");
			errno = ENOMEM;
			goto port_error;
		}

		eth_dev->data->dev_private = priv;
		eth_dev->pci_dev = pci_dev;
		eth_dev->driver = &mlx4_driver;
		eth_dev->data->rx_mbuf_alloc_failed = 0;
		eth_dev->data->max_frame_size = ETHER_MAX_LEN;

		priv->dev = eth_dev;
		eth_dev->dev_ops = &mlx4_dev_ops;
		eth_dev->data->mac_addrs = priv->mac;

		continue;

port_error:
		if (priv)
			rte_free(priv);
		if (pd)
			claim_zero(ibv_dealloc_pd(pd));
		if (ctx)
			claim_zero(ibv_close_device(ctx));
		break;
	}

	/*
	 * XXX if something went wrong in the loop above, there is a resource
	 * leak (ctx, pd, priv, dpdk ethdev) but we can do nothing about it as
	 * long as the dpdk does not provide a way to deallocate a ethdev and a
	 * way to enumerate the registered ethdevs to free the previous ones.
	 */

	/* no port found, complain */
	if (!mlx4_dev[idx].ports) {
		errno = ENODEV;
		goto error;
	}

error:
	err = errno;
	if (attr_ctx)
		claim_zero(ibv_close_device(attr_ctx));
	if (list)
		ibv_free_device_list(list);
	errno = err;
	return -err;
}

static struct rte_pci_id mlx4_pci_id_map[] = {
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX3,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX3PRO,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX3VF,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = 0
	}
};

static struct eth_driver mlx4_driver = {
	.pci_drv = {
		.name = MLX4_DRIVER_NAME,
		.id_table = mlx4_pci_id_map,
		.devinit = mlx4_pci_devinit,
	},
	.dev_private_size = sizeof(struct priv)
};

/* Shared object initializer. */
static void __attribute__((constructor))
mlx4_pmd_init(void)
{
	rte_eal_pci_register(&mlx4_driver.pci_drv);
}
