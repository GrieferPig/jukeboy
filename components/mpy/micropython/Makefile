# This file is part of the MicroPython project, http://micropython.org/
# The MIT License (MIT)
# Copyright (c) 2022-2023 Damien P. George
#
# This is a very simple makefile that demonstrates how to build the embed port.
# All it needs to do is build all *.c files in the micropython_embed directory.
# This makefile would be replaced with your custom build system.

EMBED_DIR = micropython_embed
PROG = embed

CFLAGS += -I.
CFLAGS += -I$(EMBED_DIR)
CFLAGS += -I$(EMBED_DIR)/port
CFLAGS += -pthread
CFLAGS += -DMPZ_DIG_SIZE=16
CFLAGS += -DMICROPY_MODULE_FROZEN_STR -DMICROPY_MODULE_FROZEN_MPY
CFLAGS += -DMICROPY_QSTR_EXTRA_POOL=mp_qstr_frozen_const_pool
CFLAGS += -Wall -Og -fno-common

SRC += main.c
SRC += $(wildcard $(EMBED_DIR)/*/*.c) $(wildcard $(EMBED_DIR)/*/*/*.c)
SRC := $(filter-out $(EMBED_DIR)/lib/libm/thumb_vfp_sqrtf.c $(EMBED_DIR)/port/modtime_embed.c,$(SRC))
OBJ += $(SRC:.c=.o)

$(PROG): $(OBJ)
	$(CC) -pthread -o $@ $^

clean:
	/bin/rm -f $(OBJ) $(PROG)
