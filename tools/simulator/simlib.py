#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Client of the C62 simulator's control channel.

    with Sim(dir="shots") as sim:
        sim.key("GREEN")            # open the menu
        sim.dump()["screen"]["name"]
        sim.shot("menu")

The protocol is one command per line and one closing line per command, `#end
ok` or `#end err`, so a caller never has to guess how many lines an answer
took. docs/SIMULATOR.md is the reference of the commands and of the dump.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import zlib
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class SimError(RuntimeError):
    pass


class Simulator:
    """A headless C62 simulator, driven through stdin."""

    def __init__(self, binary=None, shots=None, env=None, timeout=60):
        binary = binary or os.environ.get("OPENRTX_SIM_BIN") or self._find_binary()
        self.shots = shots or tempfile.mkdtemp(prefix="openrtx-sim-")
        os.makedirs(self.shots, exist_ok=True)
        self.timeout = timeout
        self.lines = []          # every answer line, for the log

        e = dict(os.environ)
        e.update({
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "OPENRTX_SIM_HEADLESS": "1",
            "OPENRTX_SIM_SHOTS": self.shots,
        })
        if env:
            e.update(env)

        # Bytes, not text: the radio's own log and the strings of the firmware
        # are not guaranteed to be UTF-8, and a tour that dies on one byte of
        # the S-meter is not a tour.
        self.proc = subprocess.Popen(
            [binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
            env=e,
        )
        self.bad_bytes = 0

        while True:
            banner = self._readline()
            if banner.startswith("#ready"):
                break
            if "Platform terminate" in banner:
                raise SimError("the simulator exited during start-up")
        self.banner = banner

    # -- plumbing ---------------------------------------------------------

    @staticmethod
    def _find_binary():
        for candidate in ("build_sim/openrtx_c62_sim", "build_linux/openrtx_c62_sim"):
            path = os.path.join(ROOT, candidate)
            if os.path.exists(path):
                return path
        raise SimError("openrtx_c62_sim not found: build it with "
                       "`meson setup build_sim && meson compile -C build_sim "
                       "openrtx_c62_sim`, or set OPENRTX_SIM_BIN")

    def _readline(self):
        raw = self.proc.stdout.readline()
        if raw == b"":
            raise SimError("the simulator exited (last answer: %r)"
                           % (self.lines[-3:],))
        try:
            line = raw.decode("utf-8").rstrip("\n")
        except UnicodeDecodeError:
            self.bad_bytes += 1
            line = raw.decode("utf-8", "replace").rstrip("\n")
        return line

    def _read_until_end(self, cmd=""):
        """Read one framed answer: #begin, "#| " payload lines, #end."""
        out = []
        while True:
            line = self._readline()
            if line.startswith("#begin"):
                break
            # Anything else is the firmware's own log on stdout
        while True:
            line = self._readline()
            if line.startswith("#end"):
                self.lines.extend(out)
                return out, line
            if line.startswith("#| "):
                out.append(line[3:])

    def cmd(self, line, allow_err=False):
        """Run one command; return its payload lines."""
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()
        payload, end = self._read_until_end(line)
        if not allow_err and not end.startswith("#end ok"):
            raise SimError("%r failed: %s" % (line, "\n".join(payload + [end])))
        return payload

    # -- commands ---------------------------------------------------------

    def key(self, *names):
        """Press and release one or more keys, waiting for the UI after each."""
        self.cmd("keys " + " ".join(names))

    def key_long(self, name):
        self.cmd("key %s long" % name)

    def ptt(self, state="toggle"):
        return self.cmd("ptt %s" % state)[0]

    def volume(self, value):
        return self.cmd("volume %s" % value)[0]

    def sync(self):
        self.cmd("sync")

    def sleep(self, ms):
        self.cmd("sleep %d" % ms)

    def dump(self):
        """The current screen and radio state, as parsed JSON."""
        text = "\n".join(self.cmd("dump json"))
        try:
            return json.loads(text)
        except json.JSONDecodeError as exc:
            raise SimError("bad dump JSON: %s\n%s" % (exc, text[:2000]))

    def dump_text(self):
        return "\n".join(self.cmd("dump")) + "\n"

    def shot(self, name=None):
        """Take a screenshot; `name` without .png gets a number in front."""
        if name is None:
            payload = self.cmd("screenshot")
        else:
            path = name if os.path.isabs(name) else os.path.join(self.shots, name)
            if not path.endswith(".png"):
                path += ".png"
            payload = self.cmd("screenshot " + path)
        for line in payload:
            if line.startswith("#shot "):
                _, path, size = line.split()
                return path
        raise SimError("no screenshot: %r" % (payload,))

    def quit(self):
        try:
            self.proc.stdin.write(b"quit\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.quit()
        return False


# --------------------------------------------------------------------- PNG --

def png_pixels(path):
    """Decode a PNG written by the simulator: (width, height, rows, bytes/px)."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SimError("%s is not a PNG" % path)

    pos = 8
    width = height = depth = ctype = None
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype_tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype_tag == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif ctype_tag == b"IDAT":
            idat += chunk
        elif ctype_tag == b"IEND":
            break

    if depth != 8 or ctype not in (2, 6):
        raise SimError("unsupported PNG: depth %s type %s" % (depth, ctype))

    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(height):
        filt = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        if filt == 1:      # Sub
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif filt == 2:    # Up
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif filt == 3:    # Average
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif filt == 4:    # Paeth
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        elif filt != 0:
            raise SimError("bad filter %d" % filt)
        rows.append(bytes(line))
        prev = line

    return width, height, rows, bpp


def png_ink(path):
    """Fraction of lit pixels and the number of distinct colours."""
    w, h, rows, bpp = png_pixels(path)
    lit = 0
    colours = set()
    for row in rows:
        for x in range(w):
            px = row[x * bpp:x * bpp + 3]
            if px != b"\x00\x00\x00":
                lit += 1
                colours.add(bytes(px))
    return lit / float(w * h), len(colours)


if __name__ == "__main__":
    # A tiny REPL: `python3 tools/simulator/simlib.py` runs commands by hand
    with Simulator() as sim:
        print("# connected; type commands, 'q' to quit")
        for line in sys.stdin:
            line = line.strip()
            if line in ("q", "quit"):
                break
            print("\n".join(sim.cmd(line)))
