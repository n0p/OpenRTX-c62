# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build of the firmware System Fusion receive chain, run against a real
# 48 kHz discriminator recording.
#
#   make            build build/ysf_host
#   ./build/ysf_host capture.wav out.wav
#   make clean

ROOT     := ../../..
OPENRTX  := $(ROOT)/openrtx
LIB      := $(ROOT)/lib/dmr_vocoder
BUILD    := build
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -I$(OPENRTX)/include -I$(OPENRTX)/src -I$(LIB)
CFLAGS   += -DCONFIG_YSF

SRCS := main.c txmodel.c \
        $(OPENRTX)/src/protocols/YSF/YsfPhy.c \
        $(OPENRTX)/src/protocols/YSF/YsfFrame.c \
        $(LIB)/dmr_vocoder.c \
        $(LIB)/dmr_decoder.c \
        $(LIB)/dmr_fft.c

$(BUILD)/ysf_host: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: clean
