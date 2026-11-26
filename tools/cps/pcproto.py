#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Reference client of the radio's binary protocol.

This is the layer a CPS is built on: framing, session, the message table and
the records, with no opinion about how a screen should look.  The command line
in cps.py uses it, and so would a graphical CPS.

The wire format is documented in docs/PC_PROTO.md; the constants here are the
same ones core/pc_proto.h defines, and they are not generated because a client
has to keep working with a firmware older than itself.

No third-party dependency: the port is an fd with termios, which is what a
programming cable is.
"""

import os
import select
import struct
import termios
import time

# --- framing ---------------------------------------------------------------

END = 0xC0
ESC = 0xDB
ESC_END = 0xDC
ESC_ESC = 0xDD

FRAME_VERSION = 1

TYPE_REQUEST = 1
TYPE_RESPONSE = 2
TYPE_EVENT = 3

FLAG_MORE = 0x01

PROTO_VERSION = 1
PROTO_VERSION_MIN = 1

# --- message identifiers ---------------------------------------------------

MSG_HELLO = 0x0001
MSG_PING = 0x0002
MSG_SESSION_CLOSE = 0x0003
MSG_COUNTERS = 0x0004
EVT_STATUS = 0x0080
EVT_CHANNEL = 0x0081
EVT_PTT = 0x0082
EVT_MESSAGE = 0x0083
EVT_SETTINGS = 0x0084
EVT_LOG = 0x0085
MSG_ERROR = 0x00FF

MSG_CHAN_COUNT = 0x0101
MSG_CHAN_GET = 0x0102
MSG_CHAN_SET = 0x0103
MSG_CHAN_ADD = 0x0104
MSG_CHAN_DELETE = 0x0105
MSG_CHAN_LIST = 0x0106
MSG_CHAN_WRITE_BLOCK = 0x0107
MSG_CHAN_LASTUSED = 0x0108

MSG_CONTACT_COUNT = 0x0110
MSG_CONTACT_GET = 0x0111
MSG_CONTACT_SET = 0x0112
MSG_CONTACT_ADD = 0x0113
MSG_CONTACT_DELETE = 0x0114
MSG_CONTACT_LIST = 0x0115

MSG_ZONE_COUNT = 0x0120
MSG_ZONE_GET = 0x0121
MSG_ZONE_CHANNEL = 0x0122
MSG_ZONE_IMPORT = 0x0123

MSG_SETTINGS_GET = 0x0130
MSG_SETTINGS_SET = 0x0131
MSG_SETTINGS_INFO = 0x0132

MSG_STATUS_GET = 0x0140
MSG_STATUS_SUBSCRIBE = 0x0141
MSG_STATS_GET = 0x0150

MSG_MSG_COUNT = 0x0201
MSG_MSG_INBOX = 0x0202
MSG_MSG_OUTBOX = 0x0203
MSG_MSG_SEND = 0x0204
MSG_MSG_DELETE = 0x0205
MSG_MSG_MARK_READ = 0x0206

MSG_DBG_INFO = 0x0501
MSG_DBG_LOG_SUB = 0x0502
MSG_DBG_LOG_DRAIN = 0x0503
MSG_DBG_REG_READ = 0x0510
MSG_DBG_REG_WRITE = 0x0511
MSG_DBG_AGC = 0x0512
MSG_DBG_MEM_READ = 0x0520
MSG_DBG_CAP_START = 0x0530
MSG_DBG_CAP_STOP = 0x0531
MSG_DBG_CAP_READ = 0x0532
MSG_DBG_BB = 0x0540
MSG_DBG_TX_TRACE = 0x0550
MSG_DBG_RX_COUNTERS = 0x0551
MSG_DBG_BLACKBOX = 0x0552

MSG_NAMES = {v: k[4:].lower() for k, v in list(globals().items())
             if k.startswith("MSG_") or k.startswith("EVT_")}

# --- capabilities and errors ----------------------------------------------

CAP_CHANNELS = 0x0001
CAP_CONTACTS = 0x0002
CAP_MESSAGES = 0x0004
CAP_SETTINGS = 0x0008
CAP_ZONES = 0x0010
CAP_STATS = 0x0020
CAP_PACKET_MODEM = 0x0040
CAP_ANALYZER = 0x0080
CAP_EVENTS = 0x0100
CAP_CHAN_BLOCK = 0x0200
CAP_MSG_SEND = 0x0400
CAP_SETTINGS_FIELDS = 0x0800
CAP_ZONE_IMPORT = 0x1000
CAP_DEBUG = 0x2000

CAP_NAMES = {
    CAP_CHANNELS: "channels", CAP_CONTACTS: "contacts",
    CAP_MESSAGES: "messages", CAP_SETTINGS: "settings", CAP_ZONES: "zones",
    CAP_STATS: "stats", CAP_PACKET_MODEM: "packet_modem",
    CAP_ANALYZER: "analyzer", CAP_EVENTS: "events",
    CAP_CHAN_BLOCK: "chan_block", CAP_MSG_SEND: "msg_send",
    CAP_SETTINGS_FIELDS: "settings_fields", CAP_ZONE_IMPORT: "zone_import",
    CAP_DEBUG: "debug",
}

ERR_OK = 0
ERR_NAMES = {
    0: "ok", 1: "bad_request", 2: "unknown_msg", 3: "not_supported",
    4: "bad_index", 5: "invalid_value", 6: "io", 7: "version", 8: "busy",
    9: "too_large", 10: "permission", 11: "not_allowed",
}

# --- TLV tags --------------------------------------------------------------

T_ERROR = 0x0001
T_STATUS = 0x0002
T_VERSION = 0x0003
T_VERSION_MIN = 0x0004
T_MSG_ID = 0x0005
T_MODEL = 0x0006
T_FIRMWARE = 0x0007
T_HARDWARE = 0x0008
T_CAPS = 0x0009
T_UPTIME_MS = 0x000A
T_VERIFY = 0x000B

T_INDEX = 0x0010
T_FIRST = 0x0011
T_COUNT = 0x0012
T_ZONE = 0x0013
T_ZONE_KIND = 0x0014
T_ZONE_NAME = 0x0015
T_ZONE_PREFIX = 0x0016
T_ZONE_COUNT = 0x0017
T_CHCOUNT = 0x0018
T_CTCOUNT = 0x0019
T_SETTINGS_VER = 0x001A
T_SETTINGS_CRC = 0x001B
T_SETTINGS_SIZE = 0x001C
T_CHSTORE_VER = 0x001D
T_LASTUSED = 0x001E
T_MSG_STORE_ID = 0x001F

T_CHANNEL = 0x0040
T_CHANNEL_LIST = 0x0041
T_CONTACT = 0x0042
T_CONTACT_LIST = 0x0043
T_ZONE_CHANNEL = 0x0044
T_SETTINGS = 0x0045
T_SETTINGS_REC = 0x0046
T_STATS = 0x0047

T_RX_FREQ = 0x0080
T_TX_FREQ = 0x0081
T_MODE = 0x0082
T_RSSI = 0x0083
T_TX_STATE = 0x0084
T_CHANNEL_IDX = 0x0085
T_BATTERY_MV = 0x0086
T_SQUELCH = 0x0087
T_PERIOD_MS = 0x0088
T_CHANNEL_NAME = 0x0089
T_TX_POWER = 0x008A
T_TETRA_UL = 0x008E
T_TETRA_UL_SSI = 0x008F
T_TX_REASON = 0x00EF

T_MSG_TRANSPORT = 0x00A0
T_MSG_PEER = 0x00A1
T_MSG_PEER_ID = 0x00A2
T_MSG_TEXT = 0x00A3
T_MSG_STATE = 0x00A4
T_MSG_FLAGS = 0x00A5
T_MSG_UNREAD = 0x00A6
T_MSG_INBOX = 0x00A7
T_MSG_OUTBOX = 0x00A8

T_SET_CALLSIGN = 0x00C0
T_SET_BRIGHTNESS = 0x00C1
T_SET_CONTRAST = 0x00C2
T_SET_SQLLEVEL = 0x00C3
T_SET_VOXLEVEL = 0x00C4
T_SET_TZ = 0x00C5
T_SET_DMR_ID = 0x00C6
T_SET_DMR_DEST = 0x00C7
T_SET_DMR_TYPE = 0x00C8
T_SET_DMR_TS = 0x00C9
T_SET_M17_CAN = 0x00CA
T_SET_M17_DEST = 0x00CB

T_TOKEN = 0x00D0
T_CONFIRM = 0x00D1
T_LEVEL = 0x00D2
T_MODULE = 0x00D3
T_LOG_SEQ = 0x00D4
T_DROPPED = 0x00D5
T_CHIP = 0x00D6
T_REG = 0x00D7
T_VALUE = 0x00D8
T_ADDR = 0x00D9
T_LENGTH = 0x00DA
T_DATA = 0x00DB
T_TEXT = 0x00DC
T_RESET_CAUSE = 0x00DD
T_RESET_NAME = 0x00DE
T_WDT_RESETS = 0x00DF
T_HEAP_FREE = 0x00E0
T_HEAP_TOTAL = 0x00E1
T_PSRAM_FREE = 0x00E2
T_PSRAM_TOTAL = 0x00E3
T_FLASH_USED = 0x00E4
T_FLASH_TOTAL = 0x00E5
T_RAM_USED = 0x00E6
T_RAM_TOTAL = 0x00E7
T_MEM_REGION = 0x00F2
T_TASK_STACK = 0x00E8
T_AGC = 0x00E9
T_RATE = 0x00EA
T_OFFSET = 0x00EB
T_RUNNING = 0x00EC
T_BB_STATE = 0x00ED

# The post-mortem record: raw bytes as they sit on flash (docs/BLACKBOX.md).
T_BLACKBOX = 0x00F0
T_BLACKBOX_LEN = 0x00F1

# The screen path (core/display_flush.h, docs/RX_AIRBAND.md §9).
T_DISP_FRAMES = 0x00F3
T_DISP_DROPS = 0x00F4
T_DISP_MS = 0x00F5
T_DISP_MAX_MS = 0x00F6
T_DISP_FAIL = 0x00F7

# --- records ---------------------------------------------------------------

# chstore_channel_t: 37 bytes, packed (see core/channel_store.h).
CHANNEL_FMT = "<16sIIH" + "B" * 7 + "I"
CHANNEL_SIZE = 37
CONTACT_FMT = "<10s17sIBB"
CONTACT_SIZE = 33
ZONE_CHANNEL_FMT = "<BH8sIIiBBHB"
ZONE_CHANNEL_SIZE = 28

BIT_NAMES = {0x01: "rx_only", 0x02: "simplex", 0x04: "skip"}

MODE_NAMES = {0: "none", 1: "fm", 2: "dmr", 3: "m17", 4: "tetra", 5: "nxdn",
              6: "dpmr", 7: "p25", 8: "ysf", 9: "dstar", 10: "airband",
              11: "wfm"}
MODE_IDS = {v: k for k, v in MODE_NAMES.items()}


def channel_unpack(raw):
    (name, rx, tx, power, mode, bw, rx_tones, tx_tones, cc_ts, flags,
     dest_type, dest_id) = struct.unpack(CHANNEL_FMT, raw)
    return {
        "name": name.split(b"\0")[0].decode("utf-8", "replace"),
        "rx_freq": rx,
        "tx_freq": tx,
        "power": power,
        "mode": mode,
        "bandwidth": bw,
        "rx_tone": rx_tones & 0x7F, "rx_tone_en": rx_tones >> 7,
        "tx_tone": tx_tones & 0x7F, "tx_tone_en": tx_tones >> 7,
        "dmr_cc": cc_ts & 0x0F,
        "dmr_ts": cc_ts >> 4,
        "flags": flags,
        "dmr_dest_type": dest_type,
        "dmr_dest_id": dest_id,
    }


def channel_pack(d):
    name = d["name"].encode()[:15]
    rx_tones = ((d.get("rx_tone_en", 0) & 1) << 7) | (d.get("rx_tone", 0) & 0x7F)
    tx_tones = ((d.get("tx_tone_en", 0) & 1) << 7) | (d.get("tx_tone", 0) & 0x7F)
    cc_ts = (d.get("dmr_cc", 0) & 0x0F) | ((d.get("dmr_ts", 0) & 0x0F) << 4)
    return struct.pack(CHANNEL_FMT, name, d["rx_freq"], d["tx_freq"],
                       d["power"], d["mode"], d["bandwidth"], rx_tones,
                       tx_tones, cc_ts, d.get("flags", 0),
                       d.get("dmr_dest_type", 0), d.get("dmr_dest_id", 0))


def contact_unpack(raw):
    call, name, dmr_id, mode, flags = struct.unpack(CONTACT_FMT, raw)
    return {
        "call": call.split(b"\0")[0].decode("utf-8", "replace"),
        "name": name.split(b"\0")[0].decode("utf-8", "replace"),
        "dmr_id": dmr_id, "mode": mode, "flags": flags,
    }


def contact_pack(d):
    return struct.pack(CONTACT_FMT, d["call"].encode()[:9],
                       d["name"].encode()[:16], d["dmr_id"],
                       d["mode"], d.get("flags", 0))


def zone_channel_unpack(raw):
    (zone, index, name, rx, tx, shift, mode, bw, power,
     flags) = struct.unpack(ZONE_CHANNEL_FMT, raw)
    return {"zone": zone, "index": index,
            "name": name.split(b"\0")[0].decode(), "rx": rx, "tx": tx,
            "shift": shift, "mode": mode, "bandwidth": bw, "power": power,
            "flags": flags}


# --- framing helpers -------------------------------------------------------

def escape(body):
    out = bytearray([END])
    for b in body:
        if b == END:
            out += bytes([ESC, ESC_END])
        elif b == ESC:
            out += bytes([ESC, ESC_ESC])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


def crc16(data):
    # CRC-16/CCITT-FALSE, the one core/crc.c computes.
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                else (crc << 1) & 0xFFFF
    return crc


def encode(msg_type, seq, msg_id, payload=b"", flags=0):
    # version, type, seq, id, flags, length: 8 bytes, little endian, no padding
    body = struct.pack("<BBBHBH", FRAME_VERSION, msg_type, seq, msg_id,
                       flags, len(payload)) + payload
    return escape(body + struct.pack("<H", crc16(body)))


class Frame:
    def __init__(self, mtype, seq, msg_id, flags, payload):
        self.type = mtype
        self.seq = seq
        self.msg_id = msg_id
        self.flags = flags
        self.payload = payload
        self.tlvs = tlv_parse(payload)

    def get(self, tag, default=None):
        v = self.tlvs.get(tag)
        if v is None:
            return default
        return v

    def u8(self, tag, default=0):
        v = self.get(tag)
        return default if v is None or len(v) != 1 else v[0]

    def u16(self, tag, default=0):
        v = self.get(tag)
        return default if v is None or len(v) != 2 else struct.unpack(
            "<H", v)[0]

    def u32(self, tag, default=0):
        v = self.get(tag)
        return default if v is None or len(v) != 4 else struct.unpack(
            "<I", v)[0]

    def i16(self, tag, default=0):
        v = self.get(tag)
        return default if v is None or len(v) != 2 else struct.unpack(
            "<h", v)[0]

    def s(self, tag, default=""):
        v = self.get(tag)
        return default if v is None else v.decode("utf-8", "replace")

    def __repr__(self):
        return "<frame %s %s seq=%d %d bytes>" % (
            self.type, MSG_NAMES.get(self.msg_id, hex(self.msg_id)),
            self.seq, len(self.payload))


def tlv_parse(payload):
    out = {}
    off = 0
    while off + 4 <= len(payload):
        tag, ln = struct.unpack("<HH", payload[off:off + 4])
        off += 4
        if off + ln > len(payload):
            break
        out[tag] = payload[off:off + ln]
        off += ln
    return out


def tlv_u8(tag, v):
    return struct.pack("<HHB", tag, 1, v & 0xFF)


def tlv_u16(tag, v):
    return struct.pack("<HHH", tag, 2, v & 0xFFFF)


def tlv_u32(tag, v):
    return struct.pack("<HHI", tag, 4, v & 0xFFFFFFFF)


def tlv_i16(tag, v):
    return struct.pack("<HHh", tag, 2, v)


def tlv_str(tag, s):
    b = s.encode() if isinstance(s, str) else s
    return struct.pack("<HH", tag, len(b)) + b


def tlv_bytes(tag, b):
    return struct.pack("<HH", tag, len(b)) + b


class ProtoError(Exception):
    def __init__(self, code, msg_id=None, detail=""):
        self.code = code
        self.msg_id = msg_id
        super().__init__("%s (%s)%s" % (
            ERR_NAMES.get(code, "error %d" % code),
            MSG_NAMES.get(msg_id, hex(msg_id) if msg_id else "-"),
            ": " + detail if detail else ""))


# --- port ------------------------------------------------------------------

class Port:
    """A file descriptor in raw mode: the programming cable."""

    def __init__(self, path=None, baud=115200, timeout=2.0, fd=None):
        """Either a device path or an already open fd: a test drives the radio
        on the master side of a pty, which is an fd and not a path."""
        self.timeout = timeout
        if fd is not None:
            self.path = "<fd %d>" % fd
            self.fd = fd
            return
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.timeout = timeout
        if path.startswith("/dev/tty"):
            try:
                attrs = termios.tcgetattr(self.fd)
                attrs[0] = 0                      # iflag: raw
                attrs[1] = 0                      # oflag: raw
                attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
                attrs[3] = 0                      # lflag: no echo, no canon
                speed = getattr(termios, "B%d" % baud, termios.B115200)
                attrs[4] = speed
                attrs[5] = speed
                attrs[6][termios.VMIN] = 0
                attrs[6][termios.VTIME] = 0
                termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            except termios.error:
                pass

    def write(self, data):
        os.write(self.fd, data)

    def read(self, timeout=None):
        timeout = self.timeout if timeout is None else timeout
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def close(self):
        os.close(self.fd)


class Link:
    """
    A session on the cable: framing, sequence numbers, the request/response
    pair and the event queue.  HELLO has to be done before anything else, and
    the token it returns is what the debug writes need.
    """

    def __init__(self, port, client_caps=CAP_EVENTS, verbose=False):
        self.port = port
        self.seq = 0
        self.buf = bytearray()
        self.events = []
        self.verbose = verbose
        self.client_caps = client_caps
        self.hello = None
        self.token = 0
        self.lost_frames = 0
        self.last_seq = None

    # -- low level ---------------------------------------------------------

    def _feed(self, data):
        self.buf += data
        frames = []
        while True:
            try:
                start = self.buf.index(END)
            except ValueError:
                # No delimiter yet: this is the beginning of a frame that is
                # still arriving, not garbage.  Only a runaway buffer is
                # dropped, so a device that talks nonsense cannot grow it.
                if len(self.buf) > 8192:
                    del self.buf[:-1024]
                return frames
            if start > 0:
                # Garbage before the delimiter: what the resync throws away.
                del self.buf[:start]
            try:
                end = self.buf.index(END, 1)
            except ValueError:
                return frames
            raw = bytes(self.buf[1:end])
            del self.buf[:end]
            body = bytearray()
            esc = False
            for b in raw:
                if esc:
                    esc = False
                    body.append(END if b == ESC_END else
                                ESC if b == ESC_ESC else b)
                elif b == ESC:
                    esc = True
                else:
                    body.append(b)
            if len(body) < 10 or body[0] != FRAME_VERSION:
                self.lost_frames += 1
                continue
            _ver, mtype, seq, msg_id, flags, plen = struct.unpack(
                "<BBBHBH", bytes(body[:8]))
            if plen != len(body) - 10:
                self.lost_frames += 1
                continue
            crc = struct.unpack("<H", bytes(body[-2:]))[0]
            if crc16(bytes(body[:-2])) != crc:
                self.lost_frames += 1
                continue
            frames.append(Frame(mtype, seq, msg_id, flags,
                                bytes(body[8:-2])))
        return frames

    def read_frame(self, timeout=None, want=None):
        deadline = time.time() + (self.port.timeout if timeout is None
                                  else timeout)
        while True:
            frames = self._feed(self.port.read(max(0.0, deadline -
                                                     time.time())))
            # Classify first, then return: an event that arrives in the same
            # read as a response must be queued, not dropped with the rest of
            # the chunk.
            match = None
            for f in frames:
                if f.type == TYPE_EVENT:
                    self.events.append(f)
                    if self.verbose:
                        print("# event %s" % f)
                elif match is None and (want is None or f.seq == want):
                    match = f
            if match is not None:
                return match
            if time.time() >= deadline:
                return None

    def request(self, msg_id, payload=b"", timeout=None):
        self.seq = (self.seq + 1) & 0xFF
        frame = encode(TYPE_REQUEST, self.seq, msg_id, payload)
        self.port.write(frame)
        resp = self.read_frame(timeout=timeout, want=self.seq)
        if resp is None:
            raise TimeoutError("no answer to %s" %
                               MSG_NAMES.get(msg_id, hex(msg_id)))
        if resp.msg_id == MSG_ERROR:
            raise ProtoError(resp.u8(T_ERROR), resp.u16(T_MSG_ID))
        return resp

    def poll(self, timeout=0.0):
        """Read what the cable has now and queue the events it carries.

        This is the poll that does not consume: the events stay in the queue
        for `pump()`, and a consumer that looks at the queue directly -- the
        bench's `bench_edges()` -- sees them as well.  A frame that is not an
        event is dropped, as `read_frame()` drops every answer that is not the
        one it is waiting for; requests are synchronous, so a response found
        here or there is one nobody asked for.
        """
        out = []
        for f in self._feed(self.port.read(timeout)):
            if f.type == TYPE_EVENT:
                self.events.append(f)
                out.append(f)
        return out

    def pump(self, seconds):
        """Collect events for a while and return them in order.

        The queue holds what has not been handed out yet: an event arrives in
        it when it lands behind a response (`read_frame`), and `pump` is what
        takes it out.  An event returned here is not put back, so two
        consecutive calls can never return the same line twice -- re-queueing
        the batch that was just delivered made `cps log` print every line
        twice and report negative losses, and it is what the log stream test
        caught as a sequence of [6, 7, 6, 7, 8].
        """
        got = list(self.events)
        self.events.clear()
        deadline = time.time() + seconds
        while time.time() < deadline:
            for f in self._feed(self.port.read(0.05)):
                if f.type == TYPE_EVENT:
                    got.append(f)
        return got

    # -- session -----------------------------------------------------------

    def connect(self):
        caps = tlv_u32(T_CAPS, self.client_caps)
        req = tlv_u16(T_VERSION, PROTO_VERSION) + caps
        h = self.request(MSG_HELLO, req)
        self.hello = h
        status = h.u8(T_STATUS)
        if status != ERR_OK:
            raise ProtoError(status, MSG_HELLO,
                             "firmware speaks v%d, needs client >= v%d" % (
                                 h.u16(T_VERSION), h.u16(T_VERSION_MIN)))
        self.token = h.u32(T_TOKEN)
        return h

    def close(self):
        try:
            self.request(MSG_SESSION_CLOSE, b"", timeout=0.5)
        except (TimeoutError, ProtoError):
            pass

    # -- capabilities ------------------------------------------------------

    @property
    def caps(self):
        return self.hello.u32(T_CAPS) if self.hello else 0

    def has(self, cap):
        return bool(self.caps & cap)

    def require(self, cap, what):
        if not self.has(cap):
            raise ProtoError(3, None,
                             "%s not supported by this firmware" % what)

    def debug_token(self):
        if not self.token:
            raise ProtoError(11, None, "no debug token: build the firmware "
                                       "with C62_DEBUG_PROTO")
        return tlv_u32(T_TOKEN, self.token)

    # -- channels ----------------------------------------------------------

    def chan_count(self):
        return self.request(MSG_CHAN_COUNT).u16(T_CHCOUNT)

    def chan_get(self, idx):
        return channel_unpack(self.request(MSG_CHAN_GET,
                                           tlv_u16(T_INDEX, idx)).get(T_CHANNEL))

    def chan_set(self, idx, rec):
        f = self.request(MSG_CHAN_SET,
                         tlv_u16(T_INDEX, idx) + tlv_bytes(T_CHANNEL,
                                                           channel_pack(rec)))
        return channel_unpack(f.get(T_CHANNEL))

    def chan_add(self, rec):
        f = self.request(MSG_CHAN_ADD, tlv_bytes(T_CHANNEL, channel_pack(rec)))
        return f.u16(T_INDEX)

    def chan_delete(self, idx):
        self.request(MSG_CHAN_DELETE, tlv_u16(T_INDEX, idx))

    def chan_list(self, first=0, count=64):
        """Every channel, following the MORE flag."""
        out = []
        idx = first
        while True:
            f = self.request(MSG_CHAN_LIST,
                             tlv_u16(T_FIRST, idx) +
                             tlv_u16(T_COUNT, 4))
            blob = f.get(T_CHANNEL_LIST, b"")
            n = f.u16(T_COUNT)
            for i in range(n):
                out.append(channel_unpack(blob[i * CHANNEL_SIZE:
                                               (i + 1) * CHANNEL_SIZE]))
            idx += n
            if not (f.flags & FLAG_MORE) or n == 0 or len(out) >= count:
                break
        return out

    def chan_write_block(self, first, recs):
        blob = b"".join(channel_pack(r) for r in recs)
        f = self.request(MSG_CHAN_WRITE_BLOCK,
                         tlv_u16(T_FIRST, first) +
                         tlv_bytes(T_CHANNEL_LIST, blob))
        return f.u8(T_VERIFY) == 1

    # -- contacts, zones, settings ----------------------------------------

    def contact_list(self):
        out = []
        idx = 0
        while True:
            f = self.request(MSG_CONTACT_LIST,
                             tlv_u16(T_FIRST, idx) + tlv_u16(T_COUNT, 4))
            blob = f.get(T_CONTACT_LIST, b"")
            n = f.u16(T_COUNT)
            for i in range(n):
                out.append(contact_unpack(blob[i * CONTACT_SIZE:
                                               (i + 1) * CONTACT_SIZE]))
            idx += n
            if not (f.flags & FLAG_MORE) or n == 0:
                break
        return out

    def zones(self):
        n = self.request(MSG_ZONE_COUNT).u16(T_ZONE_COUNT)
        out = []
        for z in range(n):
            f = self.request(MSG_ZONE_GET, tlv_u8(T_ZONE, z))
            out.append({"id": f.u8(T_ZONE), "name": f.s(T_ZONE_NAME),
                        "prefix": f.s(T_ZONE_PREFIX),
                        "count": f.u16(T_ZONE_COUNT), "mode": f.u8(T_MODE)})
        return out

    def zone_channel(self, zone, index):
        f = self.request(MSG_ZONE_CHANNEL, tlv_u8(T_ZONE, zone) +
                         tlv_u16(T_INDEX, index))
        return zone_channel_unpack(f.get(T_ZONE_CHANNEL))

    def settings_fields(self, tags=None):
        req = b"".join(tlv_bytes(t, b"") for t in (tags or []))
        return self.request(MSG_SETTINGS_GET, req)

    def settings_record(self):
        f = self.request(MSG_SETTINGS_GET)
        return {"version": f.u16(T_SETTINGS_VER),
                "size": f.u16(T_SETTINGS_SIZE),
                "crc": f.u16(T_SETTINGS_CRC),
                "data": f.get(T_SETTINGS, b"")}

    def settings_set(self, tlvs):
        f = self.request(MSG_SETTINGS_SET, tlvs)
        return f.u8(T_VERIFY) == 1

    def status(self):
        f = self.request(MSG_STATUS_GET)
        return {
            "rx_freq": f.u32(T_RX_FREQ), "tx_freq": f.u32(T_TX_FREQ),
            "mode": f.u8(T_MODE), "rssi": f.i16(T_RSSI),
            "tx_state": f.u8(T_TX_STATE), "channel": f.u16(T_CHANNEL_IDX),
            "battery_mv": f.u16(T_BATTERY_MV),
            "squelch": f.u8(T_SQUELCH), "name": f.s(T_CHANNEL_NAME),
            "uptime_ms": f.u32(T_UPTIME_MS),
            # What the radio would key with, and why it would not: the answer
            # to "the PTT does nothing" without opening the radio.
            "power_mw": f.u16(T_TX_POWER),
            "tx_reason": f.s(T_TX_REASON),
            # The MS side of a TETRA cell, while the "Cell uplink" session is
            # on: 1 and the sending SSI, zero in every other mode.
            "tetra_uplink": f.u8(T_TETRA_UL),
            "tetra_ul_ssi": f.u32(T_TETRA_UL_SSI),
        }

    def subscribe_status(self, period_ms):
        return self.request(MSG_STATUS_SUBSCRIBE,
                            tlv_u16(T_PERIOD_MS, period_ms)).u16(T_PERIOD_MS)

    # -- debug -------------------------------------------------------------

    def log_subscribe(self, level=2, rate=20, module=""):
        f = self.request(MSG_DBG_LOG_SUB,
                         tlv_u8(T_LEVEL, level) + tlv_u16(T_RATE, rate) +
                         tlv_str(T_MODULE, module))
        return f

    def diag(self):
        f = self.request(MSG_DBG_INFO)
        stacks = []
        blob = f.get(T_TASK_STACK, b"")
        for off in range(0, len(blob) - 15, 16):
            name, unused, size = struct.unpack("<12sHH", blob[off:off + 16])
            stacks.append({"name": name.split(b"\0")[0].decode(),
                           "unused": unused, "size": size})
        # Memory per region: what the platform can actually measure, one
        # record per region. A region that is not measurable is not sent, and
        # neither is the heap when the platform cannot ask its allocator --
        # the client shows what was measured instead of a zero.
        regions = []
        blob = f.get(T_MEM_REGION, b"")
        for off in range(0, len(blob) - 19, 20):
            name, used, total, known, _ = struct.unpack(
                "<8sIIB3s", blob[off:off + 20])
            regions.append({"name": name.split(b"\0")[0].decode(),
                            "used": used, "total": total,
                            "known": bool(known)})
        # The screen path: frames that made it, frames dropped on the deadline,
        # and which transfer was lost when one was.
        fail = f.get(T_DISP_FAIL, b"")
        disp = {
            "frames": f.u32(T_DISP_FRAMES),
            "drops": f.u32(T_DISP_DROPS),
            "last_ms": f.u32(T_DISP_MS),
            "max_ms": f.u32(T_DISP_MAX_MS),
        }
        if len(fail) >= 4:
            lost_len, chunk, data = struct.unpack("<HBB", fail[:4])
            disp.update({"fail_len": lost_len, "fail_chunk": chunk,
                         "fail_data": bool(data)})
        return {
            "disp": disp,
            "protocol": f.u16(T_VERSION), "protocol_min": f.u16(T_VERSION_MIN),
            "firmware": f.s(T_FIRMWARE), "hardware": f.s(T_HARDWARE),
            "model": f.s(T_MODEL), "uptime_ms": f.u32(T_UPTIME_MS),
            "caps": f.u32(T_CAPS), "reset_cause": f.u32(T_RESET_CAUSE),
            "reset_name": f.s(T_RESET_NAME), "wdt_resets": f.u32(T_WDT_RESETS),
            "regions": regions,
            "heap_known": f.get(T_HEAP_TOTAL) is not None,
            "heap_free": f.u32(T_HEAP_FREE), "heap_total": f.u32(T_HEAP_TOTAL),
            "psram_free": f.u32(T_PSRAM_FREE),
            "psram_total": f.u32(T_PSRAM_TOTAL),
            "flash_used": f.u32(T_FLASH_USED),
            "flash_total": f.u32(T_FLASH_TOTAL),
            "ram_used": f.u32(T_RAM_USED), "ram_total": f.u32(T_RAM_TOTAL),
            "agc": f.u8(T_AGC), "dropped": f.u32(T_DROPPED),
            "tasks": stacks,
        }

    def reg_read(self, chip, reg):
        f = self.request(MSG_DBG_REG_READ, tlv_u8(T_CHIP, chip) +
                         tlv_u8(T_REG, reg))
        return f.u16(T_VALUE)

    def reg_write(self, chip, reg, value, confirm=True):
        req = (tlv_u8(T_CHIP, chip) + tlv_u8(T_REG, reg) +
               tlv_u16(T_VALUE, value) + tlv_u8(T_CONFIRM, 1 if confirm else 0)
               + self.debug_token())
        f = self.request(MSG_DBG_REG_WRITE, req)
        return f.u16(T_VALUE), f.u8(T_VERIFY) == 1

    def mem_read(self, addr, length):
        f = self.request(MSG_DBG_MEM_READ,
                         tlv_u32(T_ADDR, addr) + tlv_u16(T_LENGTH, length) +
                         self.debug_token())
        return f.get(T_DATA, b"")

    def capture(self, samples, chunk=128):
        f = self.request(MSG_DBG_CAP_START, tlv_u16(T_LENGTH, samples))
        capacity = f.u32(T_LENGTH)
        self.request(MSG_DBG_CAP_STOP)
        out = bytearray()
        off = 0
        while off < capacity:
            n = min(chunk, capacity - off)
            try:
                r = self.request(MSG_DBG_CAP_READ,
                                 tlv_u32(T_OFFSET, off) +
                                 tlv_u16(T_LENGTH, n) + self.debug_token())
            except ProtoError:
                break
            data = r.get(T_DATA, b"")
            if not data:
                break
            out += data
            off += len(data) // 2
        return bytes(out)

    def bb(self, op=None):
        if op is None:
            req = b""
        else:
            # Arming or disarming changes what the radio does with its PSRAM
            # and its UART, so the firmware asks for the token.
            req = tlv_u8(T_BB_STATE, op) + self.debug_token()
        f = self.request(MSG_DBG_BB, req)
        return f.u8(T_BB_STATE)

    def tx_trace(self):
        return self.request(MSG_DBG_TX_TRACE).s(T_TEXT)

    def rx_counters(self):
        f = self.request(MSG_DBG_RX_COUNTERS)
        out = []
        payload = f.payload
        off = 0
        while off + 4 <= len(payload):
            tag, ln = struct.unpack("<HH", payload[off:off + 4])
            off += 4
            if off + ln > len(payload):
                break
            if tag == T_STATS:
                out.append(payload[off:off + ln].decode("utf-8", "replace"))
            off += ln
        return out
