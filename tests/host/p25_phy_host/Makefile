# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build of the firmware P25 PHY, to compare it against the validated
# prototype in work/p25-rx on the same captures.
#
#   make            build build/p25_phy_host
#   make clean

ROOT     := ../../..
OPENRTX  := $(ROOT)/openrtx
BUILD    := build
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -I$(OPENRTX)/include -I$(OPENRTX)/src

ifeq ($(TRACE),1)
CFLAGS   += -DP25_PHY_TRACE
endif

SRCS := main.c \
        $(OPENRTX)/src/protocols/P25/P25Phy.c \
        $(OPENRTX)/src/protocols/P25/P25Fec.c \
        $(OPENRTX)/src/protocols/P25/P25Defs.c \
        $(OPENRTX)/src/protocols/P25/P25Tables.c

$(BUILD)/p25_phy_host: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: clean
