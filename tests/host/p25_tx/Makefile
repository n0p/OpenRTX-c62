# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build of the P25 Phase 1 transmitter closed through the firmware's own
# receiver: FEC self-tests, a voiced call and an idle call through an AWGN
# channel, a BER sweep, and the modulating waveform for an external decoder.
#
#   make              build build/p25_tx_test
#   ./build/p25_tx_test fec
#   ./build/p25_tx_test loop [snr_db] [frames]
#   ./build/p25_tx_test idle [snr_db] [frames]
#   ./build/p25_tx_test sweep
#   ./build/p25_tx_test wave out.raw      (s16le 48 kHz, feed to dsd-neo)
#   make clean

ROOT     := ../../..
OPENRTX  := $(ROOT)/openrtx
BUILD    := build
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -I$(OPENRTX)/include -I$(OPENRTX)/src

SRCS := main.c \
        $(OPENRTX)/src/protocols/C4FM/C4fmMod.c \
        $(OPENRTX)/src/protocols/P25/P25Tx.c \
        $(OPENRTX)/src/protocols/P25/P25Phy.c \
        $(OPENRTX)/src/protocols/P25/P25Fec.c \
        $(OPENRTX)/src/protocols/P25/P25Defs.c \
        $(OPENRTX)/src/protocols/P25/P25Tables.c

$(BUILD)/p25_tx_test: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: clean
