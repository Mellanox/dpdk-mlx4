# Copyright 6WIND 2012-2013, All rights reserved.
# Copyright Mellanox 2012, All rights reserved.

ifdef BUILDING_RTE_SDK
# Compile within DPDK.

include $(RTE_SDK)/mk/rte.vars.mk

LIB = librte_pmd_mlx4.a

CFLAGS += -O3 -std=gnu99 -Wall -Wextra -fPIC
CFLAGS += $(WERROR_FLAGS)
CFLAGS += -D_XOPEN_SOURCE=600
ifeq ($(CONFIG_RTE_LIBRTE_MLX4_DEBUG),y)
CFLAGS += -pedantic -g -UNDEBUG -DPEDANTIC
else
CFLAGS += -DNDEBUG -UPEDANTIC
endif

SRCS-$(CONFIG_RTE_LIBRTE_MLX4_PMD) += mlx4.c

DEPDIRS-$(CONFIG_RTE_LIBRTE_MLX4_PMD) += lib/librte_eal
DEPDIRS-$(CONFIG_RTE_LIBRTE_MLX4_PMD) += lib/librte_ether
DEPDIRS-$(CONFIG_RTE_LIBRTE_MLX4_PMD) += lib/librte_mempool
DEPDIRS-$(CONFIG_RTE_LIBRTE_MLX4_PMD) += lib/librte_mbuf

include $(RTE_SDK)/mk/rte.lib.mk

else # RTE_LIBRTE_MLX4_PMD
# Compile a standalone shared object.
# Only depends on DPDK headers in $RTE_SDK.

OUT = librte_pmd_mlx4.so
SRC = mlx4.c
HDR = mlx4.h
OBJ = $(SRC:.c=.o)

CC = gcc
CFLAGS += -I$(RTE_SDK)/$(RTE_TARGET)/include
CFLAGS += -O3 -std=gnu99 -Wall -Wextra -fPIC
CFLAGS += -D_XOPEN_SOURCE=600
CFLAGS += -g

ifeq ($(DEBUG),1)
CFLAGS += -pedantic -UNDEBUG -DPEDANTIC
else
CFLAGS += -DNDEBUG -UPEDANTIC
endif

LDFLAGS += -shared
LDFLAGS += -libverbs

all: warn $(OUT)

$(OBJ): $(SRC) $(HDR)

$(OUT): $(OBJ)
	$(CC) $(LDFLAGS) -o $(OUT) $(OBJ)

clean:
	$(RM) $(OUT) $(OBJ)

warn:
ifeq ($(RTE_SDK),)
	@echo warning: RTE_SDK is not set.
endif
ifeq ($(RTE_TARGET),)
	@echo warning: RTE_TARGET is not set.
endif

# Default target if not set.
RTE_TARGET = build

.PHONY: warn all clean

endif # RTE_LIBRTE_MLX4_PMD

ifeq ($(DPDK_6WIND),1)
CFLAGS += -DDPDK_6WIND
endif
