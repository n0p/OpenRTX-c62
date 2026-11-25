# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build of the System Fusion transmitter closed through the firmware's own
# receiver: a call through an AWGN channel, a BER sweep against the SNR, and the
# modulating waveform for an external decoder.
#
#   make              build build/ysf_tx_test
#   ./build/ysf_tx_test loop [snr_db] [frames]
#   ./build/ysf_tx_test sweep
#   ./build/ysf_tx_test wave out.wav [frames]   (s16le 48 kHz, feed to dsd-neo)
#   make clean

ROOT     := ../../..
OPENRTX  := $(ROOT)/openrtx
LIB      := $(ROOT)/lib/dmr_vocoder
BUILD    := build
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -I$(OPENRTX)/include -I$(OPENRTX)/src -I$(LIB)
CFLAGS   += -DCONFIG_YSF

SRCS := main.c \
        $(OPENRTX)/src/protocols/C4FM/C4fmMod.c \
        $(OPENRTX)/src/protocols/YSF/YsfTx.c \
        $(OPENRTX)/src/protocols/YSF/YsfPhy.c \
        $(OPENRTX)/src/protocols/YSF/YsfFrame.c \
        $(LIB)/dmr_decoder.c \
        $(LIB)/dmr_golay.c \
        $(LIB)/dmr_golay_table.c \
        $(LIB)/dmr_fft.c

$(BUILD)/ysf_tx_test: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: clean
