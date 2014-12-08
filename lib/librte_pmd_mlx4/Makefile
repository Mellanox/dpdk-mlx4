# Copyright 6WIND 2012-2014, All rights reserved.
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
Q ?= @
SOLIB = $O/$(OUT)

SRC = mlx4.c
HDR = mlx4.h
OBJ = $(SRC:.c=.o)

CFLAGS_SAVE := $(CFLAGS)
LDFLAGS_SAVE := $(LDFLAGS)
RTE_OUTPUT = $(O)

ifndef RTE_SDK
RTE_SDK = /usr/local/dpdk
$(warning RTE_SDK is not set, assuming "$(RTE_SDK)".)
endif

ifndef RTE_TARGET
RTE_TARGET = build
$(warning RTE_TARGET is not set, assuming "$(RTE_TARGET)".)
endif

include $(RTE_SDK)/mk/rte.vars.mk

CFLAGS += $(CFLAGS_SAVE)
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

LDFLAGS += $(LDFLAGS_SAVE)
LDFLAGS += -shared
LIBS += -libverbs

all: $(OUT)

install:
	install -D -m 664 $(SOLIB) $(DESTDIR)$(libdir)/$(notdir $(SOLIB))

doc: doc-default
doc-%: FORCE
	$Q $(MAKE) -rR --no-print-directory -C doc \
		DOC_TOOLS=$(abspath $(DOC_TOOLS)) \
		O=$(abspath $O)/doc \
		DESTDIR=$(abspath $(DESTDIR))$(docdir) \
		$*

config.h: comp_check.sh
	$(RM) $@
	$Q CC="$(CC)" CFLAGS="$(CFLAGS)" sh -- $< \
		$@ RSS_SUPPORT \
		infiniband/verbs.h enum IBV_EXP_DEVICE_UD_RSS
	$Q CC="$(CC)" CFLAGS="$(CFLAGS)" sh -- $< \
		$@ SEND_RAW_WR_SUPPORT \
		infiniband/verbs.h type 'struct ibv_send_wr_raw'
	$Q CC="$(CC)" \
		CFLAGS="$(CFLAGS)" sh -- $< \
		$@ HAVE_STRUCT_RTE_PKTMBUF \
		rte_mbuf.h type 'struct rte_pktmbuf'
	$Q CC="$(CC)" \
		CFLAGS="$(CFLAGS)" sh -- $< \
		$@ HAVE_MTU_GET \
		rte_ethdev.h type mtu_get_t
	$Q CC="$(CC)" \
		CFLAGS="$(CFLAGS)" sh -- $< \
		$@ HAVE_MTU_SET \
		rte_ethdev.h type mtu_set_t
	$Q CC="$(CC)" \
		CFLAGS="$(CFLAGS)" sh -- $< \
		$@ HAVE_FC_CONF_AUTONEG \
		rte_ethdev.h field 'struct rte_eth_fc_conf.autoneg'
	$Q CC="$(CC)" \
		CFLAGS="$(CFLAGS)" sh -- $< \
		$@ HAVE_FLOW_CTRL_GET \
		rte_ethdev.h field 'struct eth_dev_ops.flow_ctrl_get'
	$Q CC="$(CC)" \
		CFLAGS="$(CFLAGS)" sh -- $< \
		$@ HAVE_STRUCT_RTE_MBUF_IN_PORT \
		rte_mbuf.h field 'struct rte_mbuf.in_port'

$(OBJ): $(SRC) $(HDR) config.h

$(OUT): $(OBJ)
	$(CC) $(LDFLAGS) -o $(OUT) $(OBJ) $(LIBS)

clean:
	$(RM) $(OUT) $(OBJ) config.h

.PHONY: all clean doc FORCE
