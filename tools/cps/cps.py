#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
cps.py -- reference client and starter for the real CPS.

    ./cps.py -d /dev/ttyUSB0 hello
    ./cps.py -d /dev/ttyUSB0 chan list
    ./cps.py -d /dev/ttyUSB0 chan set 3 --name REPEATER --rx 145.600M --mode fm
    ./cps.py -d /dev/ttyUSB0 chan dump backup.json
    ./cps.py -d /dev/ttyUSB0 log --level info --seconds 10
    ./cps.py -d /dev/ttyUSB0 diag
    ./cps.py -d /dev/ttyUSB0 reg read --chip bk4819 --reg 0x30
    ./cps.py -d /dev/ttyUSB0 reg write --chip bk4819 --reg 0x30 --value 0x1234
    ./cps.py -d /dev/ttyUSB0 mem read --addr 0x20000000 --len 64
    ./cps.py -d /dev/ttyUSB0 capture --samples 4000 --out cap.raw
    ./cps.py -d /dev/ttyUSB0 tx

Debugging the radio should be one command, not a script: that is why `log`,
`diag`, `reg`, `mem`, `capture` and `tx` are here.  Everything they do is
behind the debug capability, so against a production build they answer
`not_allowed` and this tool prints exactly that.
"""

import argparse
import json
import sys
import time

import pcproto as P


def parse_freq(text):
    """145.600M, 145600k, 433500000 or 145.6 (MHz) to Hz."""
    t = text.strip().lower().replace(",", ".")
    mult = 1
    if t.endswith("m"):
        mult, t = 1_000_000, t[:-1]
    elif t.endswith("k"):
        mult, t = 1_000, t[:-1]
    elif t.endswith("hz"):
        t = t[:-2]
    elif "." in t:
        mult = 1_000_000  # a bare decimal is MHz
    return int(round(float(t) * mult))


def parse_int(text):
    return int(text, 0) if isinstance(text, str) else int(text)


def fmt_freq(hz):
    return "%.6f MHz" % (hz / 1e6) if hz else "-"


def print_channel(idx, ch):
    flags = ",".join(n for b, n in P.BIT_NAMES.items() if ch["flags"] & b)
    print("%3d  %-16s %11s  %-7s %-4s %5dmW%s" % (
        idx, ch["name"], fmt_freq(ch["rx_freq"]),
        P.MODE_NAMES.get(ch["mode"], ch["mode"]),
        "-" if ch["bandwidth"] == 0 else ("12.5k" if ch["bandwidth"] == 1
                                         else "25k"),
        ch["power"], ("  " + flags) if flags else ""))


def open_link(args):
    port = P.Port(args.device, baud=args.baud, timeout=args.timeout)
    link = P.Link(port, client_caps=P.CAP_EVENTS, verbose=args.verbose)
    link.connect()
    return link


# --- subcommands -----------------------------------------------------------

def cmd_hello(link, args):
    h = link.hello
    caps = [n for b, n in P.CAP_NAMES.items() if h.u32(P.T_CAPS) & b]
    print("model        %s" % h.s(P.T_MODEL))
    print("firmware     %s  (hardware %s)" % (h.s(P.T_FIRMWARE),
                                              h.s(P.T_HARDWARE)))
    print("protocol     v%d (min served v%d, client v%d)" % (
        h.u16(P.T_VERSION), h.u16(P.T_VERSION_MIN), P.PROTO_VERSION))
    print("capabilities %s" % " ".join(caps))
    print("channels     %d  (bank format v%d)" % (h.u16(P.T_CHCOUNT),
                                                  h.u16(P.T_CHSTORE_VER)))
    print("contacts     %d" % h.u16(P.T_CTCOUNT))
    print("settings     v%d, %d bytes" % (h.u16(P.T_SETTINGS_VER),
                                          h.u16(P.T_SETTINGS_SIZE)))
    last = h.u16(P.T_LASTUSED)
    print("last used    %s" % ("-" if last == 0xFFFF else last))
    if link.token:
        print("debug token  0x%08x" % link.token)


def cmd_chan(link, args):
    if args.action == "list":
        for i, ch in enumerate(link.chan_list()):
            print_channel(i, ch)
        return

    if args.action == "get":
        print(json.dumps(link.chan_get(args.index), indent=2))
        return

    if args.action == "set":
        rec = link.chan_get(args.index)
        if args.name is not None:
            rec["name"] = args.name
        if args.rx is not None:
            rec["rx_freq"] = parse_freq(args.rx)
        if args.tx is not None:
            rec["tx_freq"] = parse_freq(args.tx)
        elif args.rx is not None and args.simplex:
            rec["tx_freq"] = rec["rx_freq"]
        if args.mode is not None:
            rec["mode"] = P.MODE_IDS[args.mode] if args.mode in P.MODE_IDS \
                else parse_int(args.mode)
        if args.power is not None:
            rec["power"] = args.power
        if args.cc is not None:
            rec["dmr_cc"] = args.cc
        if args.ts is not None:
            rec["dmr_ts"] = args.ts - 1
        back = link.chan_set(args.index, rec)
        print("written and read back:")
        print_channel(args.index, back)
        return

    if args.action == "add":
        rec = {"name": args.name or "NEW", "rx_freq": parse_freq(args.rx),
               "tx_freq": parse_freq(args.tx or args.rx),
               "power": args.power or 1000,
               "mode": P.MODE_IDS.get(args.mode, 1), "bandwidth": 1,
               "flags": 0x02 if args.simplex else 0}
        print("added at %d" % link.chan_add(rec))
        return

    if args.action == "delete":
        link.chan_delete(args.index)
        print("deleted, %d channels left" % link.chan_count())
        return

    if args.action == "dump":
        data = {"version": 1, "channels": link.chan_list()}
        with open(args.file, "w") as f:
            json.dump(data, f, indent=2)
        print("%d channels to %s" % (len(data["channels"]), args.file))
        return

    if args.action == "load":
        with open(args.file) as f:
            data = json.load(f)
        recs = data["channels"] if isinstance(data, dict) else data
        # Written in blocks of four, which is what one frame holds with the
        # debug fields in place; each block is verified by the radio.
        for start in range(0, len(recs), 4):
            block = recs[start:start + 4]
            if not link.chan_write_block(start, block):
                raise SystemExit("block at %d did not verify" % start)
        print("%d channels written and verified" % len(recs))


def cmd_contact(link, args):
    if args.action == "list":
        for i, c in enumerate(link.contact_list()):
            print("%3d  %-10s %-17s %8d %s" % (
                i, c["call"], c["name"], c["dmr_id"],
                "group" if c["flags"] & 1 else "private"))
    elif args.action == "add":
        f = link.request(P.MSG_CONTACT_ADD,
                         P.tlv_bytes(P.T_CONTACT, P.contact_pack({
                             "call": args.call, "name": args.name or args.call,
                             "dmr_id": args.dmr_id or 0, "mode": 0,
                             "flags": 1 if args.group else 0})))
        print("added at %d" % f.u16(P.T_INDEX))
    else:
        f = link.request(P.MSG_CONTACT_LIST,
                         P.tlv_u16(P.T_FIRST, args.index) +
                         P.tlv_u16(P.T_COUNT, 1))
        blob = f.get(P.T_CONTACT_LIST, b"")
        if len(blob) >= P.CONTACT_SIZE:
            print(json.dumps(P.contact_unpack(blob[:P.CONTACT_SIZE]),
                             indent=2))


def cmd_zone(link, args):
    if args.action == "list":
        for z in link.zones():
            print("%2d  %-32s %-4s %4d channels  mode %s" % (
                z["id"], z["name"], z["prefix"], z["count"],
                P.MODE_NAMES.get(z["mode"], z["mode"])))
    elif args.action == "channel":
        print(json.dumps(link.zone_channel(args.zone, args.index), indent=2))
    elif args.action == "import":
        f = link.request(P.MSG_ZONE_IMPORT,
                         P.tlv_u8(P.T_ZONE, args.zone) +
                         P.tlv_u16(P.T_INDEX, args.index))
        print("imported into slot %d" % f.u16(P.T_INDEX))


def cmd_settings(link, args):
    if args.action == "get":
        if args.callsign or args.sql is None and args.dmr_id is None:
            rec = link.settings_record()
            print("settings v%d, %d bytes, crc 0x%04x" % (
                rec["version"], rec["size"], rec["crc"]))
            f = link.settings_fields(
                [P.T_SET_CALLSIGN, P.T_SET_SQLLEVEL, P.T_SET_DMR_ID,
                 P.T_SET_DMR_TS, P.T_SET_BRIGHTNESS])
            print("callsign     %s" % f.s(P.T_SET_CALLSIGN))
            print("squelch      %d" % f.u8(P.T_SET_SQLLEVEL))
            print("brightness   %d" % f.u8(P.T_SET_BRIGHTNESS))
            print("DMR id       %d" % f.u32(P.T_SET_DMR_ID))
            print("DMR timeslot %d" % f.u8(P.T_SET_DMR_TS))
        return

    req = b""
    if args.callsign:
        req += P.tlv_str(P.T_SET_CALLSIGN, args.callsign)
    if args.sql is not None:
        req += P.tlv_u8(P.T_SET_SQLLEVEL, args.sql)
    if args.dmr_id is not None:
        req += P.tlv_u32(P.T_SET_DMR_ID, args.dmr_id)
    if args.ts is not None:
        req += P.tlv_u8(P.T_SET_DMR_TS, args.ts)
    if not req:
        raise SystemExit("nothing to set")
    print("verified" if link.settings_set(req) else "NOT verified")


def cmd_status(link, args):
    s = link.status()
    print("channel   %s (%s)" % (s["name"] or "-", s["channel"]))
    print("rx        %s  mode %s" % (fmt_freq(s["rx_freq"]),
                                     P.MODE_NAMES.get(s["mode"], s["mode"])))
    print("rssi      %d dBm   squelch %s" % (s["rssi"],
                                             "open" if s["squelch"] else
                                             "closed"))
    print("tx        %s" % ["off", "rx", "tx"][min(s["tx_state"], 2)])
    print("battery   %.2f V   uptime %.1f s" % (s["battery_mv"] / 1000.0,
                                                s["uptime_ms"] / 1000.0))


def cmd_log(link, args):
    if not link.has(P.CAP_DEBUG):
        print("this firmware has no debug capability (C62_DEBUG_PROTO off)")
        return 1
    f = link.log_subscribe(level=args.level, rate=args.rate,
                           module=args.module or "")
    print("# subscribed: level<=%d rate=%d lines/s module=%r dropped=%d" % (
        f.u8(P.T_LEVEL), f.u16(P.T_RATE), f.s(P.T_MODULE),
        f.u32(P.T_DROPPED)))

    deadline = time.time() + args.seconds
    expected = None
    losses = 0
    while time.time() < deadline:
        for ev in link.pump(0.2):
            if ev.msg_id != P.EVT_LOG:
                continue
            seq = ev.u32(P.T_LOG_SEQ)
            if expected is not None and seq != expected:
                losses += seq - expected
                print("# %d lines lost" % (seq - expected))
            expected = seq + 1
            print("%8.3fs %-5s %-8s %s" % (
                ev.u32(P.T_UPTIME_MS) / 1000.0,
                ["ERR", "WRN", "INF", "DBG"][min(ev.u8(P.T_LEVEL), 3)],
                ev.s(P.T_MODULE), ev.s(P.T_TEXT)))
    print("# %d lines lost in total" % losses)
    return 0


def cmd_diag(link, args):
    if not link.has(P.CAP_DEBUG):
        print("this firmware has no debug capability (C62_DEBUG_PROTO off)")
        return 1
    d = link.diag()
    if args.json:
        print(json.dumps(d, indent=2))
        return 0
    print("firmware    %s  hw %s  model %s" % (d["firmware"], d["hardware"],
                                               d["model"]))
    print("protocol    v%d (min v%d)" % (d["protocol"], d["protocol_min"]))
    print("uptime      %.1f s   reset cause 0x%x (%s)   wdt %d" % (
        d["uptime_ms"] / 1000.0, d["reset_cause"],
        d["reset_name"] or "unknown", d["wdt_resets"]))
    if d.get("heap_known"):
        print("heap        %d / %d B free" % (d["heap_free"], d["heap_total"]))
    # What the radio can measure, region by region: a region it cannot read is
    # not printed at all rather than printed as zero.
    for r in d.get("regions", []):
        if not r["known"]:
            continue
        pct = (100.0 * r["used"] / r["total"]) if r["total"] else 0.0
        print("%-11s %8d / %8d B  (%.2f %%)" % (r["name"], r["used"],
                                                 r["total"], pct))
    if not d.get("regions"):
        print("flash       %d / %d B" % (d["flash_used"], d["flash_total"]))
        print("ram         %d / %d B" % (d["ram_used"], d["ram_total"]))
    print("agc         %s" % ("unknown" if d["agc"] == 0xFF else d["agc"]))
    for t in d["tasks"]:
        print("stack       %-10s %4d free of %4d" % (t["name"], t["unused"],
                                                     t["size"]))
    print("log dropped %d" % d["dropped"])

    if args.stats:
        for line in link.rx_counters():
            print("  %s" % line)
    return 0


def cmd_reg(link, args):
    chip = 0 if args.chip == "bk4819" else 1
    if args.action == "read":
        print("0x%02x = 0x%04x" % (args.reg, link.reg_read(chip, args.reg)))
        return 0
    value, ok = link.reg_write(chip, args.reg, args.value,
                               confirm=not args.no_confirm)
    print("0x%02x <- 0x%04x, read back 0x%04x (%s)" % (
        args.reg, args.value, value,
        "verified" if ok else "NOT what was written"))
    return 0 if ok else 2


def cmd_mem(link, args):
    data = link.mem_read(args.addr, args.len)
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print("%d bytes to %s" % (len(data), args.out))
        return 0
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        print("%08x  %-47s  %s" % (
            args.addr + off, " ".join("%02x" % b for b in chunk),
            "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)))
    return 0


def cmd_capture(link, args):
    data = link.capture(args.samples)
    with open(args.out, "wb") as f:
        f.write(data)
    print("%d bytes (%d samples) to %s" % (len(data), len(data) // 2,
                                           args.out))
    return 0


def cmd_tx(link, args):
    for line in link.tx_trace().split("|"):
        print(line.strip())
    return 0


def cmd_bb(link, args):
    state = link.bb({"status": None, "arm": 1, "disarm": 2,
                     "pause": 3, "resume": 4}[args.action])
    print("baseband dump state 0x%02x%s" % (
        state, " (not built in)" if state & 0x80 else ""))
    return 0


def cmd_monitor(link, args):
    link.subscribe_status(args.period)
    deadline = None if args.seconds == 0 else time.time() + args.seconds
    print("# events, ctrl-c to stop")
    while deadline is None or time.time() < deadline:
        for ev in link.pump(0.5):
            name = P.MSG_NAMES.get(ev.msg_id, hex(ev.msg_id))
            if ev.msg_id == P.EVT_STATUS:
                print("%-10s rx=%s rssi=%d tx=%d" % (
                    name, fmt_freq(ev.u32(P.T_RX_FREQ)), ev.i16(P.T_RSSI),
                    ev.u8(P.T_TX_STATE)))
            elif ev.msg_id == P.EVT_CHANNEL:
                print("%-10s #%d %s" % (name, ev.u16(P.T_CHANNEL_IDX),
                                        ev.s(P.T_CHANNEL_NAME)))
            elif ev.msg_id == P.EVT_MESSAGE:
                print("%-10s from %d: %s" % (name, ev.u32(P.T_MSG_PEER_ID),
                                             ev.s(P.T_MSG_TEXT)))
            else:
                print("%-10s %s" % (name, ev.tlvs))
    return 0


# --- argument parsing ------------------------------------------------------

def build_parser():
    ap = argparse.ArgumentParser(
        description="Reference client of the C62 PC protocol.")
    ap.add_argument("-d", "--device", default="/dev/ttyUSB0",
                    help="serial port, or the pty of a simulated radio")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("hello").set_defaults(fn=cmd_hello)
    sub.add_parser("status").set_defaults(fn=cmd_status)

    p = sub.add_parser("chan")
    c = p.add_subparsers(dest="action", required=True)
    c.add_parser("list").set_defaults(fn=cmd_chan)
    g = c.add_parser("get")
    g.add_argument("index", type=int)
    g.set_defaults(fn=cmd_chan)
    s = c.add_parser("set")
    s.add_argument("index", type=int)
    s.add_argument("--name")
    s.add_argument("--rx")
    s.add_argument("--tx")
    s.add_argument("--simplex", action="store_true")
    s.add_argument("--mode")
    s.add_argument("--power", type=int)
    s.add_argument("--cc", type=int)
    s.add_argument("--ts", type=int)
    s.set_defaults(fn=cmd_chan)
    a = c.add_parser("add")
    a.add_argument("--name")
    a.add_argument("--rx", required=True)
    a.add_argument("--tx")
    a.add_argument("--mode", default="fm")
    a.add_argument("--power", type=int)
    a.add_argument("--simplex", action="store_true")
    a.set_defaults(fn=cmd_chan)
    d = c.add_parser("delete")
    d.add_argument("index", type=int)
    d.set_defaults(fn=cmd_chan)
    du = c.add_parser("dump")
    du.add_argument("file")
    du.set_defaults(fn=cmd_chan)
    lo = c.add_parser("load")
    lo.add_argument("file")
    lo.set_defaults(fn=cmd_chan)

    p = sub.add_parser("contact")
    c = p.add_subparsers(dest="action", required=True)
    c.add_parser("list").set_defaults(fn=cmd_contact)
    g = c.add_parser("get")
    g.add_argument("index", type=int)
    g.set_defaults(fn=cmd_contact)
    a = c.add_parser("add")
    a.add_argument("call")
    a.add_argument("--name")
    a.add_argument("--dmr-id", type=int, dest="dmr_id")
    a.add_argument("--group", action="store_true")
    a.set_defaults(fn=cmd_contact)

    p = sub.add_parser("zone")
    c = p.add_subparsers(dest="action", required=True)
    c.add_parser("list").set_defaults(fn=cmd_zone)
    g = c.add_parser("channel")
    g.add_argument("zone", type=int)
    g.add_argument("index", type=int)
    g.set_defaults(fn=cmd_zone)
    i = c.add_parser("import")
    i.add_argument("zone", type=int)
    i.add_argument("index", type=int)
    i.set_defaults(fn=cmd_zone)

    p = sub.add_parser("settings")
    c = p.add_subparsers(dest="action", required=True)
    g = c.add_parser("get")
    g.add_argument("--callsign", action="store_true")
    g.add_argument("--sql", type=int)
    g.add_argument("--dmr-id", type=int, dest="dmr_id")
    g.set_defaults(fn=cmd_settings)
    s = c.add_parser("set")
    s.add_argument("--callsign")
    s.add_argument("--sql", type=int)
    s.add_argument("--dmr-id", type=int, dest="dmr_id")
    s.add_argument("--ts", type=int)
    s.set_defaults(fn=cmd_settings)

    p = sub.add_parser("log", help="stream the firmware log")
    p.add_argument("--level", type=int, default=2,
                   help="0 err, 1 warn, 2 info, 3 debug")
    p.add_argument("--rate", type=int, default=20, help="lines per second")
    p.add_argument("--module", help="only this module tag")
    p.add_argument("--seconds", type=float, default=10)
    p.set_defaults(fn=cmd_log)

    p = sub.add_parser("diag", help="versions, reset cause, memory, stacks")
    p.add_argument("--json", action="store_true")
    p.add_argument("--stats", action="store_true",
                   help="also the per-mode receive counters")
    p.set_defaults(fn=cmd_diag)

    p = sub.add_parser("reg", help="BK4819 / BK1080 register access")
    c = p.add_subparsers(dest="action", required=True)
    for name in ("read", "write"):
        r = c.add_parser(name)
        r.add_argument("--chip", choices=["bk4819", "bk1080"],
                       default="bk4819")
        r.add_argument("--reg", type=parse_int, required=True)
        if name == "write":
            r.add_argument("--value", type=parse_int, required=True)
            r.add_argument("--no-confirm", action="store_true")
        r.set_defaults(fn=cmd_reg, action=name)

    p = sub.add_parser("mem", help="bounded read of radio memory")
    c = p.add_subparsers(dest="action", required=True)
    r = c.add_parser("read")
    r.add_argument("--addr", type=parse_int, required=True)
    r.add_argument("--len", type=parse_int, default=32)
    r.add_argument("--out")
    r.set_defaults(fn=cmd_mem)

    p = sub.add_parser("capture", help="RX audio capture")
    p.add_argument("--samples", type=int, default=4000)
    p.add_argument("--out", default="capture.raw")
    p.set_defaults(fn=cmd_capture)

    p = sub.add_parser("bb", help="baseband dump control")
    c = p.add_subparsers(dest="action", required=True)
    for name in ("status", "arm", "disarm", "pause", "resume"):
        c.add_parser(name).set_defaults(fn=cmd_bb, action=name)

    sub.add_parser("tx", help="live transmit trace").set_defaults(fn=cmd_tx)

    p = sub.add_parser("monitor", help="status and channel events")
    p.add_argument("--period", type=int, default=1000)
    p.add_argument("--seconds", type=float, default=0)
    p.set_defaults(fn=cmd_monitor)

    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        link = open_link(args)
    except FileNotFoundError:
        print("no such device: %s" % args.device, file=sys.stderr)
        return 2
    except TimeoutError:
        print("the radio did not answer: is it in the protocol build and "
              "listening on %s?" % args.device, file=sys.stderr)
        return 2
    except P.ProtoError as e:
        print("refused: %s" % e, file=sys.stderr)
        return 2

    try:
        return args.fn(link, args) or 0
    except P.ProtoError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
