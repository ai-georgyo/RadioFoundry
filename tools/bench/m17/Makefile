# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The M17 bench tool (m17tool.c) on the host, from the firmware's own sources. tools/bench/m17tool.py runs this.
#   make -C tools/bench/m17           -> build/bench/m17tool
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(HERE)/../../..)
CORE    := $(ROOT)/core
OUT     ?= $(ROOT)/build/bench
CC      ?= cc
# Blanking __attribute__ also strips glibc's always_inline from its fortified printf wrappers, which then fail to
# build where the compiler turns _FORTIFY_SOURCE on by default. It strips Codec 2's may_alias types too
# (c2_dec_int.h, fxp.h), so the optimizer must not assume strict aliasing
CFLAGS  := -std=gnu99 -O2 -fno-strict-aliasing -Wall -Wextra -Wshadow -I$(CORE) -I$(ROOT)/hal '-D__attribute__(x)=' \
           -U_FORTIFY_SOURCE -DCONFIG_M17=1 -DCONFIG_M17_PACKET=1 -DCONFIG_M17_BERT=1 -DCONFIG_M17_SCRAMBLER=1 \
           -DCONFIG_M17_AES=1
SRC     := $(HERE)/m17tool.c $(CORE)/m17/m17_frame.c $(CORE)/m17/m17_packet.c $(CORE)/m17/m17_bert.c \
           $(CORE)/m17/m17_addr.c $(CORE)/m17/m17_crypt.c $(CORE)/crypto/aes.c \
           $(CORE)/fec/conv.c $(CORE)/fec/golay.c $(CORE)/fec/crc.c \
           $(wildcard $(CORE)/codec2/*.c) $(wildcard $(CORE)/dsp/*.c)
HDRS    := $(wildcard $(CORE)/m17/*.h $(CORE)/codec2/*.h $(CORE)/dsp/*.h $(CORE)/fec/*.h $(CORE)/air/*.h \
           $(CORE)/crypto/*.h) \
           $(ROOT)/hal/hal_air.h

$(OUT)/m17tool: $(SRC) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

.PHONY: clean
clean:
	rm -f $(OUT)/m17tool
