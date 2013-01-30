/*
 * Copyright 6WIND 2012-2013, All rights reserved.
 * Copyright Mellanox 2012, All rights reserved.
 */

/*
 * Known limitations:
 * - Promiscuous mode doesn't work, libibverbs doesn't provide a way to
 *   control it.
 * - Broadcast/multicast bit isn't managed properly. This driver is only
 *   able to match broadcast frames with ff:ff:ff:ff:ff:ff as the destination
 *   address.
 * - VLAN filtering is implemented but doesn't work. Flow specifications
 *   (ibv_flow_spec) store information about it, but the kernel currently
 *   ignores it.
 * - RSS/RCA is unimplemented.
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
#ifdef PEDANTIC
#pragma GCC diagnostic error "-pedantic"
#endif

/* Verbs header. */
#include <infiniband/verbs.h>

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

/*
 * RX queue descriptor.
 *
 * Each work request in (*wrs)[] points to a subset (sges_wr_n elements) of
 * the (*sges)[] array. Sizes are as follows:
 *
 * - (*wrs)[wrs_n]
 * - sges_n = wrs_n * sges_wr_n
 * - (*sges)[sges_n]
 * - (*bufs)[sges_n]
 *
 * Each scatter/gather element in (*sges)[] is associated to a mbuf with the
 * same index in (*bufs)[]. struct ibv_sge only stores its the buffer address
 * as a uint64_t.
 * A separate array that stores the actual mbuf facilitates operations.
 */
struct rxq {
	struct priv *priv; /* Back pointer to private data. */
	struct ibv_pd *pd; /* Protection Domain. */
	struct rte_mempool *mp; /* Memory Pool for allocations. */
	size_t mp_size; /* mp size in bytes. */
	struct ibv_mr *mr; /* Memory Region (for mp). */
	struct ibv_cq *cq; /* Completion Queue. */
	struct ibv_qp *qp; /* Queue Pair. */
	/*
	 * Since flow specifications (ibv_flow_spec) are common to all
	 * RX queues, they are generated on the fly from MAC addresses,
	 * VLAN filters and other information in priv.
	 * The only information stored here is whether MAC flows are
	 * configured for this queue (local copy of priv->mac_configured,
	 * except when priv->started is 0).
	 */
	BITFIELD_DECLARE(mac_configured, uint32_t, MLX4_MAX_MAC_ADDRESSES);
	unsigned int port_id; /* Port ID for incoming packets. */
	unsigned int wrs_n; /* (*wrs)[] length. */
	unsigned int sges_n; /* (*sges)[] and (*bufs)[] lengths. */
	unsigned int sges_wr_n; /* Maximum Scatter/gather elements per WR. */
	struct ibv_recv_wr (*wrs)[]; /* Work Requests. */
	struct ibv_sge (*sges)[];  /* Scatter/Gather Elements. */
	struct rte_mbuf *(*bufs)[]; /* Scatter/Gather Elements buffers. */
	struct rte_rxq_stats stats; /* RX queue counters. */
};

/*
 * TX queue descriptor.
 */
struct txq {
	struct priv *priv; /* Back pointer to private data. */
	struct ibv_pd *pd; /* Protection Domain. */
	struct {
		struct rte_mempool *mp; /* Cached Memory Pool. */
		size_t mp_size; /* mp size in bytes. */
		struct ibv_mr *mr; /* Memory Region (for mp). */
		uint32_t lkey; /* mr->lkey */
	} mp2mr[MLX4_PMD_TX_MP_CACHE]; /* MP to MR translation table. */
	struct ibv_cq *cq; /* Completion Queue. */
	struct ibv_qp *qp; /* Queue Pair. */
	unsigned int wrs_n; /* (*wrs)[] length. */
	unsigned int sges_n; /* (*sges)[] and (*bufs)[] lengths. */
	unsigned int sges_wr_n; /* Maximum Scatter/gather elements per WR. */
	struct ibv_send_wr (*wrs)[]; /* Work Requests. */
	struct ibv_sge (*sges)[];  /* Scatter/Gather Elements. */
	struct rte_mbuf *(*bufs)[]; /* Scatter/Gather Elements buffers. */
	unsigned int wr_cur; /* Current WR index in (*wrs)[]. */
	unsigned int wrs_comp; /* Number of WRs waiting for completion. */
	unsigned int wrs_free; /* Number of free WRs. */
	struct rte_txq_stats stats; /* TX queue counters. */
};

struct priv {
	struct rte_eth_dev *dev; /* Ethernet device. */
	struct ibv_context *ctx; /* Verbs context. */
	struct ibv_device_attr device_attr; /* Device properties. */
	struct ibv_port_attr port_attr; /* Physical port properties. */
	/*
	 * MAC addresses array and configuration bit-field.
	 * An extra entry that cannot be modified by the DPDK is reserved
	 * for broadcast frames (destination MAC address ff:ff:ff:ff:ff:ff).
	 *
	 * XXX
	 * We're currently not able to use a mask on the destination MAC
	 * address. Thus, there's no way to match a single bit (least
	 * significant bit in the first octet for broadcast frames) or
	 * "anything" (for promiscuous mode emulation).
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
	/* RX/TX queues. */
	unsigned int rxqs_n; /* Number of RX queues. */
	unsigned int txqs_n; /* Number of TX queues. */
	struct rxq (*rxqs)[]; /* RX queues. */
	struct txq (*txqs)[]; /* TX queues. */
	/*
	 * Pointers to the above elements, for IGB compatibility.
	 * DPDK requires this for burst() functions.
	 */
	struct dpdk_rxq_t **dpdk_rxqs;
	struct dpdk_txq_t **dpdk_txqs;
};

/* Device configuration. */

static int
mlx4_dev_configure(struct rte_eth_dev *dev, uint16_t rxqs_n, uint16_t txqs_n)
{
	struct priv *priv = dev->data->dev_private;
	struct ibv_port_attr port_attr;
	struct rxq (*rxqs)[rxqs_n];
	struct txq (*txqs)[txqs_n];
	struct dpdk_rxq_t *(*dpdk_rxqs)[rxqs_n];
	struct dpdk_txq_t *(*dpdk_txqs)[txqs_n];

	if ((errno = ibv_query_port(priv->ctx, priv->port, &port_attr))) {
		DEBUG("port query failed: %s", strerror(errno));
		return -errno;
	}
	if (port_attr.state != IBV_PORT_ACTIVE) {
		DEBUG("bad state for port %d: \"%s\" (%d)",
		      priv->port, ibv_port_state_str(port_attr.state),
		      port_attr.state);
		/*
		 * Uncomment this to prevent the device from being configured
		 * when link state is bad.
		 */
		/* return -EIO; */
	}
	if ((rxqs_n < priv->rxqs_n) || (txqs_n < priv->txqs_n)) {
		DEBUG("lowering the number of RX or TX queues is unsupported");
		return -EINVAL;
	}
	if (((rxqs = realloc(priv->rxqs, (sizeof(*rxqs) +
					  sizeof(*dpdk_rxqs)))) == NULL) ||
	    (priv->rxqs = rxqs,
	     ((txqs = realloc(priv->txqs, (sizeof(*txqs) +
					   sizeof(*dpdk_txqs)))) == NULL))) {
		DEBUG("unable to allocate RX or TX queues descriptors: %s",
		      strerror(errno));
		return -errno;
	}
	/* Save port attributes. */
	priv->port_attr = port_attr;
	/* Save queues. */
	priv->txqs = txqs;
	memset(&(*rxqs)[priv->rxqs_n], 0,
	       (sizeof((*rxqs)[0]) * (rxqs_n - priv->rxqs_n)));
	priv->rxqs_n = rxqs_n;
	memset(&(*txqs)[priv->txqs_n], 0,
	       (sizeof((*txqs)[0]) * (txqs_n - priv->txqs_n)));
	priv->txqs_n = txqs_n;
	/*
	 * Copy this information to dev. We use the extra space allocated
	 * at the end of (*rxqs)[] and (*txqs)[] to store an array of pointers
	 * to their elements (*(*dpdk_rxqs)[] and *(*dpdk_txqs)[]). The DPDK
	 * will use them for the *burst() functions.
	 */
	dev->data->tx_queues = (dpdk_txq_t **)&(*txqs)[txqs_n];
	dev->data->nb_tx_queues = txqs_n;
	dev->data->rx_queues = (dpdk_rxq_t **)&(*rxqs)[rxqs_n];
	dev->data->nb_rx_queues = rxqs_n;
	while (txqs_n-- != 0)
		dev->data->tx_queues[txqs_n] = (dpdk_txq_t *)&(*txqs)[txqs_n];
	while (rxqs_n-- != 0)
		dev->data->rx_queues[rxqs_n] = (dpdk_rxq_t *)&(*rxqs)[rxqs_n];
	return 0;
}

/* TX queues handling. */

static int
txq_alloc_wrs(struct txq *txq, unsigned int wrs_n, unsigned int sges_wr_n)
{
	unsigned int i;
	unsigned int sges_n = (wrs_n * sges_wr_n);
	struct ibv_send_wr (*wrs)[wrs_n] = calloc(1, sizeof(*wrs));
	struct ibv_send_wr *wr;
	struct ibv_sge (*sges)[sges_n] = calloc(1, sizeof(*sges));
	struct ibv_sge *sge;
	struct rte_mbuf *(*bufs)[sges_n] = calloc(1, sizeof(*bufs));
	int ret = 0;

	if ((wrs == NULL) || (sges == NULL) || (bufs == NULL)) {
		DEBUG("%p: can't allocate something: wrs=%p sges=%p bufs=%p",
		      (void *)txq, (void *)wrs, (void *)sges, (void *)bufs);
		ret = ENOMEM;
		goto error;
	}
	for (i = 0; (i != sges_n); ++i)
		sge = &(*sges)[i];
	DEBUG("%p: allocated %u SGEs (%u WRs with %u SGEs)",
	      (void *)txq, sges_n, wrs_n, sges_wr_n);
	for (i = 0; (i != wrs_n); ++i) {
		wr = &(*wrs)[i];
		wr->wr_id = i;
		wr->next = NULL;
		wr->sg_list = &(*sges)[(i * sges_wr_n)];
		wr->num_sge = 0;
		wr->opcode = IBV_WR_SEND;
		/* IBV_SEND_SIGNALED unnecessary because .sq_sig_all == 1. */
		wr->send_flags = 0; /* XXX */
	}
	DEBUG("%p: configured %u WRs", (void *)txq, wrs_n);
	txq->wrs = wrs;
	txq->sges = sges;
	txq->bufs = bufs;
	txq->wrs_n = wrs_n;
	txq->sges_n = sges_n;
	txq->sges_wr_n = sges_wr_n;
	txq->wrs_comp = 0;
	txq->wrs_free = txq->wrs_n;
	assert(ret == 0);
	return 0;
error:
	free(wrs);
	free(sges);
	free(bufs);
	DEBUG("%p: failed, freed everything", (void *)txq);
	assert(ret != 0);
	return ret;
}

static void
txq_free_wrs(struct txq *txq)
{
	unsigned int i;
	struct ibv_send_wr (*wrs)[] = txq->wrs;
	struct ibv_sge (*sges)[] = txq->sges;
	struct rte_mbuf *(*bufs)[] = txq->bufs;

	DEBUG("%p: freeing WRs and SGEs", (void *)txq);
	txq->wrs = NULL;
	txq->sges = NULL;
	txq->bufs = NULL;
	free(wrs);
	free(sges);
	if (bufs == NULL)
		return;
	for (i = 0; (i != txq->sges_n); ++i) {
		if ((*bufs)[i] != NULL)
			rte_pktmbuf_free_seg((*bufs)[i]);
	}
	free(bufs);
}

static void
txq_cleanup(struct txq *txq)
{
	size_t i;

	DEBUG("cleaning up %p", (void *)txq);
	txq_free_wrs(txq);
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
	if (txq->pd != NULL)
		claim_zero(ibv_dealloc_pd(txq->pd));
	memset(txq, 0, sizeof(*txq));
}

static int
txq_complete(struct txq *txq)
{
	unsigned int wrs_comp = txq->wrs_comp;
	unsigned int wrs_free = txq->wrs_free;
	int wcs_n;
	int i;
	int ret = 0;

	if (wrs_comp == 0) {
		assert(wrs_free == txq->wrs_n);
		return 0;
	}
#ifdef DEBUG_SEND
	DEBUG("%p: processing %u completed work requests",
	      (void *)txq, wrs_comp);
#endif
	assert((wrs_comp + wrs_free) == txq->wrs_n);

	/* XXX careful with stack overflows. */
	struct ibv_wc wcs[wrs_comp];
	struct rte_mbuf *(*bufs)[] = txq->bufs;

	wcs_n = ibv_poll_cq(txq->cq, wrs_comp, wcs);
	if (wcs_n == 0)
		return 0;
	if (wcs_n < 0) {
		DEBUG("txq=%p, ibv_poll_cq() failed (wc_n=%d)",
		      (void *)txq, wcs_n);
		return -1;
	}
	for (i = 0; (i != wcs_n); ++i) {
		struct ibv_wc *wc = &wcs[i];
		uint64_t wr_id = wc->wr_id;
		struct ibv_send_wr *wr = &(*txq->wrs)[wr_id];
		unsigned int sge_id = (wr_id * txq->sges_wr_n);
		int j;

		assert(wr_id < txq->wrs_n);
		if (wc->status != IBV_WC_SUCCESS) {
			DEBUG("txq=%p, wr_id=%" PRIu64 ": bad work completion"
			      " status (%d): %s",
			      (void *)txq, wc->wr_id, wc->status,
			      ibv_wc_status_str(wc->status));
			/* We can't do much about this. */
			ret = -1;
#ifdef MLX4_PMD_SOFT_COUNTERS
			/* Increase dropped packets counter. */
			++txq->stats.odropped;
#endif
		}
		else
			assert(wc->opcode == IBV_WC_SEND);
		/*
		 * XXX check number of bytes transferred (wc->byte_len).
		 * XXX for some reason, wc->byte_len is always 0.
		 */
		/* Free associated SGEs mbufs. */
		assert(wr->num_sge > 0);
		assert((unsigned int)wr->num_sge <= txq->sges_wr_n);
		for (j = 0; (j < wr->num_sge); ++j, ++sge_id) {
			struct rte_mbuf *m = (*bufs)[sge_id];

			assert(sge_id < txq->sges_n);
			(*bufs)[sge_id] = NULL;
			assert(m != NULL);
			/* Make sure this segment is unlinked. */
			m->pkt.next = NULL;
			rte_pktmbuf_free_seg(m);
#ifndef NDEBUG
			/* SGE poisoning shouldn't hurt. */
			memset(&(*txq->sges)[sge_id], 0x44,
			       sizeof((*txq->sges)[sge_id]));
#endif
		}
		--wrs_comp;
		++wrs_free;
	}
	txq->wrs_comp = wrs_comp;
	txq->wrs_free = wrs_free;
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
		if (txq->mp2mr[i].mp == NULL) {
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
	mr = ibv_reg_mr(txq->pd, mp, mp_size,
			(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
	if (mr == NULL) {
		DEBUG("%p: unable to configure MR, ibv_reg_mr() failed.",
		      (void *)txq);
		return (uint32_t)-1;
	}
	if (i == elemof(txq->mp2mr)) {
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
	struct ibv_sge (*sges)[] = txq->sges;
	struct rte_mbuf *(*bufs)[] = txq->bufs;
	struct ibv_send_wr dummy;
	struct ibv_send_wr **next = &dummy.next;
	struct ibv_send_wr *bad_wr;
	uint64_t wr_id;
	unsigned int i;
	unsigned int max;
	int ret = 0;
	int err;

	txq_complete(txq);
	if (pkts_n == 0)
		return 0;
	max = txq->wrs_free;
	if (max == 0) {
#ifdef DEBUG_SEND
		DEBUG("%p: can't send %u packet(s), no WR(s) available"
		      " (%u waiting for completion)",
		      (void *)txq->priv, pkts_n, txq->wrs_comp);
#endif
		return 0;
	}
	if (max > pkts_n)
		max = pkts_n;
	/* Get the first available WR ID. */
	wr_id = txq->wr_cur;
	for (i = 0; (i != max); ++i) {
		struct ibv_send_wr *wr = &(*txq->wrs)[wr_id];
		struct ibv_sge *sge = &(*sges)[(wr_id * txq->sges_wr_n)];
		struct rte_mbuf **buf = &(*bufs)[(wr_id * txq->sges_wr_n)];
		struct rte_mbuf *m;
		unsigned int seg_n = 0;

		/* Link WRs together for ibv_post_send(). */
		*next = wr;
		next = &wr->next;
		/* Convert each segment of the current packet into a SGE. */
		for (m = pkts[i]; (m != NULL); m = m->pkt.next) {
			uint32_t lkey;

			/* Retrieve Memory Region key for this memory pool. */
			lkey = txq_mp2mr(txq, m->pool);
			if (lkey == (uint32_t)-1) {
				/* MR doesn't exist, stop here. */
				DEBUG("unable to get MP <-> MR association");
				break;
			}
			if (seg_n == txq->sges_wr_n) {
				DEBUG("too many segments for packet (maximum"
				      " is %u)",
				      txq->sges_wr_n);
				break;
			}
			/* Ignore empty segments (except the first one). */
			if ((m->pkt.data_len == 0) && (m != pkts[i]))
				continue;
			/* Update SGE. */
			buf[seg_n] = m;
			sge[seg_n].addr = (uintptr_t)m->pkt.data;
			sge[seg_n].length = m->pkt.data_len;
			sge[seg_n].lkey = lkey;
			/* Increase number of segments (SGEs). */
			++seg_n;
#ifdef MLX4_PMD_SOFT_COUNTERS
			/* Increase sent bytes counter. */
			txq->stats.obytes += m->pkt.data_len;
#endif
		}
#ifdef MLX4_PMD_SOFT_COUNTERS
		/* Increase sent packets counter. */
		++txq->stats.opackets;
#endif
		/* Update WR. */
		assert(wr->wr_id == wr_id);
		assert(wr->sg_list == sge);
		wr->num_sge = seg_n;
		assert(wr->opcode == IBV_WR_SEND);
		/* Update WR index. */
		if (++wr_id == txq->wrs_n)
			wr_id = 0;
	}
	*next = NULL;
	err = ibv_post_send(txq->qp, dummy.next, &bad_wr);
	if (err) {
		struct ibv_send_wr *wr = dummy.next;

		/*
		 * Recalculate the number of packets that are actually
		 * going to be sent.
		 */
		for (i = 0; ((wr != NULL) && (wr != bad_wr)); ++i)
			wr = wr->next;
		DEBUG("%p: ibv_post_send(): failed for WR %p [%u]"
		      " (only %u out of %u WR(s) posted): %s",
		      (void *)txq->priv,
		      (void *)bad_wr,
		      (unsigned int)(((uintptr_t)bad_wr -
				      (uintptr_t)&(*txq->wrs)[0]) /
				     sizeof((*txq->wrs)[0])),
		      i, max,
		      strerror(err));
	}
	txq->wrs_comp += i;
	txq->wrs_free -= i;
	/* Recalculate txq->wr_cur. */
	wr_id = (txq->wr_cur + i);
	while (wr_id >= txq->wrs_n)
		wr_id -= txq->wrs_n;
	txq->wr_cur = wr_id;
	/* Sanity checks. */
	assert(txq->wrs_comp <= txq->wrs_n);
	assert(txq->wrs_free <= txq->wrs_n);
	/* Increase dropped packets counter for ignored packets. */
	txq->stats.odropped += (pkts_n - i);
	ret = i;
	return ret;
}

static int
mlx4_tx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_txconf *conf)
{
	struct priv *priv = dev->data->dev_private;
	struct txq *txq = &(*priv->txqs)[idx];
	struct txq tmpl = {
		.priv = priv
	};
	union {
		struct ibv_qp_init_attr init;
		struct ibv_qp_attr mod;
	} attr;
	int ret = 0;

	(void)socket; /* CPU socket for DMA allocation (ignored). */
	(void)conf; /* Thresholds configuration (ignored). */
	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if ((desc == 0) || (desc % MLX4_PMD_SGE_WR_N)) {
		DEBUG("%p: invalid number of TX descriptors (must be a"
		      " multiple of %d)", (void *)dev, desc);
		return -EINVAL;
	}
	desc /= MLX4_PMD_SGE_WR_N;
	if (idx >= priv->txqs_n) {
		DEBUG("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->txqs_n);
		return -EOVERFLOW;
	}
	tmpl.pd = ibv_alloc_pd(priv->ctx);
	if (tmpl.pd == NULL) {
		ret = ENOMEM;
		DEBUG("%p: PD creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
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
		/* Get completion events. */
		.sq_sig_all = 1 /* XXX */
	};
	tmpl.qp = ibv_create_qp(tmpl.pd, &attr.init);
	if (tmpl.qp == NULL) {
		ret = ENOMEM;
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
	if ((ret = txq_alloc_wrs(&tmpl, desc, MLX4_PMD_SGE_WR_N))) {
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

/* RX queues handling. */

static int
rxq_alloc_wrs(struct rxq *rxq, unsigned int wrs_n, unsigned int sges_wr_n)
{
	unsigned int i;
	unsigned int sges_n = (wrs_n * sges_wr_n);
	struct ibv_recv_wr (*wrs)[wrs_n] = calloc(1, sizeof(*wrs));
	struct ibv_recv_wr *wr;
	struct ibv_sge (*sges)[sges_n] = calloc(1, sizeof(*sges));
	struct ibv_sge *sge;
	struct rte_mbuf *(*bufs)[sges_n] = calloc(1, sizeof(*bufs));
	struct rte_mbuf *buf;
	int ret = 0;

	if ((wrs == NULL) || (sges == NULL) || (bufs == NULL)) {
		DEBUG("%p: can't allocate something: wrs=%p sges=%p bufs=%p",
		      (void *)rxq, (void *)wrs, (void *)sges, (void *)bufs);
		ret = ENOMEM;
		goto error;
	}
	for (i = 0; (i != sges_n); ++i) {
		sge = &(*sges)[i];
		buf = rte_pktmbuf_alloc(rxq->mp);
		(*bufs)[i] = buf;
		if (buf == NULL) {
			DEBUG("%p: empty mbuf pool", (void *)rxq);
			ret = ENOMEM;
			goto error;
		}
		/* In the unlikely case uint64_t can't store a pointer... */
		assert(sizeof(sge->addr) >= sizeof(uintptr_t));
		/* Headroom is normally reserved by rte_pktmbuf_alloc(). */
		assert(((uintptr_t)buf->buf_addr + RTE_PKTMBUF_HEADROOM) ==
		       (uintptr_t)buf->pkt.data);
		/* Buffer is supposed to be empty. */
		assert(rte_pktmbuf_data_len(buf) == 0);
		assert(rte_pktmbuf_pkt_len(buf) == 0);
		/* Configure SGE without headroom. */
		sge->addr = (uintptr_t)buf->buf_addr;
		sge->length = buf->buf_len;
		sge->lkey = rxq->mr->lkey;
		if ((i % sges_wr_n) == 0) {
			/*
			 * Take headroom into account if this is the first
			 * buffer of a SGEs list.
			 */
			sge->addr += RTE_PKTMBUF_HEADROOM;
			sge->length -= RTE_PKTMBUF_HEADROOM;
		}
		else {
			/* Otherwise remove headroom for subsequent SGEs. */
			assert(((uintptr_t)buf->pkt.data -
				RTE_PKTMBUF_HEADROOM) ==
			       (uintptr_t)buf->buf_addr);
			buf->pkt.data = buf->buf_addr;
		}
		/* Redundant check to make sure tailroom works as intended. */
		assert(sge->length == rte_pktmbuf_tailroom(buf));
	}
	DEBUG("%p: allocated %u SGEs and mbufs (%u WRs with %u SGEs)",
	      (void *)rxq, sges_n, wrs_n, sges_wr_n);
	for (i = 0; (i != wrs_n); ++i) {
		wr = &(*wrs)[i];
		wr->wr_id = i;
		wr->next = &(*wrs)[(i + 1)];
		wr->sg_list = &(*sges)[(i * sges_wr_n)];
		wr->num_sge = sges_wr_n;
	}
	wr->next = NULL; /* The last pointer must be NULL. */
	DEBUG("%p: configured %u WRs", (void *)rxq, wrs_n);
	rxq->wrs = wrs;
	rxq->sges = sges;
	rxq->bufs = bufs;
	rxq->wrs_n = wrs_n;
	rxq->sges_n = sges_n;
	rxq->sges_wr_n = sges_wr_n;
	assert(ret == 0);
	return 0;
error:
	free(wrs);
	free(sges);
	if (bufs != NULL) {
		for (i = 0; (i != sges_n); ++i) {
			if ((*bufs)[i] != NULL)
				rte_pktmbuf_free_seg((*bufs)[i]);
		}
		free(bufs);
	}
	DEBUG("%p: failed, freed everything", (void *)rxq);
	assert(ret != 0);
	return ret;
}

static void
rxq_free_wrs(struct rxq *rxq)
{
	unsigned int i;
	struct ibv_recv_wr (*wrs)[] = rxq->wrs;
	struct ibv_sge (*sges)[] = rxq->sges;
	struct rte_mbuf *(*bufs)[] = rxq->bufs;

	DEBUG("%p: freeing WRs and SGEs", (void *)rxq);
	rxq->wrs = NULL;
	rxq->sges = NULL;
	rxq->bufs = NULL;
	free(wrs);
	free(sges);
	if (bufs == NULL)
		return;
	for (i = 0; (i != rxq->sges_n); ++i) {
		if ((*bufs)[i] != NULL)
			rte_pktmbuf_free_seg((*bufs)[i]);
	}
	free(bufs);
}

static void
rxq_mac_addr_del(struct rxq *rxq, unsigned int mac_index)
{
	struct priv *priv = rxq->priv;
	const uint8_t (*mac)[ETHER_ADDR_LEN] =
		(const uint8_t (*)[ETHER_ADDR_LEN])
		priv->mac[mac_index].addr_bytes;
	unsigned int i;

	assert(mac_index < elemof(priv->mac));
	if (!BITFIELD_ISSET(rxq->mac_configured, mac_index))
		return;
	/*
	 * Set i as the index of either the first VLAN filter enabled in
	 * priv->vlan_filter[], or the last array entry. It doesn't matter if
	 * that entry is enabled or not, we only need it to loop at least once
	 * in the following do {} block.
	 */
	for (i = 0; (i != (elemof(priv->vlan_filter) - 1)); ++i)
		if (priv->vlan_filter[i].enabled)
			break;
	/* For each configured VLAN filter... */
	do {
		unsigned int vlan = priv->vlan_filter[i].id;
		unsigned int vlan_present = priv->vlan_filter[i].enabled;
		struct ibv_flow_spec spec = {
			.type = IBV_FLOW_ETH,
			.l2_id = {
				.eth = {
					.ethertype = htons(0x0800),
					.vlan = (vlan_present ? vlan : 0),
					.vlan_present =  vlan_present,
					.mac = {
						(*mac)[0], (*mac)[1],
						(*mac)[2], (*mac)[3],
						(*mac)[4], (*mac)[5]
					},
					.port = priv->port
				}
			},
			.l4_protocol = IBV_FLOW_L4_NONE
		};

		DEBUG("rxq=%p: removing MAC address"
		      " %02x:%02x:%02x:%02x:%02x:%02x from index %u"
		      " (VLAN filter %s, ID: %u)",
		      (void *)rxq,
		      (*mac)[0], (*mac)[1], (*mac)[2],
		      (*mac)[3], (*mac)[4], (*mac)[5],
		      mac_index,
		      (vlan_present ? "enabled" : "disabled"),
		      (vlan_present ? vlan : 0));
		/* Detach related flow. */
		claim_zero(ibv_detach_flow(rxq->qp, &spec, 0));
		/* Get the next VLAN filter index (if any). */
		while ((++i) != elemof(priv->vlan_filter))
			if (priv->vlan_filter[i].enabled)
				break;
	}
	while (i != elemof(priv->vlan_filter));
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
	unsigned int i;

	assert(mac_index < elemof(priv->mac));
	if (BITFIELD_ISSET(rxq->mac_configured, mac_index))
		rxq_mac_addr_del(rxq, mac_index);
	/*
	 * Set i as the index of either the first VLAN filter enabled in
	 * priv->vlan_filter[], or the last array entry. It doesn't matter if
	 * that entry is enabled or not, we only need it to loop at least once
	 * in the following do {} block.
	 */
	for (i = 0; (i != (elemof(priv->vlan_filter) - 1)); ++i)
		if (priv->vlan_filter[i].enabled)
			break;
	/* For each configured VLAN filter... */
	do {
		unsigned int vlan = priv->vlan_filter[i].id;
		unsigned int vlan_present = priv->vlan_filter[i].enabled;
		struct ibv_flow_spec spec = {
			.type = IBV_FLOW_ETH,
			.l2_id = {
				.eth = {
					.ethertype = htons(0x0800),
					.vlan = (vlan_present ? vlan : 0),
					.vlan_present = vlan_present,
					.mac = {
						(*mac)[0], (*mac)[1],
						(*mac)[2], (*mac)[3],
						(*mac)[4], (*mac)[5]
					},
					.port = priv->port
				}
			},
			.l4_protocol = IBV_FLOW_L4_NONE
		};
		int ret;

		DEBUG("rxq=%p: adding MAC address"
		      " %02x:%02x:%02x:%02x:%02x:%02x from index %u"
		      " (VLAN filter %s, ID: %u)",
		      (void *)rxq,
		      (*mac)[0], (*mac)[1], (*mac)[2],
		      (*mac)[3], (*mac)[4], (*mac)[5],
		      mac_index,
		      (vlan_present ? "enabled" : "disabled"),
		      (vlan_present ? vlan : 0));
		/* Attach related flow. */
		if ((ret = ibv_attach_flow(rxq->qp, &spec, 0))) {
			/* Failure, rollback. */
			DEBUG("rxq=%p: failed adding MAC address",
			      (void *)rxq);
			while (i != 0) {
				--i;
				if (!priv->vlan_filter[i].enabled)
					continue;
				assert(spec.l2_id.eth.vlan_present);
				vlan = priv->vlan_filter[i].id;
				spec.l2_id.eth.vlan = vlan;
				/* Detach related flow. */
				claim_zero(ibv_detach_flow(rxq->qp, &spec, 0));
			}
			return ret;
		}
		/* Get the next VLAN filter index (if any). */
		while ((++i) != elemof(priv->vlan_filter))
			if (priv->vlan_filter[i].enabled)
				break;
	}
	while (i != elemof(priv->vlan_filter));
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
	for (i = 0; (i != priv->rxqs_n); ++i)
		rxq_mac_addr_del(&(*priv->rxqs)[i], mac_index);
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
		for (i = 0; (i != priv->rxqs_n); ++i)
			assert(!BITFIELD_ISSET
			       ((*priv->rxqs)[i].mac_configured, mac_index));
#endif
		goto end;
	}
	for (i = 0; (i != priv->rxqs_n); ++i) {
		ret = rxq_mac_addr_add(&(*priv->rxqs)[i], mac_index);
		if (!ret)
			continue;
		/* Failure, rollback. */
		while (i != 0)
			rxq_mac_addr_del(&(*priv->rxqs)[(--i)], mac_index);
		return ret;
	}
end:
	BITFIELD_SET(priv->mac_configured, mac_index);
	return 0;
}

static void
rxq_cleanup(struct rxq *rxq)
{
	DEBUG("cleaning up %p", (void *)rxq);
	rxq_free_wrs(rxq);
	if (rxq->qp != NULL) {
		rxq_mac_addrs_del(rxq);
		claim_zero(ibv_destroy_qp(rxq->qp));
	}
	if (rxq->cq != NULL)
		claim_zero(ibv_destroy_cq(rxq->cq));
	if (rxq->mr != NULL)
		claim_zero(ibv_dereg_mr(rxq->mr));
	if (rxq->pd != NULL)
		claim_zero(ibv_dealloc_pd(rxq->pd));
	memset(rxq, 0, sizeof(*rxq));
}

static uint16_t
mlx4_rx_burst(dpdk_rxq_t *dpdk_rxq, struct rte_mbuf **pkts, uint16_t pkts_n)
{
	struct rxq *rxq = (struct rxq *)dpdk_rxq;
	struct ibv_sge (*sges)[] = rxq->sges;
	struct rte_mbuf *(*bufs)[] = rxq->bufs;
	struct ibv_wc wcs[pkts_n];
	struct ibv_recv_wr dummy;
	struct ibv_recv_wr **next = &dummy.next;
	struct ibv_recv_wr *bad_wr;
	int ret = 0;
	int wcs_n;
	int i;

	wcs_n = ibv_poll_cq(rxq->cq, pkts_n, wcs);
	if (wcs_n == 0)
		return 0;
	if (wcs_n < 0) {
		DEBUG("rxq=%p, ibv_poll_cq() failed (wc_n=%d)",
		      (void *)rxq, wcs_n);
		return -1;
	}
	assert(wcs_n <= (int)pkts_n);
	for (i = 0; (i != wcs_n); ++i) {
		struct ibv_wc *wc = &wcs[i];
		uint64_t wr_id = wc->wr_id;
		struct ibv_recv_wr *wr = &(*rxq->wrs)[wr_id];
		size_t idx = (wr_id * rxq->sges_wr_n);
		size_t j;
		uint32_t sz;
		struct rte_mbuf *newbuf; /* Replacement mbuf. */
		struct rte_mbuf *pktbuf; /* Returned mbuf. */

		/* Sanity check. */
		assert(wr_id < rxq->wrs_n);
		assert(wr_id == wr->wr_id);
		assert(wr->sg_list == &(*sges)[idx]);
		assert(wr->num_sge > 0);
		/* Link completed WRs together for repost. */
		*next = wr;
		next = &wr->next;
		if (wc->status != IBV_WC_SUCCESS) {
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
		 * For each used SGE, allocate a new mbuf. If successful,
		 * link old mbufs together and return them in the pkt array.
		 * Finally, update SGEs with the new empty mbufs.
		 */
		sz = wc->byte_len;
		newbuf = NULL;
		while (1) {
			struct rte_mbuf *seg = rte_pktmbuf_alloc(rxq->mp);
			uint32_t seg_room;

			if (seg == NULL) {
				/*
				 * Unable to allocate a replacement mbuf,
				 * repost WR.
				 */
				DEBUG("rxq=%p, wr_id=%" PRIu64 ":"
				      " can't allocate a new mbuf",
				      (void *)rxq, wr_id);
				if (newbuf != NULL)
					rte_pktmbuf_free(newbuf);
				/* Increase out of memory counter. */
				++rxq->stats.rx_nombuf;
				goto repost;
			}
			seg->pkt.next = newbuf;
			newbuf = seg;
			seg_room = (newbuf->buf_len -
				    ((uintptr_t)newbuf->pkt.data -
				     (uintptr_t)newbuf->buf_addr));
			assert(seg_room == rte_pktmbuf_tailroom(newbuf));
			if (sz <= seg_room)
				break;
			sz -= seg_room;
		}
		/*
		 * Extract spent mbufs, link them, fix their size, and
		 * replace them with new mbufs.
		 */
		pktbuf = (*bufs)[idx];
		sz = wc->byte_len;
		j = idx;
		while (1) {
			struct rte_mbuf *seg = (*bufs)[j]; /* Spent segment. */
			struct ibv_sge *sge = &(*sges)[j]; /* Spent SGE. */
			uint32_t seg_room = (seg->buf_len -
					     ((uintptr_t)seg->pkt.data -
					      (uintptr_t)seg->buf_addr));

			assert(seg_room == rte_pktmbuf_tailroom(seg));
			/* Affect replacement segment. */
			(*bufs)[j] = newbuf;
			/* Use the same headroom as seg. */
			assert(rte_pktmbuf_data_len(newbuf) == 0);
			assert(rte_pktmbuf_pkt_len(newbuf) == 0);
			newbuf->pkt.data = (void *)
				((uintptr_t)newbuf->buf_addr +
				 ((uintptr_t)seg->pkt.data -
				  (uintptr_t)seg->buf_addr));
			/* Reconfigure SGE for newbuf. */
			sge->addr = (uintptr_t)newbuf->pkt.data;
			assert(sge->length == seg_room);
			assert(sge->lkey == rxq->mr->lkey);
			/* Update spent segment port ID. */
			seg->pkt.in_port = rxq->port_id;
			/* Fetch next replacement segment. */
			newbuf = newbuf->pkt.next;
			/* Unlink current replacement segment. */
			(*bufs)[j]->pkt.next = NULL;
			if (sz <= seg_room) {
				/* Last segment. */
				seg->pkt.next = NULL;
				assert(seg_room <= rte_pktmbuf_tailroom(seg));
				seg->pkt.data_len = sz;
				break;
			}
			/* Sanity check. */
			assert(newbuf != NULL);
			assert(seg->pkt.next == NULL);
			assert(seg_room <= rte_pktmbuf_tailroom(seg));
			/* Update seg. */
			seg->pkt.data_len = seg_room;
			seg->pkt.next = (*bufs)[(++j)];
			sz -= seg_room;
		}
		assert(newbuf == NULL);
		pktbuf->pkt.pkt_len = wc->byte_len;
		*(pkts++) = pktbuf;
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
	      (void *)dev, wcs_n, wcs[0].wr_id, (void *)dummy.next);
#endif
	i = ibv_post_recv(rxq->qp, dummy.next, &bad_wr);
	if (i) {
		/* Inability to repost WRs is fatal. */
		DEBUG("%p: ibv_post_recv(): failed for WR %p [%u]: %s",
		      (void *)rxq->priv,
		      (void *)bad_wr,
		      (unsigned int)(((uintptr_t)bad_wr -
				      (uintptr_t)&(*rxq->wrs)[0]) /
				     sizeof((*rxq->wrs)[0])),
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
mlx4_rx_queue_setup(struct rte_eth_dev *dev, uint16_t idx, uint16_t desc,
		    unsigned int socket, const struct rte_eth_rxconf *conf,
		    struct rte_mempool *mp)
{
	struct priv *priv = dev->data->dev_private;
	struct rxq *rxq = &(*priv->rxqs)[idx];
	struct rxq tmpl = {
		.priv = priv,
		.mp = mp
	};
	union {
		struct ibv_qp_init_attr init;
		struct ibv_qp_attr mod;
	} attr;
	struct ibv_recv_wr *bad_wr;
	int ret = 0;

	(void)socket; /* CPU socket for DMA allocation (ignored). */
	(void)conf; /* Thresholds configuration (ignored). */
	DEBUG("%p: configuring queue %u for %u descriptors",
	      (void *)dev, idx, desc);
	if ((desc == 0) || (desc % MLX4_PMD_SGE_WR_N)) {
		DEBUG("%p: invalid number of RX descriptors (must be a"
		      " multiple of %d)", (void *)dev, desc);
		return -EINVAL;
	}
	desc /= MLX4_PMD_SGE_WR_N;
	if (idx >= priv->rxqs_n) {
		DEBUG("%p: queue index out of range (%u >= %u)",
		      (void *)dev, idx, priv->rxqs_n);
		return -EOVERFLOW;
	}
	tmpl.pd = ibv_alloc_pd(priv->ctx);
	if (tmpl.pd == NULL) {
		ret = ENOMEM;
		DEBUG("%p: PD creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	/* Get mempool size. */
	tmpl.mp_size = mp_total_size(mp);
	/* Use the entire RX mempool as the memory region. */
	tmpl.mr = ibv_reg_mr(tmpl.pd, mp, tmpl.mp_size,
			     (IBV_ACCESS_LOCAL_WRITE |
			      IBV_ACCESS_REMOTE_WRITE));
	if (tmpl.mr == NULL) {
		ret = ENOMEM;
		DEBUG("%p: MR creation failure: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
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
	tmpl.qp = ibv_create_qp(tmpl.pd, &attr.init);
	if (tmpl.qp == NULL) {
		ret = ENOMEM;
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
	/* Configure MAC and broadcast addresses. */
	if ((ret = rxq_mac_addrs_add(&tmpl))) {
		DEBUG("%p: QP flow attachment failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	if ((ret = rxq_alloc_wrs(&tmpl, desc, MLX4_PMD_SGE_WR_N))) {
		DEBUG("%p: RXQ allocation failed: %s",
		      (void *)dev, strerror(ret));
		goto error;
	}
	if ((ret = ibv_post_recv(tmpl.qp, *tmpl.wrs, &bad_wr))) {
		DEBUG("%p: ibv_post_recv() failed for WR %p [%u]: %s",
		      (void *)dev,
		      (void *)bad_wr,
		      (unsigned int)(((uintptr_t)bad_wr -
				      (uintptr_t)&(*tmpl.wrs)[0]) /
				     sizeof((*tmpl.wrs)[0])),
		      strerror(ret));
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
	/* Save port ID. */
	tmpl.port_id = dev->data->port_id;
	DEBUG("%p: RTE port ID: %u", (void *)rxq, tmpl.port_id);
	/* Clean up rxq in case we're reinitializing it. */
	DEBUG("%p: cleaning-up old rxq just in case", (void *)rxq);
	rxq_cleanup(rxq);
	*rxq = tmpl;
	DEBUG("%p: rxq updated with %p", (void *)rxq, (void *)&tmpl);
	assert(ret == 0);
	dev->rx_pkt_burst = mlx4_rx_burst;
	return 0;
error:
	rxq_cleanup(&tmpl);
	assert(ret != 0);
	return -ret; /* Negative errno value. */
}

/* Simulate device start by attaching all configured flows. */
static int
mlx4_dev_start(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;

	if (priv->started)
		return 0;
	DEBUG("%p: attaching configured flows to all RX queues", (void *)dev);
	priv->started = 1;
	for (i = 0; (i != priv->rxqs_n); ++i) {
		int ret = rxq_mac_addrs_add(&(*priv->rxqs)[i]);

		if (ret == 0)
			continue;
		DEBUG("%p: QP flow attachment failed: %s",
		      (void *)dev, strerror(ret));
		/* Rollback. */
		while (i != 0)
			rxq_mac_addrs_del(&(*priv->rxqs)[(--i)]);
		priv->started = 0;
		return -1;
	}
	return 0;
}

/* Simulate device stop by detaching all configured flows. */
static void
mlx4_dev_stop(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	unsigned int i;

	if (!priv->started)
		return;
	DEBUG("%p: detaching flows from all RX queues", (void *)dev);
	priv->started = 0;
	for (i = 0; (i != priv->rxqs_n); ++i)
		rxq_mac_addrs_del(&(*priv->rxqs)[i]);
}

/* See mlx4_dev_infos_get() and mlx4_dev_close(). */
static void *removed_queue[512];

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
	unsigned int i;

	DEBUG("%p: closing device \"%s\"",
	      (void *)dev, priv->ctx->device->name);
	/* Prevent crashes when queues are still in use. */
	dev->rx_pkt_burst = removed_rx_burst;
	dev->tx_pkt_burst = removed_tx_burst;
	if (priv->rxqs != NULL) {
		/* XXX race condition if mlx4_rx_burst() is still running. */
		usleep(1000);
		dev->data->nb_rx_queues = 0;
		dev->data->rx_queues = (void *)removed_queue;
		for (i = 0; (i != priv->rxqs_n); ++i)
			rxq_cleanup(&(*priv->rxqs)[i]);
		free(priv->rxqs);
	}
	if (priv->txqs != NULL) {
		/* XXX race condition if mlx4_tx_burst() is still running. */
		usleep(1000);
		dev->data->nb_tx_queues = 0;
		dev->data->tx_queues = (void *)removed_queue;
		for (i = 0; (i != priv->txqs_n); ++i)
			txq_cleanup(&(*priv->txqs)[i]);
		free(priv->txqs);
	}
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
	/*
	 * The number of dummy queue pointers available to replace them
	 * during mlx4_dev_close() also limit this value.
	 */
	max = ((max > elemof(removed_queue)) ? elemof(removed_queue) : max);
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
		tmp.ipackets += (*priv->rxqs)[i].stats.ipackets;
		tmp.ibytes += (*priv->rxqs)[i].stats.ibytes;
		tmp.ierrors += (*priv->rxqs)[i].stats.idropped;
		tmp.rx_nombuf += (*priv->rxqs)[i].stats.rx_nombuf;
	}
	for (i = 0; (i != priv->txqs_n); ++i) {
		tmp.opackets += (*priv->txqs)[i].stats.opackets;
		tmp.obytes += (*priv->txqs)[i].stats.obytes;
		tmp.oerrors += (*priv->txqs)[i].stats.odropped;
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

	for (i = 0; (i != priv->rxqs_n); ++i)
		(*priv->rxqs)[i].stats =
			(struct rte_rxq_stats){ .ipackets = 0 };
	for (i = 0; (i != priv->txqs_n); ++i)
		(*priv->txqs)[i].stats =
			(struct rte_txq_stats){ .opackets = 0 };
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
	*stats = (*priv->rxqs)[idx].stats;
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
	*stats = (*priv->txqs)[idx].stats;
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: add hardware counters. */
#endif
}

static void
mlx4_rxq_stats_reset(struct rte_eth_dev *dev, uint16_t idx)
{
	const struct priv *priv = dev->data->dev_private;

	assert(idx < priv->rxqs_n);
	(*priv->rxqs)[idx].stats = (struct rte_rxq_stats){ .ipackets = 0 };
#ifndef MLX4_PMD_SOFT_COUNTERS
	/* FIXME: reset hardware counters. */
#endif
}

static void
mlx4_txq_stats_reset(struct rte_eth_dev *dev, uint16_t idx)
{
	const struct priv *priv = dev->data->dev_private;

	assert(idx < priv->txqs_n);
	(*priv->txqs)[idx].stats = (struct rte_txq_stats){ .opackets = 0 };
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

	if (priv->promisc)
		return;
	priv->promisc = 1;
	/* FIXME */
}

static void
mlx4_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;

	if (!priv->promisc)
		return;
	priv->promisc = 0;
	/* FIXME */
}

static void
mlx4_allmulticast_enable(struct rte_eth_dev *dev)
{
	(void)dev;
	/* FIXME */
}

static void
mlx4_allmulticast_disable(struct rte_eth_dev *dev)
{
	(void)dev;
	/* FIXME */
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

static void
mlx4_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
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
		return;
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
		for (i = 0; (i != priv->rxqs_n); ++i)
			rxq_mac_addrs_del(&(*priv->rxqs)[i]);
		priv->vlan_filter[j].enabled = 1;
		if (priv->started)
			for (i = 0; (i != priv->rxqs_n); ++i)
				rxq_mac_addrs_add(&(*priv->rxqs)[i]);
	}
	else if ((!on) && (priv->vlan_filter[j].enabled)) {
		/*
		 * Filter is enabled, disable it.
		 * Rehashing flows in all RX queues is necessary.
		 */
		for (i = 0; (i != priv->rxqs_n); ++i)
			rxq_mac_addrs_del(&(*priv->rxqs)[i]);
		priv->vlan_filter[j].enabled = 0;
		if (priv->started)
			for (i = 0; (i != priv->rxqs_n); ++i)
				rxq_mac_addrs_add(&(*priv->rxqs)[i]);
	}
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
#ifdef DPDK_6WIND
	.rxq_stats_get = mlx4_rxq_stats_get,
	.txq_stats_get = mlx4_txq_stats_get,
	.rxq_stats_reset = mlx4_rxq_stats_reset,
	.txq_stats_reset = mlx4_txq_stats_reset,
#endif /* DPDK_6WIND */
	.dev_infos_get = mlx4_dev_infos_get,
	.vlan_filter_set = mlx4_vlan_filter_set,
	.rx_queue_setup = mlx4_rx_queue_setup,
	.tx_queue_setup = mlx4_tx_queue_setup,
	.dev_led_on = NULL,
	.dev_led_off = NULL,
	.flow_ctrl_set = NULL,
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
	struct rte_eth_dev *dev; /* associated PCI device */
	uint32_t ports; /* physical ports bitfield. */
} mlx4_dev[32];

/* Return mlx4_dev[] index, or -1 on error. */
static int
mlx4_dev_idx(struct rte_eth_dev *dev)
{
	unsigned int i;
	int ret = -1;

	assert(dev != NULL);
	for (i = 0; (i != elemof(mlx4_dev)); ++i) {
		if (mlx4_dev[i].dev == dev)
			return i;
		if ((mlx4_dev[i].dev == NULL) && (ret == -1))
			ret = i;
	}
	return ret;
}

static struct eth_driver mlx4_driver;

static int
mlx4_dev_init(struct eth_driver *drv, struct rte_eth_dev *dev)
{
	struct priv *priv = dev->data->dev_private;
	struct ibv_device **list;
	int err = errno;
	uint32_t port = 1; /* ports are indexed from one */
	uint32_t test = 1;
	struct ibv_context *ctx = NULL;
	struct ibv_device_attr device_attr;
	int idx;
	int i;

	assert(drv == &mlx4_driver);
	/* Get mlx4_dev[] index. */
	idx = mlx4_dev_idx(dev);
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
	if (ibv_query_device(ctx, &device_attr)) {
		err = errno;
		ibv_close_device(ctx);
		errno = err;
		return -err;
	}
	DEBUG("%u port(s) detected", device_attr.phys_port_cnt);
	if ((port - 1) >= device_attr.phys_port_cnt) {
		DEBUG("port %u not available, skipping interface", port);
		return -ENODEV;
	}
	DEBUG("using port %u (%08" PRIx32 ")", port, test);
	mlx4_dev[idx].ports |= test;
	memset(priv, 0, sizeof(*priv));
	priv->dev = dev;
	priv->ctx = ctx;
	priv->device_attr = device_attr;
	priv->port = port;
	priv->mtu = 1500; /* Unused MTU. */
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
}

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
