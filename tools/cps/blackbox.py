#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Read the C62 post-mortem: over the programming cable, or out of a file.

Two ways in, because a radio that froze may not answer the cable at all:

  # after the radio has rebooted by itself, with the debug build attached
  tools/cps/blackbox.py /dev/ttyUSB0

  # or from the 516 bytes somebody saved (debug message dump, flash read)
  tools/cps/blackbox.py -f postmortem.bin

The wire format is PC_MSG_DBG_BLACKBOX (0x0552, docs/PC_PROTO.md) and the record
layout is docs/BLACKBOX.md.  The decode is deliberately independent of the
firmware's C structures: this file is the second reader, and it has to agree
with the first one byte for byte.
"""

import argparse
import struct
import sys

RECORD_SIZE = 516
OFF_CRC = 512

MAGIC = 0x31425842  # "BBX1"
VALID = 0x42425258  # "XRBB"
VERSION = 1

REASONS = {0: "none", 1: "STALL", 2: "FATAL", 3: "MANUAL"}
BEATS = {0: "ui", 1: "rtx", 2: "proto", 3: "main"}
MODES = {0: "none", 1: "FM", 2: "DMR", 3: "M17", 4: "TETRA", 5: "NXDN",
         6: "dPMR", 7: "P25", 8: "YSF", 9: "D-STAR", 10: "AM", 11: "WFM"}
THREAD_STATES = {0: "ready", 1: "pended", 2: "dead", 4: "dummy",
                 8: "prestart", 16: "suspended"}

MARKS = {
    0: "-", 1: "BOOT", 2: "TERM", 3: "MODE_ENTER", 4: "MODE_READY",
    5: "MODE_FAIL", 6: "MODE_OFF", 7: "RTX_CONF", 8: "RADIO_CONF",
    9: "RX_ON", 10: "TX_ON", 11: "TX_OFF", 12: "TX_ABORT", 13: "AUDIO_OPEN",
    14: "AUDIO_CLOSE", 15: "AIRBAND_LOAD", 16: "WFM_ENTER", 17: "SETTINGS",
    18: "CHAN_LOAD", 19: "UI_MODE", 20: "HANG",
}


def crc16_ccitt(data):
    """
    The CRC the record carries: core/blackbox.c uses crc_ccitt(), which is
    CRC-16/XMODEM numerically (poly 0x1021, seed 0, no reflection, no final
    xor).  Not the same seed as the protocol's frames, which use 0xFFFF.
    """
    crc = 0x0000
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else \
                  (crc << 1) & 0xFFFF
    return crc


def decode(rec):
    """Turn the 516 bytes into a dict plus a list of readable lines."""
    if len(rec) < RECORD_SIZE:
        raise SystemExit("record is %d bytes, expected %d"
                         % (len(rec), RECORD_SIZE))

    magic, version, hdr, valid = struct.unpack_from("<IHHI", rec, 0)
    uptime, reason, detail, reset, crumb_seq = struct.unpack_from("<IIIII", rec, 12)
    rtx_mode, ui_mode, rx_freq, tx_freq, flags, n_threads, n_crumbs, \
        thread_size = struct.unpack_from("<HHIIHHHH", rec, 32)

    out = {
        "magic_ok": magic == MAGIC,
        "version": version,
        "version_ok": version == VERSION,
        "valid": valid == VALID,
        "crc_ok": struct.unpack_from("<H", rec, OFF_CRC)[0]
                  == crc16_ccitt(rec[:OFF_CRC]),
        "uptime_ms": uptime,
        "reason": REASONS.get(reason, "?"),
        "detail": detail,
        "reset_cause": reset,
        "marks": crumb_seq,
        "rtx_mode": MODES.get(rtx_mode, str(rtx_mode)),
        "ui_mode": MODES.get(ui_mode, str(ui_mode)),
        "rx_freq": rx_freq,
        "tx_freq": tx_freq,
        "flags": flags,
        "threads": [],
        "crumbs": [],
    }

    base = 64
    for i in range(n_threads):
        o = base + i * 32
        name = rec[o:o + 12].split(b"\0")[0].decode("ascii", "replace")
        prio, state, free, size, role = struct.unpack_from("<bBHHH", rec, o + 12)
        pc, lr, sp = struct.unpack_from("<III", rec, o + 20)
        out["threads"].append({
            "name": name or "?", "prio": prio,
            "state": THREAD_STATES.get(state, "0x%02x" % state),
            "free": free, "size": size,
            "role": BEATS.get(role - 1, "") if role else "",
            "pc": pc, "lr": lr, "sp": sp,
        })

    base = 64 + 10 * 32
    for i in range(n_crumbs):
        w = struct.unpack_from("<I", rec, base + i * 4)[0]
        out["crumbs"].append({"mark": MARKS.get(w >> 24, "?%d" % (w >> 24)),
                              "tick": w & 0xFFFFFF})

    lines = []
    add = lines.append
    add("C62 post-mortem: record v%d, %d B, %s, %s" % (
        version, RECORD_SIZE, "crc ok" if out["crc_ok"] else "CRC BAD",
        "valid" if out["valid"] else "already read"))
    stalled = [BEATS[i] for i in range(4) if detail & (1 << i)]
    suffix = " [%s]" % " ".join(stalled) if out["reason"] == "STALL" else ""
    add("reason: %s detail 0x%08x%s" % (out["reason"], detail, suffix))
    add("uptime: %u ms (%u min %u s), reset cause 0x%08x" % (
        uptime, uptime // 60000, (uptime // 1000) % 60, reset))
    add("radio: rtx=%s ui=%s flags 0x%02x" % (out["rtx_mode"], out["ui_mode"],
                                              flags))
    add("  rx %u Hz, tx %u Hz" % (rx_freq, tx_freq))
    add("threads: %d" % n_threads)
    for t in out["threads"]:
        add("  %-10s pr %-3d st %-9s free %5d/%-5d role %-5s pc 0x%08x lr 0x%08x"
            " sp 0x%08x" % (t["name"], t["prio"], t["state"], t["free"],
                            t["size"], t["role"] or "-", t["pc"], t["lr"],
                            t["sp"]))
    add("breadcrumbs: %d shown of %u marks, oldest first" % (n_crumbs, crumb_seq))
    for c in out["crumbs"]:
        add("  +%-7d ms  %s" % (c["tick"], c["mark"]))

    return out, lines


def fetch(port, baud, timeout):
    """Ask a radio for the record, chunk by chunk, over the debug protocol."""
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pcproto

    link = pcproto.Link(pcproto.Port(port, baud=baud, timeout=timeout))
    try:
        link.connect()
        if not (link.caps() & pcproto.CAP_DEBUG):
            raise SystemExit("this firmware has no debug namespace "
                             "(build with -DC62_DEBUG_PROTO=ON)")

        rec = b""
        total = None
        while total is None or len(rec) < total:
            resp = link.request(pcproto.MSG_DBG_BLACKBOX,
                                pcproto.tlv_u32(pcproto.T_OFFSET, len(rec)))
            total = resp.u16(pcproto.T_BLACKBOX_LEN)
            chunk = resp.get(pcproto.T_BLACKBOX) or b""
            if not chunk:
                break
            rec += chunk
        return rec
    finally:
        link.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="programming cable, e.g. /dev/ttyUSB0")
    ap.add_argument("-f", "--file", help="a saved 516-byte record instead")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("-r", "--raw", action="store_true",
                    help="write the record to stdout as binary and nothing else")
    ap.add_argument("-o", "--out", help="also save the raw record here")
    args = ap.parse_args()

    if args.file:
        with open(args.file, "rb") as f:
            rec = f.read()
    elif args.port:
        rec = fetch(args.port, args.baud, args.timeout)
    else:
        ap.error("give a port or -f")

    if not rec:
        raise SystemExit("no post-mortem: the radio has not crashed since the "
                         "last one was read")

    if args.out:
        with open(args.out, "wb") as f:
            f.write(rec)

    if args.raw:
        sys.stdout.buffer.write(rec)
        return

    decoded, lines = decode(rec)
    for line in lines:
        print(line)

    if not decoded["crc_ok"]:
        raise SystemExit("record failed its CRC: partial write or corruption")
    if not decoded["valid"]:
        raise SystemExit("record was already read (validity word cleared)")


if __name__ == "__main__":
    main()
