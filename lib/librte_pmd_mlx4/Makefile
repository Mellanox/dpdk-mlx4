# Copyright 6WIND 2012-2013, All rights reserved.
# Copyright Mellanox 2012, All rights reserved.

prefix ?= /usr/local/dpdk-addons
exec_prefix ?= $(prefix)
libdir ?= $(exec_prefix)/lib
datarootdir ?= $(prefix)/share
docdir ?= $(datarootdir)/doc

# Compile a standalone shared object.
# Only depends on DPDK headers in $RTE_SDK.

OUT = librte_pmd_mlx4.so
O ?= .
SOLIB=$O/$(OUT)

SRC = mlx4.c
HDR = mlx4.h
OBJ = $(SRC:.c=.o)

CC = gcc
CFLAGS += -I$(RTE_SDK)/$(RTE_TARGET)/include
CFLAGS += -I$(O)
CFLAGS += -O3 -std=gnu99 -Wall -Wextra -fPIC
CFLAGS += -D_XOPEN_SOURCE=600
CFLAGS += -g

ifeq ($(DEBUG),1)
CFLAGS += -pedantic -UNDEBUG -DPEDANTIC
else
CFLAGS += -DNDEBUG -UPEDANTIC
endif

ifdef MLX4_PMD_SGE_WR_N
CFLAGS += -DMLX4_PMD_SGE_WR_N=$(MLX4_PMD_SGE_WR_N)
endif

ifdef MLX4_PMD_MAX_INLINE
CFLAGS += -DMLX4_PMD_MAX_INLINE=$(MLX4_PMD_MAX_INLINE)
endif

LDFLAGS += -shared
LIBS += -libverbs

all: warn $(OUT)

install:
	install -D -m 664 $(SOLIB) $(DESTDIR)$(libdir)/$(notdir $(SOLIB))

doc: doc-default
doc-%: FORCE
	@ $(MAKE) -rR --no-print-directory -C doc \
		DOC_TOOLS=$(abspath $(DOC_TOOLS)) \
		O=$(abspath $O)/doc \
		DESTDIR=$(abspath $(DESTDIR))$(docdir) \
		$*

config.h: comp_check.sh
	$(RM) $@
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh -- $< \
		$@ RSS_SUPPORT \
		infiniband/verbs.h enum IBV_EXP_DEVICE_UD_RSS
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh -- $< \
		$@ SEND_RAW_WR_SUPPORT \
		infiniband/verbs.h type 'struct ibv_send_wr_raw'
	CC="$(CC)" \
		CFLAGS="$(CFLAGS) \
			-DRTE_MAX_LCORE=64 \
			-DRTE_PKTMBUF_HEADROOM=128" sh -- $< \
		$@ HAVE_STRUCT_RTE_PKTMBUF \
		rte_mbuf.h type 'struct rte_pktmbuf'
	CC="$(CC)" \
		CFLAGS="$(CFLAGS) \
			-DRTE_MAX_LCORE=64 \
			-DRTE_PKTMBUF_HEADROOM=128 \
			-DRTE_ETHDEV_QUEUE_STAT_CNTRS=2 \
			-DRTE_LOG_LEVEL=0" sh -- $< \
		$@ HAVE_MTU_GET \
		rte_ethdev.h type mtu_get_t
	CC="$(CC)" \
		CFLAGS="$(CFLAGS) \
			-DRTE_MAX_LCORE=64 \
			-DRTE_PKTMBUF_HEADROOM=128 \
			-DRTE_ETHDEV_QUEUE_STAT_CNTRS=2 \
			-DRTE_LOG_LEVEL=0" sh -- $< \
		$@ HAVE_MTU_SET \
		rte_ethdev.h type mtu_set_t
	CC="$(CC)" \
		CFLAGS="$(CFLAGS) \
			-DRTE_MAX_LCORE=64 \
			-DRTE_PKTMBUF_HEADROOM=128 \
			-DRTE_ETHDEV_QUEUE_STAT_CNTRS=2 \
			-DRTE_LOG_LEVEL=0" sh -- $< \
		$@ HAVE_FC_CONF_AUTONEG \
		rte_ethdev.h field 'struct rte_eth_fc_conf.autoneg'
	CC="$(CC)" \
		CFLAGS="$(CFLAGS) \
			-DRTE_MAX_LCORE=64 \
			-DRTE_PKTMBUF_HEADROOM=128 \
			-DRTE_ETHDEV_QUEUE_STAT_CNTRS=2 \
			-DRTE_LOG_LEVEL=0" sh -- $< \
		$@ HAVE_FLOW_CTRL_GET \
		rte_ethdev.h field 'struct eth_dev_ops.flow_ctrl_get'

$(OBJ): $(SRC) $(HDR) config.h

$(OUT): $(OBJ)
	$(CC) $(LDFLAGS) -o $(OUT) $(OBJ) $(LIBS)

clean:
	$(RM) $(OUT) $(OBJ) config.h

warn:
ifeq ($(RTE_SDK),)
	@echo warning: RTE_SDK is not set.
endif
ifeq ($(RTE_TARGET),)
	@echo warning: RTE_TARGET is not set.
endif

# Default target if not set.
RTE_TARGET = build

.PHONY: warn all clean doc FORCE
