/*
 * Copyright 6WIND 2012-2013, All rights reserved.
 * Copyright Mellanox 2012, All rights reserved.
 */

#ifndef RTE_PMD_MLX4_H_
#define RTE_PMD_MLX4_H_

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

/*
 * Maximum number of simultaneous MAC addresses supported.
 *
 * According to ConnectX's Programmer Reference Manual:
 *   The L2 Address Match is implemented by comparing a MAC/VLAN combination
 *   of 128 MAC addresses and 127 VLAN values, comprising 128x127 possible
 *   L2 addresses.
 */
#define MLX4_MAX_MAC_ADDRESSES 128

/* Maximum number of simultaneous VLAN filters supported. See above. */
#define MLX4_MAX_VLAN_IDS 127

/* Maximum number of Scatter/Gather Elements per Work Request. */
#ifndef MLX4_PMD_SGE_WR_N
#define MLX4_PMD_SGE_WR_N 4
#endif

/* Maximum size for inline data. */
#ifndef MLX4_PMD_MAX_INLINE
#define MLX4_PMD_MAX_INLINE 0
#endif

/*
 * Maximum number of cached Memory Pools (MPs) per TX queue. Each RTE MP
 * from which buffers are to be transmitted will have to be mapped by this
 * driver to their own Memory Region (MR). This is a slow operation.
 *
 * This value is always 1 for RX queues.
 */
#define MLX4_PMD_TX_MP_CACHE 8

/*
 * If defined, only use software counters. The PMD will never ask the hardware
 * for these, and many of them won't be available.
 */
#define MLX4_PMD_SOFT_COUNTERS 1

/*
 * If defined, enable VMware compatibility code. It also requires the
 * environment variable MLX4_COMPAT_VMWARE set to a nonzero value at runtime.
 */
#define MLX4_COMPAT_VMWARE 1

#define PCI_VENDOR_ID_MELLANOX 0x15b3
#define PCI_DEVICE_ID_MELLANOX_CONNECTX3 0x1003
#define PCI_DEVICE_ID_MELLANOX_CONNECTX3PRO 0x1007
#define PCI_DEVICE_ID_MELLANOX_CONNECTX3VF 0x1004

/* Bit-field manipulation. */
#define BITFIELD_DECLARE(bf, type, size)				\
	type bf[(((size_t)(size) / (sizeof(type) * CHAR_BIT)) +		\
		 !!((size_t)(size) % (sizeof(type) * CHAR_BIT)))]
#define BITFIELD_DEFINE(bf, type, size)					\
	BITFIELD_DECLARE((bf), type, (size)) = { 0 }
#define BITFIELD_SET(bf, b)						\
	(assert((size_t)(b) < (sizeof(bf) * CHAR_BIT)),			\
	 (void)((bf)[((b) / (sizeof((bf)[0]) * CHAR_BIT))] |=		\
		((size_t)1 << ((b) % (sizeof((bf)[0]) * CHAR_BIT)))))
#define BITFIELD_RESET(bf, b)						\
	(assert((size_t)(b) < (sizeof(bf) * CHAR_BIT)),			\
	 (void)((bf)[((b) / (sizeof((bf)[0]) * CHAR_BIT))] &=		\
		~((size_t)1 << ((b) % (sizeof((bf)[0]) * CHAR_BIT)))))
#define BITFIELD_ISSET(bf, b)						\
	(assert((size_t)(b) < (sizeof(bf) * CHAR_BIT)),			\
	 !!(((bf)[((b) / (sizeof((bf)[0]) * CHAR_BIT))] &		\
	     ((size_t)1 << ((b) % (sizeof((bf)[0]) * CHAR_BIT))))))

/* Number of elements in array. */
#define elemof(a) (sizeof(a) / sizeof((a)[0]))

/* Cast pointer p to structure member m to its parent structure of type t. */
#define containerof(p, t, m) ((t *)((uint8_t *)(p) - offsetof(t, m)))

/* Branch prediction helpers. */
#ifndef likely
#define likely(c) __builtin_expect(!!(c), 1)
#endif
#ifndef unlikely
#define unlikely(c) __builtin_expect(!!(c), 0)
#endif

/* Debugging */
#ifndef NDEBUG
#include <stdio.h>
#define DEBUG__(m, ...)						\
	(fprintf(stderr, "%s:%d: %s(): " m "%c",		\
		 __FILE__, __LINE__, __func__, __VA_ARGS__),	\
	 fflush(stderr),					\
	 (void)0)
/*
 * Save/restore errno around DEBUG__().
 * XXX somewhat undefined behavior, but works.
 */
#define DEBUG_(...)				\
	(errno = ((int []){			\
		*(volatile int *)&errno,	\
		(DEBUG__(__VA_ARGS__), 0)	\
	})[0])
#define DEBUG(...) DEBUG_(__VA_ARGS__, '\n')
#define claim_zero(...) assert((__VA_ARGS__) == 0)
#define claim_nonzero(...) assert((__VA_ARGS__) != 0)
#define claim_positive(...) assert((__VA_ARGS__) >= 0)
#else /* NDEBUG */
/* No-ops. */
#define DEBUG(...) (void)0
#define claim_zero(...) (__VA_ARGS__)
#define claim_nonzero(...) (__VA_ARGS__)
#define claim_positive(...) (__VA_ARGS__)
#endif /* NDEBUG */

#endif /* RTE_PMD_MLX4_H_ */
