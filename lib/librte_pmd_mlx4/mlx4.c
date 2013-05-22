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
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <arpa/inet.h>

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
#include <rte_version.h>
#include <rte_prefetch.h>
#include <rte_malloc.h>
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

/* Force this to 0 for libibverbs versions lacking RSS support. */
#ifndef RSS_SUPPORT
#define RSS_SUPPORT 1
#endif

/* PMD header. */
#include "mlx4.h"

#ifndef MLX4_PMD_SOFT_COUNTERS
#error Hardware counters not implemented, MLX4_PMD_SOFT_COUNTERS is required.
#endif

/* 6WIND/Intel DPDK compatibility. */
#ifndef DPDK_6WIND

struct rte_rxq_stats {
	uint64_t ipackets;  /**< Total of successfully received packets. */
	uint64_t ibytes;    /**< Total of successfully received bytes. */
	uint64_t idropped;  /**< Total of packets dropped when RX ring full. */
	uint64_t rx_nombuf; /**< Total of RX mbuf allocation failures. */
};

struct rte_txq_stats {
	uint64_t opackets; /**< Total of successfully sent packets. */
	uint64_t obytes;   /**< Total of successfully sent bytes. */
	uint64_t odropped; /**< Total of packets not sent when TX ring full. */
};

#endif /* DPDK_6WIND */

#ifndef DPDK_VERSION
#define DPDK_VERSION(x, y, z) ((x) << 16 | (y) << 8 | (z))
#endif /* DPDK_VERSION */

#ifndef BUILT_DPDK_VERSION
#define BUILT_DPDK_VERSION \
	DPDK_VERSION(RTE_VER_MAJOR, RTE_VER_MINOR, RTE_VER_PATCH_LEVEL)
#endif /* BUILT_DPDK_VERSION */

#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)
typedef struct igb_tx_queue dpdk_txq_t;
typedef struct igb_rx_queue dpdk_rxq_t;
#else /* BUILT_DPDK_VERSION */
typedef void dpdk_txq_t;
typedef void dpdk_rxq_t;
#endif /* BUILT_DPDK_VERSION */

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
	struct ibv_recv_wr wr; /* Work Requesst. */
	struct ibv_sge sge; /* Scatter/Gather Element. */
	struct rte_mbuf *buf; /* SGE buffer. */
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
	struct rte_rxq_stats stats; /* RX queue counters. */
	unsigned int socket; /* CPU socket ID for allocations. */
};

/* TX element. */
struct txq_elt {
	struct ibv_send_wr wr; /* Work Request. */
	struct ibv_sge sges[MLX4_PMD_SGE_WR_N]; /* Scatter/Gather Elements. */
	struct rte_mbuf *bufs[MLX4_PMD_SGE_WR_N]; /* SGEs buffers. */
	struct txq_elt *comp; /* Elements with the same completion event. */
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
	unsigned int elts_n; /* (*elts)[] length. */
	struct txq_elt (*elts)[]; /* TX elements. */
	unsigned int elts_cur; /* Current index in (*elts)[]. */
	unsigned int elts_comp; /* Number of WRs waiting for completion. */
	unsigned int elts_used; /* Number of used WRs (including elts_comp). */
	unsigned int elts_free; /* Number of free WRs. */
	struct txq_elt *lost_comp; /* Elements without a completion event. */
	struct rte_txq_stats stats; /* TX queue counters. */
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
	/* RX/TX queues. */
	struct rxq rxq_parent; /* Parent queue when RSS is enabled. */
	unsigned int rxqs_n; /* RX queues array size. */
	unsigned int txqs_n; /* TX queues array size. */
	struct rxq *(*rxqs)[]; /* RX queues. */
	struct txq *(*txqs)[]; /* TX queues. */
};

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

#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)

static int
mlx4_dev_configure(struct rte_eth_dev *dev)
{
	return dev_configure(dev);
}

#else /* BUILT_DPDK_VERSION */

static int
mlx4_dev_configure(struct rte_eth_dev *dev, uint16_t rxqs_n, uint16_t txqs_n)
{
	struct priv *priv = dev->data->dev_private;
	struct rxq *(*rxqs)[rxqs_n] = NULL;
	struct txq *(*txqs)[txqs_n] = NULL;

	if ((rxqs_n == priv->rxqs_n) && (txqs_n == priv->txqs_n))
		return 0;
	/* Changing the number of RX or TX queues with DPDK < 1.3.0 is
	 * unsafe, thus not supported. */
	if ((priv->rxqs_n) || (priv->txqs_n)) {
		DEBUG("%p: changing the number of RX or TX queues is"
		      " not supported",
		      (void *)dev);
		return -EEXIST;
	}
	assert(priv->rxqs == NULL);
	assert(priv->txqs == NULL);
	/* Allocate arrays. */
	if (((rxqs = rte_calloc("RXQs", 1, sizeof(*rxqs), 0)) == NULL) ||
	    ((txqs = rte_calloc("TXQs", 1, sizeof(*txqs), 0)) == NULL)) {
		DEBUG("%p: unable to allocate RX or TX queues descriptors: %s",
		      (void *)dev, strerror(errno));
		rte_free(rxqs);
		rte_free(txqs);
		return -errno;
	}
	dev->data->nb_rx_queues = rxqs_n;
	dev->data->rx_queues = (dpdk_rxq_t **)rxqs;
	dev->data->nb_tx_queues = txqs_n;
	dev->data->tx_queues = (dpdk_txq_t **)txqs;
	return dev_configure(dev);
}

#endif /* BUILT_DPDK_VERSION */

/* TX queues handling. */

static int
txq_alloc_elts(struct txq *txq, unsigned int elts_n)
{
	unsigned int i;
	struct txq_elt (*elts)[elts_n] =
		rte_calloc_socket("TXQ elements", 1, sizeof(*elts), 0,
				  txq->socket);
	int ret = 0;

	if (elts == NULL) {
		DEBUG("%p: can't allocate packets array", (void *)txq);
		ret = ENOMEM;
		goto error;
	}
	for (i = 0; (i != elts_n); ++i) {
		struct txq_elt *elt = &(*elts)[i];
		struct ibv_send_wr *wr = &elt->wr;
		struct ibv_sge (*sges)[(elemof(elt->sges))] = &elt->sges;

		/* These two arrays must have the same size. */
		assert(elemof(elt->sges) == elemof(elt->bufs));
		/* Configure WR. */
		wr->wr_id = i;
		wr->sg_list = &(*sges)[0];
		wr->opcode = IBV_WR_SEND;
		/* The following fields will be updated during each TX
		 * operation. Initializing them is pointless. */
		wr->next = NULL;
		wr->num_sge = elemof(*sges);
		wr->send_flags = IBV_SEND_SIGNALED;
		/* The same applies to elt->sges, elt->bufs and elt->comp. */
	}
	DEBUG("%p: allocated and configured %u WRs (%zu segments)",
	      (void *)txq, elts_n, (elts_n * elemof((*elts)[0].sges)));
	txq->elts_n = elts_n;
	txq->elts = elts;
	txq->elts_cur = 0;
	txq->elts_comp = 0;
	txq->elts_used = 0;
	txq->elts_free = elts_n;
	assert(ret == 0);
	return 0;
error:
	rte_free(elts);
	DEBUG("%p: failed, freed everything", (void *)txq);
	assert(ret != 0);
	return ret;
}

static void
txq_free_elts(struct txq *txq)
{
	unsigned int i;
	unsigned int elts_n = txq->elts_n;
	struct txq_elt (*elts)[elts_n] = txq->elts;

	DEBUG("%p: freeing WRs", (void *)txq);
	txq->elts_n = 0;
	txq->elts = NULL;
	if (elts == NULL)
		return;
	for (i = 0; (i != elemof(*elts)); ++i) {
		unsigned int j;
		struct txq_elt *elt = &(*elts)[i];

		for (j = 0; (j != elemof(elt->bufs)); ++j) {
			struct rte_mbuf *buf = elt->bufs[j];

			if (buf != NULL)
				rte_pktmbuf_free_seg(buf);
		}
	}
	rte_free(elts);
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
		int j;
		struct txq_elt *elt = txq->lost_comp;
		struct ibv_send_wr *wr = &elt->wr;
		struct rte_mbuf *(*bufs)[(elemof(elt->sges))] = &elt->bufs;

		txq->lost_comp = elt->comp;
		assert(elts_used != 0);
		assert(elts_free != txq->elts_n);
		assert(wr->num_sge > 0);
		for (j = 0; (j != wr->num_sge); ++j)
			rte_pktmbuf_free_seg((*bufs)[j]);
		--elts_used;
		++elts_free;
	}
	for (i = 0; (i != wcs_n); ++i) {
		struct ibv_wc *wc = &wcs[i];
		uint64_t wr_id = wc->wr_id;
		struct txq_elt *elt = &(*txq->elts)[wr_id];

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
			int j;
			struct ibv_send_wr *wr = &elt->wr;
			struct rte_mbuf *(*bufs)[(elemof(elt->sges))] =
				&elt->bufs;

			assert(elts_used != 0);
			assert(elts_free != txq->elts_n);
			assert(wr->num_sge > 0);
			assert((unsigned int)wr->num_sge <= elemof(elt->sges));
			for (j = 0; (j != wr->num_sge); ++j) {
				struct rte_mbuf *buf = (*bufs)[j];

				assert(buf != NULL);
#ifndef NDEBUG
				(*bufs)[j] = NULL;
				/* Make sure this segment is unlinked. */
				buf->pkt.next = NULL;
				/* SGE poisoning shouldn't hurt. */
				memset(&elt->sges[j], 0x44,
				       sizeof(elt->sges[j]));
#endif
				rte_pktmbuf_free_seg(buf);
			}
			--elts_used;
			++elts_free;
			elt = elt->comp;
		}
		while (unlikely(elt != NULL));
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
mlx4_tx_burst(dpdk_txq_t *dpdk_txq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct txq *txq = (struct txq *)dpdk_txq;
	struct ibv_send_wr head;
	struct ibv_send_wr **wr_next = &head.next;
	struct txq_elt *elt_prev = NULL;
	struct txq_elt (*elts)[txq->elts_n] = txq->elts;
	struct ibv_send_wr *bad_wr;
	unsigned int elts_cur = txq->elts_cur;
	unsigned int i;
	unsigned int max;
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
	for (i = 0; (i != max); ++i) {
		struct txq_elt *elt = &(*elts)[elts_cur];
		struct ibv_send_wr *wr = &elt->wr;
		struct rte_mbuf *buf;
		unsigned int seg_n = 0;

		assert(elts_cur < txq->elts_n);
		assert(wr->wr_id == elts_cur);
		assert(wr->sg_list == elt->sges);
		assert(wr->opcode == IBV_WR_SEND);
		/* Link WRs together for ibv_post_send(). */
		*wr_next = wr;
		wr_next = &wr->next;
		/* Link elements together in order to register a single
		 * completion event per burst of packets. */
		wr->send_flags &= ~IBV_SEND_SIGNALED;
		elt->comp = elt_prev;
		elt_prev = elt;
		/* Register each segment as SGEs. */
		for (buf = pkts[i];
		     ((buf != NULL) && (seg_n != elemof(elt->sges)));
		     buf = buf->pkt.next) {
			struct ibv_sge *sge = &elt->sges[seg_n];
			uint32_t lkey;

			/* Retrive Memory Region key for this memory pool. */
			lkey = txq_mp2mr(txq, buf->pool);
			if (unlikely(lkey == (uint32_t)-1)) {
				/* MR doesn't exist, stop here. */
				DEBUG("unable to get MP <-> MR association");
				break;
			}
			/* Ignore empty segments (except the first one). */
			if (unlikely((buf->pkt.data_len == 0) &&
				     (buf != pkts[i])))
				continue;
			/* Update SGE. */
			elt->bufs[seg_n] = buf;
			sge->addr = (uintptr_t)buf->pkt.data;
			sge->length = buf->pkt.data_len;
			sge->lkey = lkey;
			/* Increase number of segments (SGEs). */
			++seg_n;
#ifdef MLX4_PMD_SOFT_COUNTERS
			/* Increase sent bytes counter. */
			txq->stats.obytes += sge->length;
#endif
		}
		if (unlikely(buf != NULL)) {
			DEBUG("too many segments for packet (maximum is %zu)",
			      elemof(elt->sges));
			/* Ignore this packet. */
			++txq->stats.odropped;
			rte_pktmbuf_free(pkts[i]);
			goto next;
		}
		/* Update WR. */
		wr->num_sge = seg_n;
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
	*wr_next = NULL;
	/* The last WR is the only one asking for a completion event. */
	containerof(wr_next, struct ibv_send_wr, next)->
		send_flags |= IBV_SEND_SIGNALED;
	/* Make sure all packets have been processed in the previous loop. */
	assert(i == max);
	err = ibv_post_send(txq->qp, head.next, &bad_wr);
	if (unlikely(err)) {
		struct txq_elt *bad = containerof(bad_wr, struct txq_elt, wr);
		struct txq_elt *first = bad;

		/* Number of packets that will be sent. */
		for (i = 0; (first->comp != NULL); ++i)
			first = first->comp;
		assert(i < max);
		DEBUG("%p: ibv_post_send(): failed for WR %p"
		      " (only %u out of %u WR(s) posted): %s",
		      (void *)txq->priv, (void *)bad_wr, i, max,
		      ((err <= -1) ? "Internal error" : strerror(err)));
		/* Rollback elts_cur. */
		elts_cur -= (max - i);
		elts_cur %= txq->elts_n;
		/* Completion event has been lost. Link these elements to the
		 * list of those without a completion event. They will be
		 * processed the next time a completion event is received. */
		assert(first->comp == NULL);
		if (bad->comp != NULL) {
			first->comp = txq->lost_comp;
			txq->lost_comp = bad->comp;
		}
		else
			assert(bad == first);
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Decrement packets and bytes counters for each element that
		 * won't be sent. */
		while (bad != NULL) {
			--txq->stats.opackets;
			assert(bad->bufs[0] != NULL);
			txq->stats.obytes -= bad->bufs[0]->pkt.pkt_len;
			bad = containerof(bad->wr.next, struct txq_elt, wr);
		}
#endif
	}
	else
		++txq->elts_comp;
	txq->elts_used += i;
	txq->elts_free -= i;
	txq->elts_cur = elts_cur;
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
					 MLX4_PMD_SGE_WR_N)
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
	dev->tx_pkt_burst = mlx4_tx_burst;
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

	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if (idx >= priv->txqs_n) {
		DEBUG("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->txqs_n);
		return -EOVERFLOW;
	}
	if (txq != NULL) {
		DEBUG("%p: queue index %u is already allocated (%p)",
		      (void *)dev, idx, (void *)txq);
		if (priv->started)
			return -EEXIST;
		(*priv->txqs)[idx] = NULL;
		txq_cleanup(txq);
		rte_free(txq);
	}
	txq = rte_calloc_socket("TXQ", 1, sizeof(*txq), 0, socket);
	if (txq == NULL) {
		DEBUG("%p: unable to allocate queue index %u: %s",
		      (void *)dev, idx, strerror(errno));
		return -errno;
	}
	ret = txq_setup(dev, txq, desc, socket, conf);
	if (ret)
		rte_free(txq);
	else {
		DEBUG("%p: adding TX queue %p to list",
		      (void *)dev, (void *)txq);
		(*priv->txqs)[idx] = txq;
	}
	return ret;
}

#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)

static void
mlx4_tx_queue_release(dpdk_txq_t *dpdk_txq)
{
	struct txq *txq = (struct txq *)dpdk_txq;
	struct priv *priv;
	unsigned int i;

	if (txq == NULL)
		return;
	priv = txq->priv;
	for (i = 0; (i != priv->txqs_n); ++i)
		if ((*priv->txqs)[i] == txq) {
			DEBUG("%p: removing TX queue %p from list",
			      (void *)priv->dev, (void *)txq);
			(*priv->txqs)[i] = NULL;
			break;
		}
	txq_cleanup(txq);
	rte_free(txq);
}

#endif /* BUILT_DPDKP_VERSION */

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
			assert(((uintptr_t)buf->buf_addr +
				RTE_PKTMBUF_HEADROOM) ==
			       (uintptr_t)buf->pkt.data);
			/* Buffer is supposed to be empty. */
			assert(rte_pktmbuf_data_len(buf) == 0);
			assert(rte_pktmbuf_pkt_len(buf) == 0);
			/* sge->addr must be able to store a pointer. */
			assert(sizeof(sge->addr) >= sizeof(uintptr_t));
			if (j == 0) {
				/* The first SGE keeps its headroom. */
				sge->addr = (uintptr_t)buf->pkt.data;
				sge->length = (buf->buf_len -
					       RTE_PKTMBUF_HEADROOM);
			}
			else {
				/* Subsequent SGEs lose theirs. */
				assert(((uintptr_t)buf->pkt.data -
					RTE_PKTMBUF_HEADROOM) ==
				       (uintptr_t)buf->buf_addr);
				buf->pkt.data = buf->buf_addr;
				sge->addr = (uintptr_t)buf->pkt.data;
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

		/* Configure WR. */
		wr->wr_id = i;
		wr->next = &(*elts)[(i + 1)].wr;
		wr->sg_list = sge;
		wr->num_sge = 1;
		elt->buf = rte_pktmbuf_alloc(rxq->mp);
		if (elt->buf == NULL) {
			DEBUG("%p: empty mbuf pool", (void *)rxq);
			ret = ENOMEM;
			goto error;
		}
		/* Headroom is reserved by rte_pktmbuf_alloc(). */
		assert(((uintptr_t)elt->buf->buf_addr +
			RTE_PKTMBUF_HEADROOM) ==
		       (uintptr_t)elt->buf->pkt.data);
		/* Buffer is supposed to be empty. */
		assert(rte_pktmbuf_data_len(elt->buf) == 0);
		assert(rte_pktmbuf_pkt_len(elt->buf) == 0);
		/* sge->addr must be able to store a pointer. */
		assert(sizeof(sge->addr) >= sizeof(uintptr_t));
		/* SGE keeps its headroom. */
		sge->addr = (uintptr_t)elt->buf->pkt.data;
		sge->length = (elt->buf->buf_len - RTE_PKTMBUF_HEADROOM);
		sge->lkey = rxq->mr->lkey;
		/* Redundant check for tailroom. */
		assert(sge->length == rte_pktmbuf_tailroom(elt->buf));
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

			if (elt->buf != NULL)
				rte_pktmbuf_free_seg(elt->buf);
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

		if (elt->buf != NULL)
			rte_pktmbuf_free_seg(elt->buf);
	}
	rte_free(elts);
}

static void
rxq_mac_addr_del(struct rxq *rxq, unsigned int mac_index)
{
#ifndef NDEBUG
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
mlx4_rx_burst(dpdk_rxq_t *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n);

static uint16_t
mlx4_rx_burst_sp(dpdk_rxq_t *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
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
			rte_prefetch0(&seg->pkt);
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
				rep->pkt.data_len = 0;
			}
#endif
			seg_headroom = ((uintptr_t)seg->pkt.data -
					(uintptr_t)seg->buf_addr);
			seg_tailroom = (seg->buf_len - seg_headroom);
			assert(seg_tailroom == rte_pktmbuf_tailroom(seg));
			/* Only the first segment comes with headroom. */
			assert((j == 0) ?
			       (seg_headroom == RTE_PKTMBUF_HEADROOM) :
			       (seg_headroom == 0));
			rep->pkt.data = (void *)((uintptr_t)rep->buf_addr +
						 seg_headroom);
			assert(rte_pktmbuf_headroom(rep) == seg_headroom);
			assert(rte_pktmbuf_tailroom(rep) == seg_tailroom);
			/* Reconfigure sge to use rep instead of seg. */
			sge->addr = (uintptr_t)rep->pkt.data;
			assert(sge->length == seg_tailroom);
			assert(sge->lkey == rxq->mr->lkey);
			elt->bufs[j] = rep;
			++j;
			/* Update pkt_buf if it's the first segment, or link
			 * seg to the previous one and update pkt_buf_next. */
			*pkt_buf_next = seg;
			pkt_buf_next = &seg->pkt.next;
			/* Update seg information. */
			seg->pkt.nb_segs = 1;
			seg->pkt.in_port = rxq->port_id;
			if (likely(len <= seg_tailroom)) {
				/* Last segment. */
				seg->pkt.data_len = len;
				seg->pkt.pkt_len = len;
				break;
			}
			seg->pkt.data_len = seg_tailroom;
			seg->pkt.pkt_len = seg_tailroom;
			len -= seg_tailroom;
		}
		/* Update head and tail segments. */
		*pkt_buf_next = NULL;
		assert(pkt_buf != NULL);
		assert(j != 0);
		pkt_buf->pkt.nb_segs = j;
		pkt_buf->pkt.pkt_len = wc->byte_len;
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
mlx4_rx_burst(dpdk_rxq_t *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
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
		uint64_t wr_id = wc->wr_id;
		uint32_t len = wc->byte_len;
		struct rxq_elt *elt = &(*elts)[wr_id];
		struct ibv_recv_wr *wr = &elt->wr;
		struct rte_mbuf *seg = elt->buf;
		struct rte_mbuf *rep;
		uint32_t seg_headroom;
#ifndef NDEBUG
		uint32_t seg_tailroom;
#endif

		/* Sanity checks. */
		assert(wr_id < rxq->elts_n);
		assert(wr_id == wr->wr_id);
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
		rte_prefetch0(&seg->pkt);
		rep = __rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(rep == NULL)) {
			/*
			 * Unable to allocate a replacement mbuf,
			 * repost WR.
			 */
			DEBUG("rxq=%p, wr_id=%" PRIu64 ":"
			      " can't allocate a new mbuf",
			      (void *)rxq, wr_id);
			/* Increase out of memory counters. */
			++rxq->stats.rx_nombuf;
			++rxq->priv->dev->data->rx_mbuf_alloc_failed;
			goto repost;
		}
#ifndef NDEBUG
		else {
			/* assert() checks below need this. */
			rep->pkt.data_len = 0;
		}
#endif
		seg_headroom = ((uintptr_t)seg->pkt.data -
				(uintptr_t)seg->buf_addr);
		/* We don't expect any scattered packets. */
		assert(len <= (seg_tailroom = (seg->buf_len - seg_headroom)));
		assert(seg_tailroom == rte_pktmbuf_tailroom(seg));
		/* Only the first segment comes with headroom. */
		assert(seg_headroom == RTE_PKTMBUF_HEADROOM);
		rep->pkt.data = (void *)((uintptr_t)rep->buf_addr +
					 seg_headroom);
		assert(rte_pktmbuf_headroom(rep) == seg_headroom);
		assert(rte_pktmbuf_tailroom(rep) == seg_tailroom);
		/* Reconfigure sge to use rep instead of seg. */
		elt->sge.addr = (uintptr_t)rep->pkt.data;
		assert(elt->sge.length == seg_tailroom);
		assert(elt->sge.lkey == rxq->mr->lkey);
		elt->buf = rep;
		/* Update seg information. */
		seg->pkt.nb_segs = 1;
		seg->pkt.in_port = rxq->port_id;
		seg->pkt.pkt_len = len;
		seg->pkt.data_len = len;
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
	union {
		struct ibv_qp_init_attr init;
		struct ibv_qp_attr mod;
	} attr;
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
	attr.init = (struct ibv_qp_init_attr){
		/* CQ to be associated with the send queue. */
		.send_cq = tmpl.cq,
		/* CQ to be associated with the receive queue. */
		.recv_cq = tmpl.cq,
		.cap = {
			/* Max number of outstanding WRs. */
			.max_recv_wr = ((priv->device_attr.max_qp_wr < desc) ?
					priv->device_attr.max_qp_wr :
					desc),
			/* Max number of scatter/gather elements in a WR. */
			.max_recv_sge = ((priv->device_attr.max_sge <
					  MLX4_PMD_SGE_WR_N) ?
					 priv->device_attr.max_sge :
					 MLX4_PMD_SGE_WR_N)
		},
		.qp_type = IBV_QPT_RAW_PACKET
	};
#if RSS_SUPPORT
	if (parent) {
		attr.init.qpg_type = IBV_QPG_PARENT;
		/* TSS isn't necessary. */
		attr.init.parent_attrib.tss_child_count = 0;
		attr.init.parent_attrib.rss_child_count = priv->rxqs_n;
		DEBUG("initializing parent RSS queue");
	}
	else if (priv->rss) {
		/* Make it a RSS queue. */
		attr.init.qpg_type = IBV_QPG_CHILD_RX;
		assert(priv->rxq_parent.qp != NULL);
		attr.init.qpg_parent = priv->rxq_parent.qp;
		DEBUG("initializing child RSS queue");
	}
	else {
		attr.init.qpg_type = IBV_QPG_NONE;
		attr.init.qpg_parent = NULL;
		DEBUG("initializing queue without RSS");
	}
#endif /* RSS_SUPPORT */
	tmpl.qp = ibv_create_qp(priv->pd, &attr.init);
	if (tmpl.qp == NULL) {
		ret = errno;
		DEBUG("%p: QP creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	attr.mod = (struct ibv_qp_attr){
		/* Move the QP to this state. */
		.qp_state = IBV_QPS_INIT,
		/* Primary port number. */
		.port_num = priv->port
	};
	if ((ret = ibv_modify_qp(tmpl.qp, &attr.mod,
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
	attr.mod = (struct ibv_qp_attr){
		.qp_state = IBV_QPS_RTR
	};
	if ((ret = ibv_modify_qp(tmpl.qp, &attr.mod, IBV_QP_STATE))) {
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

	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if (idx >= priv->rxqs_n) {
		DEBUG("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->rxqs_n);
		return -EOVERFLOW;
	}
	if (rxq != NULL) {
		DEBUG("%p: queue index %u is already allocated (%p)",
		      (void *)dev, idx, (void *)rxq);
		if (priv->started)
			return -EEXIST;
		(*priv->rxqs)[idx] = NULL;
		rxq_cleanup(rxq);
		rte_free(rxq);
	}
	rxq = rte_calloc_socket("RXQ", 1, sizeof(*rxq), 0, socket);
	if (rxq == NULL) {
		DEBUG("%p: unable to allocate queue index %u: %s",
		      (void *)dev, idx, strerror(errno));
		return -errno;
	}
	ret = rxq_setup(dev, rxq, desc, socket, conf, mp);
	if (ret)
		rte_free(rxq);
	else {
		DEBUG("%p: adding RX queue %p to list",
		      (void *)dev, (void *)rxq);
		(*priv->rxqs)[idx] = rxq;
		/* Update receive callback. */
		if (rxq->sp)
			dev->rx_pkt_burst = mlx4_rx_burst_sp;
		else
			dev->rx_pkt_burst = mlx4_rx_burst;
	}
	return ret;
}

#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)

static void
mlx4_rx_queue_release(dpdk_rxq_t *dpdk_rxq)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct priv *priv;
	unsigned int i;

	if (rxq == NULL)
		return;
	priv = rxq->priv;
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
}

#endif /* BUILT_DPDK_VERSION */

/* Simulate device start by attaching all configured flows. */
static int
mlx4_dev_start(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i = 0;
	unsigned int r;
	struct rxq *rxq;

	if (priv->started)
		return 0;
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

	if (!priv->started)
		return;
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
}

#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)

/* See mlx4_dev_infos_get() and mlx4_dev_close().
 * DPDK 1.2 leaves queues allocation and management to the PMD. */
static void *removed_queue[512];

#endif /* BUILT_DPDK_VERSION */

static uint16_t
removed_tx_burst(dpdk_txq_t *dpdk_txq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	(void)dpdk_txq;
	(void)pkts;
	(void)pkts_n;
	return 0;
}

static uint16_t
removed_rx_burst(dpdk_rxq_t *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
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

	DEBUG("%p: closing device \"%s\"",
	      (void *)dev, priv->ctx->device->name);
	/* Prevent crashes when queues are still in use. This is unfortunately
	 * still required for DPDK 1.3 because some programs (such as testpmd)
	 * never release them before closing the device. */
	dev->rx_pkt_burst = removed_rx_burst;
	dev->tx_pkt_burst = removed_tx_burst;
	if (priv->rxqs != NULL) {
		/* XXX race condition if mlx4_rx_burst() is still running. */
		usleep(1000);
#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)
		dev->data->nb_rx_queues = 0;
		dev->data->rx_queues = (void *)removed_queue;
#endif /* BUILT_DPDK_VERSION */
		for (i = 0; (i != priv->rxqs_n); ++i) {
			tmp = (*priv->rxqs)[i];
			if (tmp == NULL)
				continue;
			(*priv->rxqs)[i] = NULL;
			rxq_cleanup(tmp);
			rte_free(tmp);
		}
		priv->rxqs_n = 0;
#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)
		tmp = priv->rxqs;
		priv->rxqs = NULL;
		rte_free(tmp);
#else /* BUILT_DPDK_VERSION */
		priv->rxqs = NULL;
#endif /* BUILT_DPDK_VERSION */
	}
	if (priv->txqs != NULL) {
		/* XXX race condition if mlx4_tx_burst() is still running. */
		usleep(1000);
#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)
		dev->data->nb_tx_queues = 0;
		dev->data->tx_queues = (void *)removed_queue;
#endif /* BUILT_DPDK_VERSION */
		for (i = 0; (i != priv->txqs_n); ++i) {
			tmp = (*priv->txqs)[i];
			if (tmp == NULL)
				continue;
			(*priv->txqs)[i] = NULL;
			txq_cleanup(tmp);
			rte_free(tmp);
		}
		priv->txqs_n = 0;
#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)
		tmp = priv->txqs;
		priv->txqs = NULL;
		rte_free(tmp);
#else /* BUILT_DPDK_VERSION */
		priv->txqs = NULL;
#endif /* BUILT_DPDK_VERSION */
	}
	if (priv->rss)
		rxq_cleanup(&priv->rxq_parent);
	assert(priv->pd != NULL);
	claim_zero(ibv_dealloc_pd(priv->pd));
	claim_zero(ibv_close_device(priv->ctx));
	memset(priv, 0, sizeof(*priv));
}

static void
mlx4_dev_infos_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *info)
{
	const struct priv *priv = dev->data->dev_private;
	unsigned int max;

	/* FIXME: we should ask the device for these values. */
	info->min_rx_bufsize = 32;
	info->max_rx_pktlen = 65536;
	/*
	 * Since we need one CQ per QP, the limit is the minimum number
	 * between the two values.
	 */
	max = ((priv->device_attr.max_cq > priv->device_attr.max_qp) ?
	       priv->device_attr.max_qp : priv->device_attr.max_cq);
#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)
	/*
	 * The number of dummy queue pointers available to replace them
	 * during mlx4_dev_close() also limit this value.
	 */
	max = ((max > elemof(removed_queue)) ? elemof(removed_queue) : max);
#endif /* BUILT_DPDK_VERSION */
	info->max_rx_queues = max;
	info->max_tx_queues = max;
	info->max_mac_addrs = elemof(priv->mac);
}

static void
mlx4_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	const struct priv *priv = dev->data->dev_private;
	struct rte_eth_stats tmp = { .ipackets = 0 };
	unsigned int i;

	/* Add software counters. */
	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		tmp.ipackets += (*priv->rxqs)[i]->stats.ipackets;
		tmp.ibytes += (*priv->rxqs)[i]->stats.ibytes;
		tmp.ierrors += (*priv->rxqs)[i]->stats.idropped;
		tmp.rx_nombuf += (*priv->rxqs)[i]->stats.rx_nombuf;
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		if ((*priv->txqs)[i] == NULL)
			continue;
		tmp.opackets += (*priv->txqs)[i]->stats.opackets;
		tmp.obytes += (*priv->txqs)[i]->stats.obytes;
		tmp.oerrors += (*priv->txqs)[i]->stats.odropped;
	}
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: retrieve and add hardware counters. */
#endif
	*stats = tmp;
}

static void
mlx4_stats_reset(struct rte_eth_dev *dev)
{
	const struct priv *priv = dev->data->dev_private;
	unsigned int i;

	for (i = 0; (i != priv->rxqs_n); ++i) {
		if ((*priv->rxqs)[i] == NULL)
			continue;
		(*priv->rxqs)[i]->stats =
			(struct rte_rxq_stats){ .ipackets = 0 };
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		if ((*priv->txqs)[i] == NULL)
			continue;
		(*priv->txqs)[i]->stats =
			(struct rte_txq_stats){ .opackets = 0 };
	}
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: reset hardware counters. */
#endif
}

#ifdef DPDK_6WIND

static void
mlx4_rxq_stats_get(struct rte_eth_dev *dev, uint16_t idx,
		   struct rte_rxq_stats *stats)
{
	const struct priv *priv = dev->data->dev_private;

	assert(idx < priv->rxqs_n);
	if ((*priv->rxqs)[idx] == NULL)
		return;
	*stats = (*priv->rxqs)[idx]->stats;
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: add hardware counters. */
#endif
}

static void
mlx4_txq_stats_get(struct rte_eth_dev *dev, uint16_t idx,
		   struct rte_txq_stats *stats)
{
	const struct priv *priv = dev->data->dev_private;

	assert(idx < priv->txqs_n);
	if ((*priv->txqs)[idx] == NULL)
		return;
	*stats = (*priv->txqs)[idx]->stats;
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: add hardware counters. */
#endif
}

static void
mlx4_rxq_stats_reset(struct rte_eth_dev *dev, uint16_t idx)
{
	const struct priv *priv = dev->data->dev_private;

	assert(idx < priv->rxqs_n);
	if ((*priv->rxqs)[idx] == NULL)
		return;
	(*priv->rxqs)[idx]->stats = (struct rte_rxq_stats){ .ipackets = 0 };
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: reset hardware counters. */
#endif
}

static void
mlx4_txq_stats_reset(struct rte_eth_dev *dev, uint16_t idx)
{
	const struct priv *priv = dev->data->dev_private;

	assert(idx < priv->txqs_n);
	if ((*priv->txqs)[idx] == NULL)
		return;
	(*priv->txqs)[idx]->stats = (struct rte_txq_stats){ .opackets = 0 };
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: reset hardware counters. */
#endif
}

#endif /* DPDK_6WIND */

static void
mlx4_mac_addr_remove(struct rte_eth_dev *dev, uint32_t index)
{
	struct priv *priv = dev->data->dev_private;

	DEBUG("%p: removing MAC address from index %" PRIu32,
	      (void *)dev, index);
	if (index >= MLX4_MAX_MAC_ADDRESSES)
		return;
	/* Refuse to remove the broadcast address, this one is special. */
	if (!memcmp(priv->mac[index].addr_bytes, "\xff\xff\xff\xff\xff\xff",
		    ETHER_ADDR_LEN))
		return;
	priv_mac_addr_del(priv, index);
}

static void
mlx4_mac_addr_add(struct rte_eth_dev *dev, struct ether_addr *mac_addr,
		  uint32_t index, uint32_t vmdq)
{
	struct priv *priv = dev->data->dev_private;

	(void)vmdq;
	DEBUG("%p: adding MAC address at index %" PRIu32,
	      (void *)dev, index);
	if (index >= MLX4_MAX_MAC_ADDRESSES)
		return;
	/* Refuse to add the broadcast address, this one is special. */
	if (!memcmp(mac_addr->addr_bytes, "\xff\xff\xff\xff\xff\xff",
		    ETHER_ADDR_LEN))
		return;
	priv_mac_addr_add(priv, index,
			  (const uint8_t (*)[ETHER_ADDR_LEN])
			  mac_addr->addr_bytes);
}

static void
mlx4_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	int ret;

	if (priv->promisc)
		return;
	/* If device isn't started, this is all we need to do. */
	if (!priv->started)
		goto end;
	if (priv->rss) {
		ret = rxq_promiscuous_enable(&priv->rxq_parent);
		if (ret)
			return;
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
		return;
	}
end:
	priv->promisc = 1;
}

static void
mlx4_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;

	if (!priv->promisc)
		return;
	if (priv->rss) {
		rxq_promiscuous_disable(&priv->rxq_parent);
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] != NULL)
			rxq_promiscuous_disable((*priv->rxqs)[i]);
end:
	priv->promisc = 0;
}

static void
mlx4_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;
	int ret;

	if (priv->allmulti)
		return;
	/* If device isn't started, this is all we need to do. */
	if (!priv->started)
		goto end;
	if (priv->rss) {
		ret = rxq_allmulticast_enable(&priv->rxq_parent);
		if (ret)
			return;
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
		return;
	}
end:
	priv->allmulti = 1;
}

static void
mlx4_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;

	if (!priv->allmulti)
		return;
	if (priv->rss) {
		rxq_allmulticast_disable(&priv->rxq_parent);
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i)
		if ((*priv->rxqs)[i] != NULL)
			rxq_allmulticast_disable((*priv->rxqs)[i]);
end:
	priv->allmulti = 0;
}

static int
mlx4_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct priv *priv = dev->data->dev_private;
	struct ibv_port_attr port_attr;
	static const uint8_t width_mult[] = {
		/* Multiplier values taken from devinfo.c in libibverbs. */
		0, 1, 4, 0, 8, 0, 0, 0, 12, 0
	};

	(void)wait_to_complete;
	DEBUG("%p", (void *)dev);
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

#ifdef DPDK_6WIND

static int
mlx4_dev_control(struct rte_eth_dev *dev, uint32_t command, void *arg)
{
	struct priv *priv = dev->data->dev_private;
	union {
		uint16_t u16;
		struct rte_dev_ethtool_drvinfo info;
		struct rte_dev_ethtool_gsettings gset;
	} *data = arg;

	switch (command) {
	case RTE_DEV_CMD_GET_MTU:
		data->u16 = priv->mtu;
		return 0;
	case RTE_DEV_CMD_SET_MTU:
		priv->mtu = data->u16;
		return 0;
	case RTE_DEV_CMD_ETHTOOL_GET_DRVINFO:
		snprintf(data->info.driver, sizeof(data->info.driver),
			 "mlx4_pmd");
		snprintf(data->info.bus_info, sizeof(data->info.bus_info),
			 PCI_PRI_FMT,
			 (unsigned int)dev->pci_dev->addr.domain,
			 (unsigned int)dev->pci_dev->addr.bus,
			 (unsigned int)dev->pci_dev->addr.devid,
			 (unsigned int)dev->pci_dev->addr.function);
		return 0;
	case RTE_DEV_CMD_ETHTOOL_GET_SETTINGS:
		mlx4_link_update(dev, 0);
		/* phy_addr not available. */
		data->gset.phy_addr = 0;
		data->gset.phy_addr = ~data->gset.phy_addr;
		/* Approximate values. */
		data->gset.supported = RTE_SUPPORTED_Autoneg;
		data->gset.autoneg_advertised = RTE_ADVERTISED_Autoneg;
		data->gset.port = RTE_PORT_OTHER;
		data->gset.transceiver = RTE_XCVR_EXTERNAL;
		data->gset.speed = dev->data->dev_link.link_speed;
		data->gset.duplex = dev->data->dev_link.link_duplex;
		data->gset.status = dev->data->dev_link.link_status;
		data->gset.autoneg = RTE_AUTONEG_ENABLE;
		return 0;
	case RTE_DEV_CMD_ETHTOOL_GET_SSET_COUNT:
	case RTE_DEV_CMD_ETHTOOL_GET_STRINGS:
	case RTE_DEV_CMD_ETHTOOL_GET_STATS:
	case RTE_DEV_CMD_ETHTOOL_GET_PAUSEPARAM:
	case RTE_DEV_CMD_ETHTOOL_SET_PAUSEPARAM:
	default:
		break;
	}
	return -ENOTSUP;
}

#endif /* DPDK_6WIND */

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

#if BUILT_DPDK_VERSION < DPDK_VERSION(1, 3, 0)

static void
mlx4_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	vlan_filter_set(dev, vlan_id, on);
}

#else /* BUILT_DPDK_VERSION */

static int
mlx4_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	return vlan_filter_set(dev, vlan_id, on);
}

#endif /* BUILT_DPDK_VERSION */

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
#ifdef DPDK_6WIND
	.rxq_stats_get = mlx4_rxq_stats_get,
	.txq_stats_get = mlx4_txq_stats_get,
	.rxq_stats_reset = mlx4_rxq_stats_reset,
	.txq_stats_reset = mlx4_txq_stats_reset,
#endif /* DPDK_6WIND */
#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)
	.queue_stats_mapping_set = NULL,
#endif /* BUILT_DPDK_VERSION */
	.dev_infos_get = mlx4_dev_infos_get,
	.vlan_filter_set = mlx4_vlan_filter_set,
#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)
	.vlan_tpid_set = NULL,
	.vlan_strip_queue_set = NULL,
	.vlan_offload_set = NULL,
#endif /* BUILT_DPDK_VERSION */
	.rx_queue_setup = mlx4_rx_queue_setup,
	.tx_queue_setup = mlx4_tx_queue_setup,
#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)
	.rx_queue_release = mlx4_rx_queue_release,
	.tx_queue_release = mlx4_tx_queue_release,
#endif /* BUILT_DPDK_VERSION */
	.dev_led_on = NULL,
	.dev_led_off = NULL,
	.flow_ctrl_set = NULL,
#if BUILT_DPDK_VERSION >= DPDK_VERSION(1, 3, 0)
	.priority_flow_ctrl_set = NULL,
#endif /* BUILT_DPDK_VERSION */
	.mac_addr_remove = mlx4_mac_addr_remove,
	.mac_addr_add = mlx4_mac_addr_add,
#ifdef DPDK_6WIND
	.dev_control = mlx4_dev_control,
#endif /* DPDK_6WIND */
	.fdir_add_signature_filter = NULL,
	.fdir_update_signature_filter = NULL,
	.fdir_remove_signature_filter = NULL,
	.fdir_add_perfect_filter = NULL,
	.fdir_update_perfect_filter = NULL,
	.fdir_remove_perfect_filter = NULL,
	.fdir_set_masks = NULL
};

/* Get PCI information from a device name, return nonzero on error. */
static int
mlx4_dev_name_to_pci_addr(const char *dev_name, struct rte_pci_addr *pci_addr)
{
	FILE *file = NULL;
	size_t len = 0;
	int ret;
	const char *sys_root = getenv("SYSFS_PATH");
	char line[32];

	if (sys_root == NULL)
		sys_root = "/sys";
	do {
		char path[(len + 1)];

		ret = snprintf(path,
			       (len + 1),
			       "%s/class/infiniband/%s/device/uevent",
			       sys_root,
			       dev_name);
		if (ret <= 0)
			return -errno;
		if (len == 0) {
			len = ret;
			continue;
		}
		file = fopen(path, "rb");
		if (file == NULL)
			return -errno;
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

/* Convert a 64 bit GUID to a MAC address. */
static void
guid_to_mac(uint8_t (*mac)[ETHER_ADDR_LEN], uint64_t guid)
{
	uint8_t tmp[sizeof(guid)];

	memcpy(tmp, &guid, sizeof(guid));
	memcpy(&(*mac)[0], &tmp[0], 3);
	memcpy(&(*mac)[3], &tmp[5], 3);
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

static struct eth_driver mlx4_driver;

static int
mlx4_generic_init(struct eth_driver *drv, struct rte_eth_dev *dev, int probe)
{
	struct priv *priv = dev->data->dev_private;
	struct ibv_device **list;
	int err = errno;
	uint32_t port = 1; /* ports are indexed from one */
	uint32_t test = 1;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_device_attr device_attr;
	struct ibv_port_attr port_attr;
	int idx;
	int i;

	assert(drv == &mlx4_driver);
	/* Get mlx4_dev[] index. */
	idx = mlx4_dev_idx(&dev->pci_dev->addr);
	if (idx == -1) {
		DEBUG("this driver cannot support any more adapters");
		return -ENOMEM;
	}
	DEBUG("using driver device index %d", idx);
#ifdef RTE_PCI_DRV_MULTIPLE
	/* Determine the physical port number to bind this driver to. */
	if (drv->pci_drv.drv_flags & RTE_PCI_DRV_MULTIPLE) {
		while (test & mlx4_dev[idx].ports) {
			test <<= 1;
			++port;
		}
		/* Abort now if all all possible ports are already bound. */
		if (test == 0)
			return -ENODEV;
	}
#else /* RTE_PCI_DRV_MULTIPLE */
	(void)drv;
	/* RTE_PCI_DRV_MULTIPLE not supported, only check the first port. */
	if (mlx4_dev[idx].ports & test)
		return -ENODEV;
#endif /* RTE_PCI_DRV_MULTIPLE */
	/* Save PCI address. */
	mlx4_dev[idx].pci_addr = dev->pci_dev->addr;
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
		if (mlx4_dev_name_to_pci_addr(list[i]->name, &pci_addr))
			continue;
		if ((dev->pci_dev->addr.domain != pci_addr.domain) ||
		    (dev->pci_dev->addr.bus != pci_addr.bus) ||
		    (dev->pci_dev->addr.devid != pci_addr.devid) ||
		    (dev->pci_dev->addr.function != pci_addr.function))
			continue;
		DEBUG("PCI information matches, using device \"%s\"",
		      list[i]->name);
		ctx = ibv_open_device(list[i]);
		err = errno;
		break;
	}
	ibv_free_device_list(list);
	if (ctx == NULL) {
		if (err == 0)
			err = ENODEV;
		errno = err;
		return -err;
	}
	DEBUG("device opened");
	if (ibv_query_device(ctx, &device_attr))
		goto error;
	DEBUG("%u port(s) detected", device_attr.phys_port_cnt);
	if ((port - 1) >= device_attr.phys_port_cnt) {
		DEBUG("port %u not available, skipping interface", port);
		errno = ENODEV;
		goto error;
	}
	DEBUG("using port %u (%08" PRIx32 ")", port, test);
	/* Check port status. */
	if ((errno = ibv_query_port(ctx, port, &port_attr))) {
		DEBUG("port query failed: %s", strerror(errno));
		goto error;
	}
	if (port_attr.state != IBV_PORT_ACTIVE)
		DEBUG("bad state for port %d: \"%s\" (%d)",
		      port, ibv_port_state_str(port_attr.state),
		      port_attr.state);
	if (probe)
		pd = NULL;
	else {
		/* Allocate protection domain. */
		pd = ibv_alloc_pd(ctx);
		if (pd == NULL) {
			DEBUG("PD allocation failure");
			errno = ENOMEM;
			goto error;
		}
	}
	mlx4_dev[idx].ports |= test;
	memset(priv, 0, sizeof(*priv));
	priv->dev = dev;
	priv->ctx = ctx;
	priv->device_attr = device_attr;
	priv->port_attr = port_attr;
	priv->port = port;
	priv->pd = pd;
	priv->mtu = 1500; /* Unused MTU. */
#if RSS_SUPPORT
	priv->hw_qpg = !!(device_attr.device_cap_flags & IBV_DEVICE_QPG);
	priv->hw_tss = !!(device_attr.device_cap_flags & IBV_DEVICE_UD_TSS);
	priv->hw_rss = !!(device_attr.device_cap_flags & IBV_DEVICE_UD_RSS);
	DEBUG("device flags: %s%s%s",
	      (priv->hw_qpg ? "IBV_DEVICE_QPG " : ""),
	      (priv->hw_tss ? "IBV_DEVICE_TSS " : ""),
	      (priv->hw_rss ? "IBV_DEVICE_RSS " : ""));
	if (priv->hw_rss)
		DEBUG("maximum RSS indirection table size: %u",
		      device_attr.max_rss_tbl_sz);
#endif /* RSS_SUPPORT */
	guid_to_mac(&priv->mac[0].addr_bytes, priv->device_attr.node_guid);
	BITFIELD_SET(priv->mac_configured, 0);
	/* Update MAC address according to port number. */
	priv->mac[0].addr_bytes[5] += (priv->port - 1);
	DEBUG("port %u MAC address is %02x:%02x:%02x:%02x:%02x:%02x",
	      priv->port,
	      priv->mac[0].addr_bytes[0], priv->mac[0].addr_bytes[1],
	      priv->mac[0].addr_bytes[2], priv->mac[0].addr_bytes[3],
	      priv->mac[0].addr_bytes[4], priv->mac[0].addr_bytes[5]);
	/* Register MAC and broadcast addresses. */
	claim_zero(priv_mac_addr_add(priv, 0,
				     (const uint8_t (*)[ETHER_ADDR_LEN])
				     priv->mac[0].addr_bytes));
	claim_zero(priv_mac_addr_add(priv, 1,
				     &(const uint8_t [ETHER_ADDR_LEN])
				     { "\xff\xff\xff\xff\xff\xff" }));
	dev->dev_ops = &mlx4_dev_ops;
	dev->data->mac_addrs = priv->mac;
	return 0;
error:
	err = errno;
	if (pd)
		claim_zero(ibv_dealloc_pd(pd));
	if (ctx)
		claim_zero(ibv_close_device(ctx));
	errno = err;
	return -err;
}

static int
mlx4_dev_init(struct eth_driver *drv, struct rte_eth_dev *dev)
{
	return mlx4_generic_init(drv, dev, 0);
}

#ifdef DPDK_6WIND
static int
mlx4_dev_probe(struct eth_driver *drv, struct rte_eth_dev *dev)
{
	return mlx4_generic_init(drv, dev, 1);
}
#endif

static struct rte_pci_id mlx4_pci_id_map[] = {
	{
		.vendor_id = PCI_VENDOR_ID_MELLANOX,
		.device_id = PCI_DEVICE_ID_MELLANOX_CONNECTX3,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID
	},
	{
		.vendor_id = 0
	}
};

static struct eth_driver mlx4_driver = {
	.pci_drv = {
		.name = "rte_mlx4_pmd",
		.id_table = mlx4_pci_id_map,
#ifdef RTE_PCI_DRV_MULTIPLE
		.drv_flags = RTE_PCI_DRV_MULTIPLE
#endif /* RTE_PCI_DRV_MULTIPLE */
	},
	.eth_dev_init = mlx4_dev_init,
#ifdef DPDK_6WIND
	.eth_dev_probe = mlx4_dev_probe,
#endif
	.dev_private_size = sizeof(struct priv)
};

#ifdef RTE_LIBRTE_MLX4_PMD

/* Exported initializer. */
int
rte_mlx4_pmd_init(void)
{
	rte_eth_driver_register(&mlx4_driver);
	return 0;
}

#else /* RTE_LIBRTE_MLX4_PMD */

/* Shared object initializer. */
static void __attribute__((constructor))
mlx4_pmd_init(void)
{
	rte_eth_driver_register(&mlx4_driver);
}

#endif /* RTE_LIBRTE_MLX4_PMD */
