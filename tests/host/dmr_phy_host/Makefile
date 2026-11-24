# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host build of the firmware DMR PHY, to compare it against the validated
# prototype in work/dmr-rx on the same captures.
#
#   make            build build/dmr_phy_host
#   make clean

ROOT     := ../../..
OPENRTX  := $(ROOT)/openrtx
BUILD    := build
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += -I$(OPENRTX)/include -I$(OPENRTX)/src

SRCS := main.c \
        $(OPENRTX)/src/protocols/DMR/DmrPhy.c \
        $(OPENRTX)/src/protocols/DMR/DmrFec.c \
        $(OPENRTX)/src/protocols/DMR/DmrDefs.c

$(BUILD)/dmr_phy_host: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: clean
