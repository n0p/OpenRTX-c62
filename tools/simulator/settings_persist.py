#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The settings record across a restart and across a power cut.

    meson compile -C build_linux openrtx_c62_sim
    python3 tools/simulator/settings_persist.py [--out DIR]

The simulator runs the radio's own firmware, and its flash is a file:
`$XDG_STATE_HOME/OpenRTX/state.bin`, two 512-byte partitions of
platform/drivers/NVM/nvmem_linux.c -- the VFO channel first, the settings record
at 0x0200.  So "change a setting and power-cycle the radio" is a run of the
simulator, a second run on the same directory, and a comparison, and that is
what this walks:

  1. **a change survives a restart**: a value moved in Settings -> Display and
     the accessory port flipped in the tools menu are both there after the
     simulator is stopped and started again;
  2. **a change is deferred, not written at once**: the record on disk still
     does not have the new value half a second after the change, so a power cut
     (`powercut`, the volume knob) loses it -- and one taken after the debounce
     window of core/state.c keeps it, which is the guarantee the radio gives:
     what the operator set three seconds ago is on the flash;
  3. **a value the firmware rewrites at load is written back**: a record with a
     brightness above 100 is loaded as 100 and the flash ends up holding 100,
     not the value the firmware refuses, so the repair happens once and not on
     every boot.

Every case starts from a state directory of its own: nothing here depends on
what another run left behind.
"""

import argparse
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from menu_audit import Audit  # noqa: E402

# platform/drivers/NVM/nvmem_linux.c: the settings partition starts at 0x0200
# and settings_t begins with `brightness`, so that byte is the display level.
RECORD = os.path.join("OpenRTX", "state.bin")
SETTINGS_AT = 0x0200

# core/state.c, NVM_WRITE_DEBOUNCE_MS, plus room for a loaded machine: the
# debounce only starts once state_task() has seen the change, and a test box
# running the rest of the suite can delay that by a pass or three.
DEBOUNCE_MS = 3000
AFTER_WINDOW_MS = DEBOUNCE_MS + 3000
BEFORE_WINDOW_MS = 500

failures = []


def check(what, got, want):
    ok = got == want
    print("  %s %s: %r%s" % ("ok  " if ok else "FAIL", what, got,
                             "" if ok else " (want %r)" % (want,)))
    if not ok:
        failures.append("%s: got %r, want %r" % (what, got, want))
    return ok


class Radio:
    """One power-on of the simulator on a state directory."""

    def __init__(self, home, binary):
        self.home = home
        self.audit = Audit(binary, env={
            "XDG_STATE_HOME": home,
            "OPENRTX_SIM_STORE": os.path.join(home, "store.img"),
        })

    @property
    def sim(self):
        return self.audit.sim

    def settings(self):
        return self.audit.d()["settings"]

    def off(self):
        """The shutdown sequence: what a `quit` writes on the way out."""
        self.sim.quit()

    def cut(self):
        """The volume knob: the rail goes, nothing of ours runs."""
        self.sim.cmd("powercut")
        self.sim.proc.wait(timeout=10)

    def record(self):
        return os.path.join(self.home, RECORD)

    def flash_byte(self, offset):
        with open(self.record(), "rb") as f:
            f.seek(offset)
            return f.read(1)[0]


def fresh(out, name):
    home = os.path.join(out, name)
    if os.path.isdir(home):
        shutil.rmtree(home)
    os.makedirs(home)
    return home


def set_brightness(r, presses):
    """
    Settings -> Display, Brightness, four steps down (100 -> 80).

    The Edit key starts the edit and P1 is the value key, as on the radio: an
    arrow outside the edit only moves the cursor.
    """
    audit = r.audit
    audit.open_settings_page("Display")
    audit.key("GREEN")                      # edit the first row: Brightness
    for _ in range(presses):
        audit.key("P1")


def flip_port(r):
    """The accessory port: P1 held down, the last row of the tools menu."""
    audit = r.audit
    audit.home()
    audit.key_long("P1")
    audit.tools_to("Puerto")
    before = r.settings()["pc_port"]
    audit.key("GREEN")
    after = r.settings()["pc_port"]
    audit.home()
    return before, after


def case_change_survives_a_restart(out, binary):
    """A change in a Settings page and one in the tools menu, both kept."""
    print("a change survives a restart")
    home = fresh(out, "restart")

    r = Radio(home, binary)
    set_brightness(r, 4)
    check("brightness after the change",
          r.settings()["brightness"], 80)
    port_before, port_after = flip_port(r)
    check("the port row changed the setting", port_after != port_before, True)
    r.off()                                  # the shutdown sequence runs

    r = Radio(home, binary)
    s = r.settings()
    check("brightness after the restart", s["brightness"], 80)
    check("the accessory port after the restart", s["pc_port"], port_after)
    r.cut()


def case_the_write_is_deferred(out, binary):
    """Early cut loses the change; a cut after the window keeps it."""
    print("a change reaches the flash inside the debounce window")

    # (a) half a second is not enough: the debouncer has not written yet, and
    # the power cut takes the change with it.  One clean power cycle first, so
    # that the record exists and the check is "it still says 100", not "the
    # flash is still erased".
    home = fresh(out, "cut-early")
    r = Radio(home, binary)
    r.off()
    check("the record of a clean first boot", r.flash_byte(SETTINGS_AT), 100)

    r = Radio(home, binary)
    set_brightness(r, 4)
    check("brightness in RAM", r.settings()["brightness"], 80)
    r.sim.cmd("sleep %d" % BEFORE_WINDOW_MS)
    check("brightness on the flash after %d ms" % BEFORE_WINDOW_MS,
          r.flash_byte(SETTINGS_AT), 100)
    r.cut()

    r = Radio(home, binary)
    check("brightness after a power cut inside the window",
          r.settings()["brightness"], 100)
    r.cut()

    # (b) after the window the same change is on the flash, and survives the
    # cut with no shutdown sequence at all.
    home = fresh(out, "cut-late")
    r = Radio(home, binary)
    set_brightness(r, 4)
    r.sim.cmd("sleep %d" % AFTER_WINDOW_MS)
    check("brightness on the flash after %d ms" % AFTER_WINDOW_MS,
          r.flash_byte(SETTINGS_AT), 80)
    r.cut()

    r = Radio(home, binary)
    check("brightness after a power cut past the window",
          r.settings()["brightness"], 80)
    r.cut()


def case_a_repair_at_load_is_written_back(out, binary):
    """A record the firmware has to fix does not stay wrong on the flash."""
    print("a value rewritten at load is stored")

    home = fresh(out, "repair")
    os.makedirs(os.path.join(home, "OpenRTX"), exist_ok=True)
    data = bytearray(1024)
    data[SETTINGS_AT] = 250                  # above the 0..100 the firmware takes
    with open(os.path.join(home, RECORD), "wb") as f:
        f.write(data)

    r = Radio(home, binary)
    check("brightness the firmware loaded", r.settings()["brightness"], 100)
    r.sim.cmd("sleep %d" % AFTER_WINDOW_MS)
    check("brightness on the flash", r.flash_byte(SETTINGS_AT), 100)
    r.cut()


CASES = [case_change_survives_a_restart, case_the_write_is_deferred,
         case_a_repair_at_load_is_written_back]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None,
                    help="where the state directories of the cases go")
    ap.add_argument("--binary", default=None)
    ap.add_argument("--keep", action="store_true",
                    help="keep the state directories of a clean run too")
    args = ap.parse_args()

    out = args.out or tempfile.mkdtemp(prefix="c62-persist-")
    os.makedirs(out, exist_ok=True)
    print("state directories under %s" % out)

    for case in CASES:
        try:
            case(out, args.binary)
        except Exception as exc:                       # noqa: BLE001
            print("  FAIL %s raised %s: %s"
                  % (case.__name__, type(exc).__name__, exc))
            failures.append("%s raised %s: %s"
                            % (case.__name__, type(exc).__name__, exc))
        print("")

    if failures:
        print("%d failure(s):" % len(failures))
        for f in failures:
            print("  " + f)
        return 1

    print("every case passed")
    if not args.keep and not args.out:
        shutil.rmtree(out, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
