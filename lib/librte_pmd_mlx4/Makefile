# Copyright 6WIND 2012-2013, All rights reserved.
# Copyright Mellanox 2012, All rights reserved.

OUT = librte_pmd_mlx4.so
SRC = mlx4.c
HDR = mlx4.h
OBJ = $(SRC:.c=.o)

CC = gcc
CFLAGS += -I$(RTE_SDK)/$(RTE_TARGET)/include
CFLAGS += -O3 -std=gnu99 -Wall -Wextra -fPIC
CFLAGS += -D_XOPEN_SOURCE=600

ifeq ($(DEBUG),1)
CFLAGS += -pedantic -g -UNDEBUG -DPEDANTIC
else
CFLAGS += -DNDEBUG -UPEDANTIC
endif

ifneq ($(IBVERBS),)
CFLAGS += -I$(IBVERBS)/include
LDFLAGS += -L$(IBVERBS)/
endif

LDFLAGS += -shared
LDFLAGS += -libverbs

all: $(OUT)

$(OBJ): warn $(SRC) $(HDR)

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
