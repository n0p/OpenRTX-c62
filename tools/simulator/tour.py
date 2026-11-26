#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The menu tour of the C62 simulator: every screen, walked with keys.

    meson compile -C build_linux openrtx_c62_sim
    python3 tools/simulator/tour.py [--out DIR]

It drives the real firmware in the host emulator through the control channel
(docs/SIMULATOR.md), the way the operator drives the radio: green for OK, red
for back, P1/P2, UP/DOWN, digits, a long press where a screen only offers one.
For every screen it writes a PNG and the text dump of what the screen shows, and
it fails - loudly, with the step and the reason - when

  * a screen comes out blank (no ink, or no text in the dump);
  * a row of a list or of the quick menu cannot be selected;
  * a value that should be editable does not change when the key that edits it
    is pressed;
  * a key press is not claimed by any block of the UI (the "deaf screen"
    defect, see enum uiKeyClaim);
  * after leaving a screen the main screen no longer answers the keys;
  * a screen, a mode, a row or a menu entry the firmware has is not on the list
    this file declares (COVERAGE, below), or the list says something the
    firmware no longer has.

That last one is what keeps this file honest. The constants below are the
declaration: the eleven modes with their quick menu rows, whether they can
transmit and which Settings page is theirs; the rows of the top menu and of the
Settings list; and every screen of the UI, either visited or listed as
unreachable with the reason. The tour checks the declaration against what the
firmware *draws* while it runs, and tests/unit/sim_coverage.cpp checks it
against what the firmware *registers* (uiModes[], the menu tables of ui.c, the
uiScreen enum) and against what its own key walk can reach. A mode or a screen
added without a step here breaks both.

`--coverage-manifest FILE` writes the declaration as plain text, which is how
the C++ test reads it (meson builds the file from this script).

The run is reproducible: it gets its own settings record and its own channel /
contact / message store (XDG_STATE_HOME and OPENRTX_SIM_STORE point into a
temporary directory that is created empty), so what it sees does not depend on
what another run of the emulator left behind - and it asserts that the store it
starts from is empty.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simlib import Simulator, SimError, png_ink

# ---------------------------------------------------------------------------
# The coverage this tour declares. See the module docstring: everything here is
# checked against the firmware, never trusted.
# ---------------------------------------------------------------------------

# The eleven modes of the registry (uiModes[]), in the order the side key walks
# them. `tx` is what the tools menu must show on its right-hand column, `page`
# the Settings page the mode has, `rows` the quick menu top to bottom.
MODES = {
    "FM": {"tx": True, "page": "SETTINGS_FM", "rows": [
        "SQL", "PWR", "CTCSS", "TONE", "BW", "SHIFT", "1750", "BKL",
        "KEYLIT", "LOCK", "MSGS", "ESPECTRO", "MSG", "APRS", "PKT"]},
    "M17": {"tx": True, "page": "SETTINGS_M17", "rows": [
        "DEST", "CAN", "CANRX", "SRC", "META", "PWR", "BKL", "KEYLIT",
        "LOCK", "MSGS", "MSG"]},
    "TETRA": {"tx": False, "page": "SETTINGS_TETRA", "rows": [
        "AFC", "FINE", "CELL", "LA/CC", "REG", "NET", "UL", "CALL", "BKL",
        "KEYLIT", "LOCK", "MSGS", "MSG"]},
    # DMR transmits in the C62 image and in the simulator (CONFIG_DMR_TX on the
    # simulator target only, see meson.build); its page is the generic one.
    "DMR": {"tx": True, "page": "SETTINGS_MODE", "rows": [
        "CC", "TS", "SYNC", "CALL", "DEST", "FINE", "BKL", "KEYLIT", "LOCK",
        "MSGS", "MSG"]},
    "NXDN": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "RAN", "BW", "SYNC", "CALL", "TYPE", "FINE", "BKL", "KEYLIT", "LOCK",
        "MSGS", "MSG"]},
    "dPMR": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "SYNC", "FINE", "BKL", "KEYLIT", "LOCK", "MSGS", "MSG"]},
    "P25": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "SYNC", "NAC", "TYPE", "CALL", "FINE", "BKL", "KEYLIT", "LOCK",
        "MSGS", "MSG"]},
    "YSF": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "SYNC", "FINE", "BKL", "KEYLIT", "LOCK", "MSGS", "MSG"]},
    "DSTAR": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "SYNC", "MY", "UR", "RPT", "TXT", "FINE", "BKL", "KEYLIT", "LOCK",
        "MSGS", "MSG"]},
    "AM": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "SQL", "AGC", "STEP", "SNAP", "CH", "IF", "FINE", "BKL", "KEYLIT",
        "LOCK", "MSGS", "ESPECTRO", "MSG"]},
    "WFM": {"tx": False, "page": "SETTINGS_MODE", "rows": [
        "ST", "LVL", "SNR", "CH", "BKL", "KEYLIT", "LOCK", "MSGS", "MSG"]},
}

MODE_ORDER = ["FM", "M17", "TETRA", "DMR", "NXDN", "dPMR", "P25", "YSF",
              "DSTAR", "AM", "WFM"]

# Rows of the top menu, in menu order (ui.c, menu_items[]).
TOP_MENU = ["Banks", "Channels", "Zones", "Contacts", "Messages", "GPS",
            "Settings", "Backup", "Info", "About"]

# Settings pages, in menu order (ui.c, settings_items[]).
SETTINGS_PAGES = ["Display", "Time & Date", "GPS", "Radio", "Audio", "M17",
                  "TETRA", "FM", "DMR", "NXDN", "dPMR", "P25", "YSF", "DSTAR",
                  "AM", "WFM", "Accessibility", "Default Settings"]

# Every screen of this build the tour visits (ui/ui_default.h, enum uiScreen;
# the names are the ones ui_dump_screenName() prints).
SCREENS = [
    "MAIN_VFO", "MAIN_VFO_INPUT", "MAIN_MEM",
    "MENU_TOP", "MENU_BANK", "MENU_CHANNEL", "MENU_ZONES", "MENU_CONTACTS",
    "MENU_GPS", "MENU_SETTINGS", "MENU_BACKUP_RESTORE", "MENU_BACKUP",
    "MENU_RESTORE", "MENU_INFO", "MENU_ABOUT",
    "SETTINGS_TIMEDATE", "SETTINGS_TIMEDATE_SET", "SETTINGS_DISPLAY",
    "SETTINGS_GPS", "SETTINGS_RADIO", "SETTINGS_AUDIO", "SETTINGS_M17",
    "SETTINGS_TETRA", "SETTINGS_FM", "SETTINGS_MODE", "SETTINGS_ACCESSIBILITY",
    "SETTINGS_RESET2DEFAULTS",
    "MENU_APRS", "APRS_DETAIL", "APRS_MSGS", "APRS_MSG_VIEW", "APRS_MSG_EDIT",
    "APRS_CFG",
    "PKT_CFG", "PKT_STATUS",
    "MENU_MESSAGES", "MSG_VIEW", "MSG_SENT", "MSG_COMPOSE", "MSG_STATUS",
    "LOW_BAT",
    "SPECTRUM", "TOOLS_MENU",
]

# Screens the enum has and no key reaches. They are declared, not visited, and
# the reason is the point: tests/unit/sim_coverage.cpp walks the UI from the
# main screen and fails if one of these becomes reachable without a step here.
UNREACHABLE = {
    "MODE_VFO": "leftover of the memory/VFO mode pair: nothing sets ui_screen "
                "to it (the main screens are MAIN_VFO/MAIN_MEM)",
    "MODE_MEM": "same pair, same reason",
}

# Fraction of lit pixels below which a screen is considered blank.
MIN_INK = 0.005

# Rows of a list screen in the dump: every list draws its rows at x = 2 (the
# layout's left margin) between the header and the hint bar, and the row under
# the cursor with a leading "* ".
ROW_X = 2
ROW_TOP = 20
ROW_BOTTOM = 112

# Rows whose key does not change the value they show, by design: LOCK locks the
# keypad (the point of the row, and it would make every later step deaf) and
# 1750 is a momentary burst the FSM clears on the next key. Every other row
# with a value is pressed and has to move at least one value per table.
NO_EDIT = {"LOCK", "1750"}


class Tour:
    def __init__(self, outdir, binary=None, quiet=False):
        self.outdir = os.path.abspath(outdir)
        self.shots = os.path.join(self.outdir, "shots")
        os.makedirs(self.shots, exist_ok=True)

        self.tmp = tempfile.mkdtemp(prefix="c62-tour-")
        self.quiet = quiet
        self.step_name = "start"
        self.n = 0
        self.records = []
        self.failures = []
        self.findings = []
        self.visited = set()

        env = {
            # A settings record of this run, not of the last one
            "XDG_STATE_HOME": os.path.join(self.tmp, "state"),
            # The channel bank, the contact book and the message store of this
            # run: a file that does not exist yet, so the bank starts empty
            # (user_store_host.c creates it on the first write)
            "OPENRTX_SIM_STORE": os.path.join(self.tmp, "userstore.img"),
        }
        os.makedirs(env["XDG_STATE_HOME"], exist_ok=True)

        self.sim = Simulator(binary=binary, shots=self.shots, env=env)

    # ---------------------------------------------------------------- output --

    def log(self, msg):
        if not self.quiet:
            print(msg, flush=True)

    def fail(self, msg):
        where = self.step_name
        self.failures.append("%s: %s" % (where, msg))
        print("FAIL %s: %s" % (where, msg), flush=True)

    def finding(self, msg):
        self.findings.append("%s: %s" % (self.step_name, msg))
        print("NOTE %s: %s" % (self.step_name, msg), flush=True)

    def check(self, cond, msg):
        if not cond:
            self.fail(msg)
        return cond

    # ------------------------------------------------------------------- i/o --

    def dump(self):
        return self.sim.dump()

    def key(self, *names):
        self.sim.key(*names)

    def key_long(self, name):
        self.sim.key_long(name)

    def combo(self, *names):
        self.sim.cmd("keycombo " + " ".join(names))

    def inject(self, *args):
        return self.sim.cmd("inject " + " ".join(args))

    def capture(self, name, screen=None, need=(), allow_blank=False,
                forbidden=()):
        """Take the picture and the dump of the screen, and check it is a screen."""
        self.n += 1
        stem = "%03d-%s" % (self.n, name)
        png = self.sim.shot(stem + ".png")
        d = self.dump()
        text = self.sim.dump_text()

        with open(os.path.join(self.shots, stem + ".txt"), "w") as f:
            f.write(text)

        name_now = d["screen"]["name"]
        self.records.append({"step": stem, "screen": name_now,
                             "png": png, "dump": d})
        self.visited.add(name_now)

        if screen is not None and name_now != screen:
            self.fail("%s: expected screen %s, got %s"
                      % (stem, screen, name_now))

        if not allow_blank:
            ink, colours = png_ink(png)
            if ink < MIN_INK or not d["lines"]:
                self.fail("%s: blank screen (ink %.3f, %d text lines)"
                          % (stem, ink, len(d["lines"])))

        blob = self.text_of(d) + "\n" + d["header"]["title"]
        for fragment in need:
            if fragment not in blob:
                self.fail("%s: %r is not on the screen (screen: %s)"
                          % (stem, fragment, self.text_of(d)))
        for fragment in forbidden:
            if fragment in blob:
                self.fail("%s: %r must not be on this screen (%s)"
                          % (stem, fragment, self.text_of(d)))

        self.step_name = stem
        return d

    @staticmethod
    def text_of(d):
        return " | ".join(l["text"] for l in d["lines"])

    @staticmethod
    def rows_of(d):
        """Texts of the list rows a screen drew, top to bottom."""
        rows = []
        for l in d["lines"]:
            if l["x"] == ROW_X and ROW_TOP <= l["y"] <= ROW_BOTTOM \
                    and l["font"] == 1:
                rows.append(l["text"].lstrip("* "))
        return rows

    @staticmethod
    def selected_of(d):
        """Text of the row under the cursor, by the selection rectangle."""
        s = d["selection"]
        if not s["present"]:
            return None
        best = None
        for l in d["lines"]:
            if l["x"] != ROW_X or l["font"] != 1:
                continue
            if s["top"] - 2 <= l["y"] <= s["top"] + s["height"]:
                best = l["text"].lstrip("* ")
        return best

    # ----------------------------------------------------------- navigation --

    def at_home(self):
        d = self.dump()
        return (d["screen"]["name"] in ("MAIN_VFO", "MAIN_MEM")
                and not any(d["modules"].values())
                and d["quickmenu"] is None and not d["tools"]["open"])

    def home(self):
        for _ in range(14):
            if self.at_home():
                if self.dump()["input"]["locked"]:
                    # A row of the popup (or a stray key) locked the keypad:
                    # red held down is the only way out, and the radio must
                    # offer it or every screen behind the lock is dead.
                    self.key_long("RED")
                    if self.dump()["input"]["locked"]:
                        self.fail("the keypad is locked and red held down "
                                  "does not unlock it")
                return self.dump()
            self.key("RED")
        self.fail("cannot get back to the main screen (stuck on %s)"
                  % self.dump()["screen"]["name"])
        raise SystemExit(2)

    def top_menu(self, index):
        """Open the top menu from the main screen, cursor on row `index`."""
        self.home()
        self.key("GREEN")
        if self.dump()["screen"]["name"] != "MENU_TOP":
            self.fail("green on the main screen does not open the menu")
            return
        self.menu_to(index)

    def menu_to(self, index, on="MENU_TOP"):
        """Move the cursor of a menu page (the top menu or Settings)."""
        for _ in range(40):
            d = self.dump()
            if d["screen"]["name"] != on:
                self.fail("left %s while moving the cursor (on %s)"
                          % (on, d["screen"]["name"]))
                return
            cur = d["input"]["menu"]
            if cur == index:
                return
            self.key("DOWN" if index > cur else "UP")
        self.fail("the cursor of %s does not reach row %d" % (on, index))

    def settings_list(self):
        """Main screen -> Menu -> Settings, cursor on the first row."""
        self.top_menu(TOP_MENU.index("Settings"))
        self.key("GREEN")
        if self.dump()["screen"]["name"] != "MENU_SETTINGS":
            self.fail("green on Settings does not open the Settings list")
            return False
        self.menu_to(0, on="MENU_SETTINGS")
        return True

    def qm_to(self, index):
        for _ in range(40):
            q = self.dump()["quickmenu"]
            if q is None:
                self.fail("the quick menu closed while moving the cursor")
                return
            if q["selected"] == index:
                return
            self.key("DOWN" if index > q["selected"] else "UP")
        self.fail("the quick menu cursor does not reach row %d" % index)

    def open_qm(self):
        self.key("P2")
        q = self.dump()["quickmenu"]
        if q is None:
            self.fail("P2 did not open the quick menu")
        return q

    def qm_row(self, label):
        """Index of a quick menu row by label, or None."""
        q = self.dump()["quickmenu"]
        if q is None:
            return None
        for i, row in enumerate(q["rows"]):
            if row["label"] == label:
                return i
        return None

    def qm_goto(self, label):
        """Open the quick menu and press green on the row with that label."""
        self.open_qm()
        index = self.qm_row(label)
        if index is None:
            self.fail("no %r row in the quick menu" % label)
            self.key("RED")
            return False
        self.qm_to(index)
        self.key("GREEN")
        return True

    def assert_alive(self, where):
        """The screen still answers the keys: one press must do something."""
        d0 = self.dump()
        before = (d0["radio"]["rx"], tuple(l["text"] for l in d0["lines"]))
        self.key("DOWN")
        d1 = self.dump()
        after = (d1["radio"]["rx"], tuple(l["text"] for l in d1["lines"]))
        claim = d1["input"]["claim"]
        self.key("UP")

        if claim == 0:
            self.fail("%s: the key was not claimed by any block of the UI" % where)
        elif before == after:
            self.fail("%s: the screen does not answer the keys "
                      "(nothing changed on %s)" % (where, d0["screen"]["name"]))

    # --------------------------------------------------------- tools menu ----

    def tools_open(self):
        """P1 held down on a main screen: the tools menu (docs/UI.md)."""
        self.home()
        self.key_long("P1")
        d = self.dump()

        if not d["tools"]["open"]:
            self.fail("P1 long does not open the tools menu")
            return None

        return d

    def tools_to(self, label):
        """Move the cursor of the tools menu (or of its submenu) by label."""
        for _ in range(12):
            t = self.dump()["tools"]

            if not t["open"]:
                self.fail("the tools menu closed while moving the cursor")
                return False

            if t["rows"][t["selected"]]["label"] == label:
                return True

            self.key("DOWN")

        self.fail("the tools menu cursor does not reach %r" % label)
        return False

    def open_channels(self):
        """The channel list, through the row the tools menu added for it."""
        self.tools_open()
        self.tools_to("Channels")
        self.key("GREEN")

    def walk_quickmenu(self, name, rows, strict=False):
        """
        Every row selectable; the value rows change with P1/P2.

        Returns the rows that changed. With `strict`, a row that shows a value
        and does not change is a failure too; without it the requirement is that
        the table has at least one row that moves, because a table mixes values
        with the receiver's status and the status does not change with a key.
        """
        for i in range(len(rows)):
            self.qm_to(i)
            if self.dump()["quickmenu"]["selected"] != i:
                self.fail("%s: quick menu row %d cannot be selected" % (name, i))

        changed = []
        for i, row in enumerate(rows):
            if row["value"] == "" or row["label"] in NO_EDIT:
                continue
            self.qm_to(i)
            before = self.dump()["quickmenu"]["rows"][i]["value"]
            self.key("P2")
            after = self.dump()["quickmenu"]["rows"][i]["value"]
            if before == after:
                # An integer at its maximum only moves the other way
                self.key("P1")
                after = self.dump()["quickmenu"]["rows"][i]["value"]
            if before != after:
                changed.append("%s %s->%s" % (row["label"], before, after))
                self.capture("%s-%s-edited" % (name, row["label"].lower()
                                               .replace("/", "-").strip()))
            elif strict:
                self.fail("%s: row %s (%s) does not change with P1/P2"
                          % (name, row["label"], before))

        if not changed:
            self.fail("%s: no row changes its value (rows with a value: %s)"
                      % (name, ", ".join(r["label"] for r in rows
                                         if r["value"] != "")))
        return changed

    # --------------------------------------------------------------- phases --

    def phase_start(self):
        """The store this run starts from: empty, and the same every run."""
        self.step_name = "start"
        d = self.capture("main-vfo", screen="MAIN_VFO", need=["FM"])

        self.check(d["radio"]["mode"] == "FM", "the radio does not start in FM")
        self.check(d["radio"]["bank_count"] == 0,
                   "the channel bank does not start empty (%d channels): the "
                   "store is being shared with another run"
                   % d["radio"]["bank_count"])
        self.check(d["radio"]["contact_count"] == 0,
                   "the contact book does not start empty (%d)"
                   % d["radio"]["contact_count"])
        self.check(d["radio"]["name"] == "",
                   "the boot channel has a name (%r): the default channel is "
                   "not zeroed (cps_getDefaultChannel)" % d["radio"]["name"])
        self.check(d["radio"]["rx"] > 0, "the VFO has no frequency")

        # The main screen answers UP and DOWN by stepping the VFO
        before = d["radio"]["rx"]
        self.key("DOWN")
        down = self.dump()["radio"]["rx"]
        self.key("UP")
        up = self.dump()["radio"]["rx"]
        self.check(down != before and up == before,
                   "UP/DOWN do not step the VFO (%d -> %d -> %d)"
                   % (before, down, up))

    def phase_frequency_entry(self):
        """Type a frequency with the keypad and check that it is applied."""
        self.step_name = "frequency entry"
        self.home()

        # The first digit opens the input screen, the rest fill it.
        self.key("4")
        d = self.capture("vfo-input-open", screen="MAIN_VFO_INPUT")
        self.check("Rx:" in self.text_of(d),
                   "the input screen does not show the frequency being typed")

        for digit in ("3", "3", "5", "0", "0"):
            self.key(digit)
        self.capture("vfo-input-433500", screen="MAIN_VFO_INPUT")

        self.key("GREEN")
        d = self.capture("vfo-433500", screen="MAIN_VFO")
        self.check(d["radio"]["rx"] == 433500000,
                   "typed 433.500 MHz, the radio is on %s (%d Hz)"
                   % (d["radio"]["rx_text"], d["radio"]["rx"]))

        # Red cancels: the frequency must not change
        self.key("7")
        self.key("0")
        self.key("0")
        self.key("RED")
        d = self.capture("vfo-input-cancelled", screen="MAIN_VFO")
        self.check(d["radio"]["rx"] == 433500000,
                   "red did not cancel the frequency input")

        # Back to the VFO the radio started on, for the rest of the tour
        self.key("1")
        for digit in ("4", "5", "5", "0", "0"):
            self.key(digit)
        self.key("GREEN")
        d = self.dump()
        self.log("  frequency entry: %s" % d["radio"]["rx_text"])
        self.check(d["radio"]["rx"] == 145500000,
                   "the second entry of 145.500 MHz did not land")

    def goto_mode(self, mode):
        """Reach a mode the way the operator does: the side key and a digit."""
        for _ in range(len(MODE_ORDER) + 1):
            d = self.dump()
            if d["radio"]["mode"] == mode:
                return
            self.home()
            self.combo("MONI", "5")
        self.fail("cannot reach mode %s" % mode)

    def phase_mode(self, mode):
        """One mode, end to end: the main screen, the popup, its page, out."""
        spec = MODES[mode]
        self.step_name = "mode %s" % mode
        self.goto_mode(mode)

        # 1. The main screen of the mode
        d = self.capture("mode-%s" % mode.lower())
        self.check(d["radio"]["mode"] == mode,
                   "the side key does not reach %s (got %s)"
                   % (mode, d["radio"]["mode"]))
        self.check(mode in self.text_of(d),
                   "the main screen does not label the mode %s: %s"
                   % (mode, self.text_of(d)))

        # 2. The quick menu: the declared table, row by row, and a value
        q = self.open_qm()
        if q is None:
            return
        d = self.capture("qm-%s" % mode.lower(), need=[q["title"]])
        labels = [r["label"] for r in q["rows"]]
        if labels != spec["rows"]:
            self.fail("%s: the quick menu has %s, the coverage declares %s"
                      % (mode, labels, spec["rows"]))
        changed = self.walk_quickmenu("qm-%s" % mode.lower(), q["rows"])
        if changed:
            self.log("  %s popup, values changed: %s" % (mode, ", ".join(changed)))
        self.key("RED")
        self.check(self.dump()["quickmenu"] is None,
                   "%s: red does not close the popup" % mode)

        # 3. The Settings page of the mode, all of its rows
        if not self.settings_list():
            return
        self.menu_to(SETTINGS_PAGES.index(mode), on="MENU_SETTINGS")
        self.key("GREEN")
        page = spec["page"]
        d = self.capture("settings-page-%s" % mode.lower(), screen=page)
        q = d["quickmenu"]

        if q is not None:
            page_rows = [r["label"] for r in q["rows"]]
            if page_rows != spec["rows"]:
                self.fail("%s: the page has %s, the quick menu declares %s"
                          % (mode, page_rows, spec["rows"]))
            self.walk_quickmenu("settings-page-%s" % mode.lower(), q["rows"])
            self.capture("settings-page-%s-rows" % mode.lower())
        else:
            # A hand-written page (M17, TETRA, FM): it draws rows, not a table.
            # The TETRA page is one row (AFC) in this build, so one is enough.
            rows = self.rows_of(d)
            self.check(len(rows) >= 1,
                       "%s: the page draws no rows" % mode)
            tops = []
            for _ in range(len(rows)):
                tops.append(self.dump()["selection"]["top"])
                self.key("DOWN")
            if len(set(tops)) != len(rows):
                self.fail("%s: the cursor of the page visits %d of its %d rows"
                          % (mode, len(set(tops)), len(rows)))
            self.capture("settings-page-%s-rows" % mode.lower())

            before = self.text_of(self.dump())
            self.menu_to(0, on=page)
            self.key("GREEN")
            self.key("DOWN")
            self.key("GREEN")
            after = self.text_of(self.dump())
            self.capture("settings-page-%s-edited" % mode.lower())
            self.check(before != after,
                       "%s: no row of the page changes a value" % mode)

        for _ in range(3):
            if self.dump()["screen"]["name"] == "MENU_SETTINGS":
                break
            self.key("RED")
        self.check(self.dump()["screen"]["name"] == "MENU_SETTINGS",
                   "%s: red does not come back to the Settings list" % mode)

        # 4. Out, and the main screen answers again
        self.home()
        self.check(self.dump()["radio"]["mode"] == mode,
                   "%s: leaving the menus changed the mode to %s"
                   % (mode, self.dump()["radio"]["mode"]))
        self.assert_alive("after the %s page" % mode)

    def phase_modes(self):
        """The eleven modes of the registry, one by one, in side-key order."""
        self.step_name = "modes"
        self.home()

        for mode in MODE_ORDER:
            self.phase_mode(mode)

        # A full turn of the side key comes back to where it started
        self.goto_mode("FM")
        seen = []
        for _ in range(len(MODE_ORDER)):
            seen.append(self.dump()["radio"]["mode"])
            self.combo("MONI", "5")
        seen.append(self.dump()["radio"]["mode"])
        self.check(seen[-1] == "FM",
                   "a full turn of the side key does not come back to FM: %s"
                   % seen)
        self.check(seen == MODE_ORDER + ["FM"],
                   "the side key walks %s, the registry is %s"
                   % (seen, MODE_ORDER + ["FM"]))
        self.log("  modes walked: %s" % " -> ".join(seen))

    def phase_quick_menu_fm(self):
        """The FM popup in depth: every row, every editable value, LOCK."""
        self.step_name = "quick menu"
        self.goto_mode("FM")
        self.home()

        q = self.open_qm()
        if q is None:
            return
        rows = q["rows"]
        self.capture("qm-fm", need=["SQL", "PWR", "CTCSS"])
        self.check([r["label"] for r in rows] == MODES["FM"]["rows"],
                   "the FM table is %s, the coverage declares %s"
                   % ([r["label"] for r in rows], MODES["FM"]["rows"]))

        changed = self.walk_quickmenu("qm-fm", rows, strict=True)
        self.log("  FM popup, values changed: %s" % ", ".join(changed))
        self.check(len(changed) >= 8,
                   "only %d FM rows change a value" % len(changed))

        # P1 lowers a value: SQL is an integer with a minimum of 0
        sql = self.qm_row("SQL")
        self.qm_to(sql)
        before = self.dump()["quickmenu"]["rows"][sql]["value"]
        self.key("P1")
        after = self.dump()["quickmenu"]["rows"][sql]["value"]
        self.check(before != after, "P1 does not lower the squelch (%r -> %r)"
                   % (before, after))
        self.capture("qm-fm-sql-edited", need=["SQL"])

        # The LOCK row locks the keypad, and red held down unlocks it again
        lock = self.qm_row("LOCK")
        if lock is not None:
            self.qm_to(lock)
            self.key("P2")
            locked = self.dump()["input"]["locked"]
            self.capture("qm-fm-lock")
            self.check(locked, "the LOCK row does not lock the keypad")
            self.key("RED")
            if self.dump()["quickmenu"] is not None:
                self.fail("red does not close the popup")
            self.key_long("RED")
            self.check(not self.dump()["input"]["locked"],
                       "red held down does not unlock the keypad")
        else:
            self.key("RED")

        self.check(self.dump()["quickmenu"] is None, "red does not close the popup")
        self.home()

    def phase_dmr_tx(self):
        """
        DMR transmits in this build: the two-step frequency entry.

        A receive-only mode asks for one frequency and applies it at the eighth
        digit; a mode with a transmitter asks for RX and then TX and keeps the
        two apart. It is where the difference is visible on a host with no
        baseband to key (docs/SIMULATOR.md, "What this does not cover").
        """
        self.step_name = "dmr tx"
        self.goto_mode("DMR")
        self.home()

        for k in "43050000":
            self.key(k)
        d = self.capture("dmr-tx-rx-entry", screen="MAIN_VFO_INPUT")
        self.check("Tx:" in self.text_of(d),
                   "DMR does not ask for a transmit frequency: %s"
                   % self.text_of(d))

        for k in "43100000":
            self.key(k)
        if self.dump()["screen"]["name"] == "MAIN_VFO_INPUT":
            self.key("GREEN")
        d = self.capture("dmr-tx-applied", screen="MAIN_VFO")
        self.check(d["radio"]["rx"] == 430500000,
                   "DMR RX landed on %d Hz" % d["radio"]["rx"])
        self.check(d["radio"]["tx"] == 431000000,
                   "DMR TX landed on %d Hz (the RX/TX split is not kept)"
                   % d["radio"]["tx"])

        # A receive-only mode, the same digits: no TX field, and the frequency
        # is applied at the eighth digit (there is nothing else to ask for)
        self.goto_mode("TETRA")
        self.home()
        for k in "4305000":
            self.key(k)
        d = self.capture("tetra-rx-entry", screen="MAIN_VFO_INPUT")
        self.check("Tx:" not in self.text_of(d),
                   "TETRA, a receive-only mode, asks for a TX frequency: %s"
                   % self.text_of(d))
        self.key("0")
        d = self.capture("tetra-rx-applied", screen="MAIN_VFO")
        self.check(d["radio"]["rx"] == 430500000
                   and d["radio"]["tx"] == 430500000,
                   "TETRA did not apply the typed frequency to both sides "
                   "(%d/%d)" % (d["radio"]["rx"], d["radio"]["tx"]))

        self.goto_mode("FM")
        self.home()

    def phase_top_menu(self):
        self.step_name = "top menu"
        self.home()
        self.key("GREEN")
        d = self.capture("menu-top", screen="MENU_TOP", need=["Menu"])

        drawn = self.rows_of(d)
        if drawn != TOP_MENU[:len(drawn)]:
            self.fail("the top menu draws %s, the coverage declares %s"
                      % (drawn, TOP_MENU))

        for i, label in enumerate(TOP_MENU):
            self.menu_to(i)
            d = self.capture("menu-top-%02d-%s" % (i, label.lower()))
            if label not in self.text_of(d):
                self.fail("row %s is not drawn in the top menu" % label)

            self.key("GREEN")
            # Banks and Channels read the codeplug, which a host build has no
            # image of: they open, and say nothing. Everything else has to draw
            # a screen, and capture() checks the ink.
            self.capture("menu-top-%02d-%s-open" % (i, label.lower()),
                         allow_blank=(label in ("Banks", "Channels")))
            self.check(self.dump()["screen"]["name"] != "MENU_TOP",
                       "green on %s does not open anything" % label)

            self.home()
            self.assert_alive("after leaving %s" % label)
            self.key("GREEN")

        self.key("RED")
        self.check(self.dump()["screen"]["name"] == "MAIN_VFO",
                   "red does not leave the top menu")

    def phase_settings(self):
        self.step_name = "settings"
        if not self.settings_list():
            return
        d = self.capture("menu-settings", screen="MENU_SETTINGS", need=["Display"])

        drawn = self.rows_of(d)
        if drawn != SETTINGS_PAGES[:len(drawn)]:
            self.fail("the Settings list draws %s, the coverage declares %s"
                      % (drawn, SETTINGS_PAGES))

        for i, label in enumerate(SETTINGS_PAGES):
            self.menu_to(i, on="MENU_SETTINGS")
            slug = label.lower().replace(" ", "-").replace("&", "and")
            d = self.capture("settings-%02d-%s" % (i, slug))
            if label not in self.text_of(d):
                self.fail("page %s is not in the Settings list" % label)

            self.key("GREEN")
            self.capture("settings-%02d-%s-page" % (i, slug))

            self.key("DOWN")
            self.key("DOWN")
            self.capture("settings-%02d-%s-page-rows" % (i, slug))

            for _ in range(4):
                if self.dump()["screen"]["name"] == "MENU_SETTINGS":
                    break
                self.key("RED")
            if self.dump()["screen"]["name"] != "MENU_SETTINGS":
                self.fail("red does not come back from %s" % label)
                self.home()
                self.top_menu(TOP_MENU.index("Settings"))

        # A value that the radio must apply at once: Display -> Brightness
        self.settings_list()
        self.menu_to(0, on="MENU_SETTINGS")
        self.key("GREEN")
        before = self.dump()["settings"]["brightness"]
        self.key("GREEN")
        self.key("DOWN")
        after = self.dump()["settings"]["brightness"]
        self.key("GREEN")
        self.capture("settings-display-brightness", need=["Brightness"])
        self.check(before != after,
                   "Display -> Brightness does not change (%d)" % before)
        self.key("RED")
        self.key("RED")

        # And one that lives in the radio: Radio -> Squelch
        self.settings_list()
        self.menu_to(SETTINGS_PAGES.index("Radio"), on="MENU_SETTINGS")
        self.key("GREEN")
        self.menu_to(3, on="SETTINGS_RADIO")
        before = self.dump()["settings"]["sql"]
        self.key("GREEN")
        self.key("DOWN")
        after = self.dump()["settings"]["sql"]
        self.key("GREEN")
        self.capture("settings-radio-squelch", need=["Squelch"])
        self.check(before != after,
                   "Radio -> Squelch does not change (%d)" % before)
        for _ in range(3):
            self.key("RED")

        # The clock: its page and the digit entry it opens
        self.settings_list()
        self.menu_to(SETTINGS_PAGES.index("Time & Date"), on="MENU_SETTINGS")
        self.key("GREEN")
        self.capture("settings-timedate", screen="SETTINGS_TIMEDATE")
        self.key("GREEN")
        d = self.capture("settings-timedate-set", screen="SETTINGS_TIMEDATE_SET")
        self.check("__/__/__" in self.text_of(d),
                   "the clock entry does not show empty fields: %s"
                   % self.text_of(d))

        # Type a date; the entry takes digits (the clock needs all ten, so it
        # does not apply) and red leaves it. The first red may only drop the
        # edit mode, so red until the clock page is back.
        for k in ("2", "5", "1", "1", "2", "6"):
            self.key(k)
        d = self.capture("settings-timedate-typed")
        self.check("25/11/26" in self.text_of(d),
                   "the clock entry does not take the typed digits: %s"
                   % self.text_of(d))

        for _ in range(3):
            if self.dump()["screen"]["name"] == "SETTINGS_TIMEDATE":
                break
            self.key("RED")
        self.check(self.dump()["screen"]["name"] == "SETTINGS_TIMEDATE",
                   "red does not come back to the clock page (on %s)"
                   % self.dump()["screen"]["name"])
        self.key("RED")
        self.key("RED")
        self.home()

    def phase_tools(self):
        """P1 held down: the tools menu, its five rows and the mode submenu."""
        self.step_name = "tools"
        self.goto_mode("FM")
        self.home()

        if self.tools_open() is None:
            return

        d = self.capture("tools-list", screen="TOOLS_MENU",
                         need=["TOOLS", "Mode", "Scan", "VFO scan", "Spectrum",
                               "Channels"])
        self.check(d["radio"]["tx_disable"],
                   "the tools menu does not disable the transmitter")
        self.check(d["tools"]["selected"] == 0,
                   "the tools menu does not open on the first row")

        rows = [r["label"] for r in d["tools"]["rows"]]
        self.check(rows == ["Mode", "Scan", "VFO scan", "Spectrum", "Channels"],
                   "the rows of the tools menu are %s, not the five of the "
                   "design" % rows)
        self.check(d["tools"]["rows"][0]["value"] == "FM",
                   "the Mode row does not show the mode in use: %r"
                   % d["tools"]["rows"][0]["value"])

        # Walk the five rows: each one selectable, in order
        for index, label in enumerate(rows):
            if not self.tools_to(label):
                return
            d = self.capture("tools-row-%d" % index)
            self.check(d["tools"]["selected"] == index,
                       "row %d (%s) cannot be selected" % (index, label))

        # The cursor wraps at both ends, and P1/P2 move it as well (the C62 has
        # no arrow pair of its own: the operator has UP/DOWN and P1/P2)
        self.key("DOWN")
        d = self.dump()
        self.check(d["tools"]["selected"] == 0,
                   "DOWN on the last row does not wrap to the first one")
        self.key("UP")
        d = self.dump()
        self.check(d["tools"]["selected"] == len(rows) - 1,
                   "UP on the first row does not wrap to the last one")
        self.key("P2")
        d = self.dump()
        self.check(d["tools"]["selected"] == 0, "P2 does not move the cursor down")
        self.key("P1")
        d = self.dump()
        self.check(d["tools"]["selected"] == len(rows) - 1,
                   "P1 does not move the cursor up")
        self.capture("tools-rows-walked")

        # The mode submenu: opens on the mode in use, marks every mode TX or RX
        # as this build really is, and red goes back to the list
        self.tools_to("Mode")
        self.key("GREEN")
        d = self.capture("tools-modes", screen="TOOLS_MENU", need=["MODE"])
        self.check(d["tools"]["modes"], "green on Mode does not open the submenu")
        self.check(len(d["tools"]["rows"]) == len(MODE_ORDER),
                   "the submenu lists %d modes, the image has %d"
                   % (len(d["tools"]["rows"]), len(MODE_ORDER)))
        self.check(d["tools"]["rows"][d["tools"]["selected"]]["label"] == "FM",
                   "the submenu does not open on the mode in use")

        labels = [r["label"] for r in d["tools"]["rows"]]
        if labels != MODE_ORDER:
            self.fail("the submenu lists %s, the coverage declares %s"
                      % (labels, MODE_ORDER))

        for row in d["tools"]["rows"]:
            want = "TX" if MODES[row["label"]]["tx"] else "RX"
            if row["value"] != want:
                self.fail("%s is marked %s in the mode submenu, the coverage "
                          "says %s" % (row["label"], row["value"], want))

        # Into the submenu, down to the last mode and back out with red
        self.tools_to(MODE_ORDER[-1])
        d = self.capture("tools-modes-last")
        self.check(d["tools"]["rows"][d["tools"]["selected"]]["label"]
                   == MODE_ORDER[-1],
                   "the submenu cursor does not reach %s" % MODE_ORDER[-1])
        self.key("RED")
        d = self.capture("tools-modes-back")
        self.check(d["tools"]["open"] and not d["tools"]["modes"],
                   "red in the submenu leaves the menu instead of going back")
        self.check(d["tools"]["selected"] == 0,
                   "the list cursor is not back on the Mode row")

        # Green twice: open the submenu and apply the mode the cursor is on,
        # which is the one already in use, so the radio does not move and the
        # tour exercises apply-and-return without changing what follows
        self.key("GREEN")
        self.key("GREEN")
        d = self.capture("tools-applied", screen="MAIN_VFO")
        self.check(not d["tools"]["open"],
                   "choosing a mode does not close the menu")
        self.check(d["radio"]["mode"] == "FM",
                   "choosing FM left the radio in %s" % d["radio"]["mode"])
        self.check(not d["radio"]["tx_disable"],
                   "the transmitter is not re-armed after the menu closes")

        self.assert_alive("after the tools menu")

    def phase_channels(self):
        """The in-radio channel bank: create, edit the frequency, save, tune."""
        self.step_name = "channels"
        self.goto_mode("FM")
        self.home()

        self.open_channels()
        d = self.capture("channels-list-empty")
        self.check(d["modules"]["channels"],
                   "the Channels row of the tools menu does not open the bank")
        self.check("No channels" in self.text_of(d),
                   "the empty bank does not say so")

        self.key("P1")
        d = self.capture("channels-editor-new", need=["NEW CHANNEL"])
        self.check(d["header"]["title"] == "NEW CHANNEL",
                   "P1 in the list does not open a new channel")

        # RX frequency: green enters the field, UP/DOWN walk the digits
        self.key("DOWN")
        self.key("DOWN")
        rx_before = self.text_of(d)
        self.key("GREEN")
        self.key("DOWN")
        self.key("DOWN")
        self.key("DOWN")
        self.key("GREEN")
        d = self.capture("channels-editor-rx")
        self.check(rx_before != self.text_of(d),
                   "the RX frequency of the editor does not change")

        # Save: P2 is SAVE in the editor
        self.key("P2")
        d = self.capture("channels-list-saved")
        self.check(d["radio"]["bank_count"] == 1,
                   "the channel was not written to the bank (%d)"
                   % d["radio"]["bank_count"])
        self.check("CHANNELS 1/128" in d["header"]["title"],
                   "the list does not count the new channel: %r"
                   % d["header"]["title"])

        # Tune it: green on the entry loads it and closes the list
        self.key("GREEN")
        d = self.capture("channels-tuned", screen="MAIN_MEM")
        self.check(d["radio"]["mode"] == "FM", "the tuned channel is not FM")
        self.check(d["radio"]["rx"] > 0, "the tuned channel has no frequency")
        self.check(d["radio"]["name"].startswith("CH"),
                   "the tuned channel did not land in state.channel: %r"
                   % d["radio"]["name"])
        self.check("CH 1" in self.text_of(d),
                   "the memory screen does not show the channel name")

        # And the editor opens the stored record: P2 on the entry
        self.open_channels()
        self.capture("channels-list-after-tune")
        self.key("P2")
        d = self.capture("channels-editor-existing", need=["EDIT"])
        self.check(d["header"]["title"].startswith("EDIT"),
                   "P2 on an entry does not open it for editing: %r"
                   % d["header"]["title"])
        for _ in range(3):
            self.key("RED")
        self.home()
        self.assert_alive("after the channel bank")

    def phase_contacts(self):
        self.step_name = "contacts"
        self.home()
        self.top_menu(TOP_MENU.index("Contacts"))
        self.key("GREEN")
        d = self.capture("contacts-empty", need=["CONTACTS"])
        self.check("No contacts" in self.text_of(d),
                   "the empty contact book does not say so")

        self.key("P1")
        self.capture("contacts-editor-new", need=["NEW CONTACT"])

        # One character of the callsign, then save
        self.key("UP")
        self.capture("contacts-editor-call")
        self.key("GREEN")
        d = self.capture("contacts-saved", need=["CONTACTS"])
        self.check(d["radio"]["contact_count"] == 1,
                   "the contact was not written (%d)" % d["radio"]["contact_count"])

        self.key("P2")
        d = self.capture("contacts-editor-existing", need=["EDIT"])
        self.check(d["header"]["title"].startswith("EDIT"),
                   "P2 on a contact does not open it")
        self.key("RED")
        self.key("DOWN")
        self.capture("contacts-list")
        self.home()
        self.assert_alive("after the contact book")

    def phase_messages(self):
        """Inbox, view, composer, typing, send, the delivery page and sent."""
        self.step_name = "messages"

        # One message in the inbox, through the store the screens read
        self.inject("message", "1234", "hola", "desde", "el", "aire")
        self.home()
        self.top_menu(TOP_MENU.index("Messages"))
        self.key("GREEN")
        d = self.capture("messages-inbox", need=["INBOX"])
        self.check("1234" in self.text_of(d),
                   "the injected message is not in the inbox: %s"
                   % self.text_of(d))

        # Open it: the view page
        self.key("GREEN")
        d = self.capture("messages-view", screen="MSG_VIEW")
        self.check("desde el aire" in self.text_of(d),
                   "the message view does not show the text: %s"
                   % self.text_of(d))

        # Reply from the view: the composer, with the peer already in TO
        self.key("GREEN")
        d = self.capture("messages-compose", screen="MSG_COMPOSE",
                         need=["COMPOSE", "TP", "TO", "TXT", "SEND"])
        self.check("1234" in self.text_of(d),
                   "the reply does not carry the peer: %s" % self.text_of(d))

        # Type on TXT: green starts the multi-tap composer, digits type
        for _ in range(6):
            if self.selected_of(self.dump()) == "TXT":
                break
            self.key("DOWN")
        self.check(self.selected_of(self.dump()) == "TXT",
                   "the composer cursor does not reach TXT")
        self.key("GREEN")
        for k in ("2", "2", "2"):
            self.key(k)
        d = self.capture("messages-compose-typed")
        self.check("C" in self.text_of(d),
                   "the composer does not take characters: %s"
                   % self.text_of(d))
        self.key("GREEN")

        # SEND: the delivery page
        for _ in range(6):
            if self.selected_of(self.dump()) == "SEND":
                break
            self.key("DOWN")
        self.check(self.selected_of(self.dump()) == "SEND",
                   "the composer cursor does not reach SEND: %s"
                   % self.selected_of(self.dump()))
        self.capture("messages-compose-send")
        self.key("GREEN")
        d = self.capture("messages-status", screen="MSG_STATUS")
        self.check(d["header"]["title"] == "1234",
                   "the delivery page does not name the peer: %r"
                   % d["header"]["title"])
        for _ in range(4):
            if self.dump()["screen"]["name"] == "MENU_MESSAGES":
                break
            self.key("RED")

        # The sent folder, and back
        self.key("P1")
        self.capture("messages-sent", need=["SENT"])
        self.key("P1")

        # The composer from the quick menu (a message with no peer yet)
        self.home()
        if self.qm_goto("MSG"):
            d = self.capture("messages-compose-menu", screen="MSG_COMPOSE")
            self.check("TP" in self.text_of(d), "the composer has no TP row")
            for _ in range(3):
                self.key("DOWN")
            self.capture("messages-compose-menu-rows")
            for _ in range(3):
                self.key("RED")
        self.home()
        self.assert_alive("after the messages")

    def phase_zones(self):
        self.step_name = "zones"
        self.home()
        self.top_menu(TOP_MENU.index("Zones"))
        self.key("GREEN")
        d = self.capture("zones-list", need=["ZONES"])
        self.check("R" in self.text_of(d),
                   "the zone list does not show the repeater zone")

        self.key("GREEN")
        self.capture("zones-entries", need=["145.60000"])
        for _ in range(3):
            self.key("DOWN")
        self.capture("zones-entries-scrolled")

        # Green tunes the entry and leaves for the VFO
        self.key("GREEN")
        d = self.capture("zones-tuned", screen="MAIN_VFO")
        self.check(d["radio"]["rx"] == 145675000,
                   "the fourth entry of zone R (R3 145.675 MHz) did not tune "
                   "(rx %d)" % d["radio"]["rx"])
        self.home()
        self.assert_alive("after the zone browser")

    def phase_aprs(self):
        """The APRS screens: the station list, a station, the messages, setup."""
        self.step_name = "aprs"

        # A station heard on the air and a message addressed to us, through the
        # service's own receive path (the control channel's `inject`).
        self.inject("aprs-station", "EA5ABC", "38.87", "-0.07", "Bench",
                    "station")
        self.inject("aprs-message", "EA5XYZ", "hola", "desde", "el", "aire")
        self.home()

        # The station list is reached with a long P2 in FM (docs/UI.md). The
        # keypad reports the press and the long press as two events, so the
        # popup opens first and the long press has to take the keypad back.
        self.sim.cmd("hold P2 1500")
        d = self.capture("aprs-stations", screen="MENU_APRS")
        self.check(d["quickmenu"] is None, "the popup stayed open over APRS")
        self.check("EA5ABC" in self.text_of(d),
                   "the injected station is not on the list: %s"
                   % self.text_of(d))

        # Green on a station: its detail page, P2 steps to the next one
        self.key("GREEN")
        d = self.capture("aprs-detail", screen="APRS_DETAIL")
        self.check("Bench station" in self.text_of(d),
                   "the detail page does not show the comment: %s"
                   % self.text_of(d))
        self.key("P2")
        self.capture("aprs-detail-next")
        self.key("RED")

        # P1 from the station list: the APRS inbox, then the message, then the
        # reply editor
        self.check(self.dump()["screen"]["name"] == "MENU_APRS",
                   "red does not come back to the station list")
        self.key("P1")
        d = self.capture("aprs-messages", screen="APRS_MSGS")
        self.check("EA5XYZ" in self.text_of(d),
                   "the APRS inbox does not show the message: %s"
                   % self.text_of(d))
        self.key("GREEN")
        d = self.capture("aprs-message-view", screen="APRS_MSG_VIEW")
        self.check("hola desde" in self.text_of(d),
                   "the APRS message view does not show the text: %s"
                   % self.text_of(d))
        self.key("GREEN")
        d = self.capture("aprs-message-reply", screen="APRS_MSG_EDIT")
        self.check("EA5XYZ" in d["header"]["right"]
                   or "EA5XYZ" in self.text_of(d),
                   "the reply editor does not address the sender: %s"
                   % self.text_of(d))
        # One character with the keypad, the way the editor takes them
        # (up/down choose, P1 adds)
        self.key("UP")
        self.key("P1")
        self.capture("aprs-message-reply-typed")
        self.key("RED")
        self.check(self.dump()["screen"]["name"] == "APRS_MSGS",
                   "red does not leave the reply editor")
        self.key("RED")

        # And the setup page, from the station list
        if self.dump()["screen"]["name"] == "MAIN_VFO":
            self.sim.cmd("hold P2 1500")
        self.key("P2")
        d = self.capture("aprs-setup", screen="APRS_CFG", need=["Decode"])
        self.check("APRS" in d["header"]["title"].upper(),
                   "the APRS row does not open the setup page")

        # The fields change with P1/P2 and land in the settings record; the page
        # marks the selected row with a hollow rectangle and yellow text, not
        # with the black-on-white of the other lists.
        before = (d["aprs"]["decode"], d["aprs"]["beacon"], d["aprs"]["ssid"])
        changed = False
        for _ in range(4):
            self.key("DOWN")
            self.key("P2")
            d = self.dump()
            after = (d["aprs"]["decode"], d["aprs"]["beacon"], d["aprs"]["ssid"])
            if after != before:
                changed = True
                break
        self.capture("aprs-setup-edited", screen="APRS_CFG")
        self.check(changed, "no field of the APRS setup changes with P2")
        for _ in range(3):
            self.key("RED")
        self.home()

    def phase_packet(self):
        self.step_name = "packet modem"
        self.goto_mode("FM")
        self.home()
        if self.qm_goto("PKT"):
            d = self.capture("packet-config", screen="PKT_CFG",
                             need=["Enable", "Modem"])
            before = d["packet"]["enabled"]
            self.key("P2")
            d = self.capture("packet-config-enabled")
            self.check(before != d["packet"]["enabled"],
                       "the Enable row of the packet page does not change")

            # The counters page is the eighth and last row of the page
            for _ in range(7):
                self.key("DOWN")
            self.capture("packet-config-status-row")
            self.key("GREEN")
            self.capture("packet-status", screen="PKT_STATUS",
                         need=["Frames TX", "FCS"])
            for _ in range(3):
                self.key("RED")
        self.home()
        self.assert_alive("after the packet modem")

    def phase_spectrum(self):
        self.step_name = "spectrum"
        self.goto_mode("FM")
        self.home()
        if self.qm_goto("ESPECTRO"):
            d = self.capture("spectrum", screen="SPECTRUM")
            self.check("ESPECTRO" in self.text_of(d),
                       "the analyser is not on the screen: %s" % self.text_of(d))

            self.key("P2")
            q = self.dump()["quickmenu"]
            if q is None:
                self.fail("P2 does not open the analyser's own quick menu")
            else:
                self.capture("spectrum-menu", need=["SPAN"])
                for i in range(len(q["rows"])):
                    self.qm_to(i)
                    if self.dump()["quickmenu"]["selected"] != i:
                        self.fail("the analyser's row %d cannot be selected" % i)
                self.key("RED")
            self.key("RED")
        self.home()
        self.assert_alive("after the analyser")

    def phase_backup(self):
        """
        The internal backup, end to end: take one, change the radio, restore it
        and check that the six stores came back (docs/BACKUP.md).

        The record is a file of this run (`backup.img` in the state directory the
        tour gave the simulator), so what is restored here is what this run
        wrote and not something left behind by another one.
        """
        self.step_name = "backup"
        self.goto_mode("FM")
        self.home()

        d = self.dump()
        before = {
            "brightness": d["settings"]["brightness"],
            "sql": d["settings"]["sql"],
            "channels": d["radio"]["bank_count"],
            "contacts": d["radio"]["contact_count"],
            "name": d["radio"]["name"],
        }
        self.check(before["channels"] > 0,
                   "the bank is empty: there is nothing to back up")
        self.check(before["contacts"] > 0,
                   "the contact book is empty: there is nothing to back up")

        # Menu -> Backup: the two rows the generic branch never reached
        self.top_menu(TOP_MENU.index("Backup"))
        self.key("GREEN")
        d = self.capture("backup-menu", screen="MENU_BACKUP_RESTORE",
                         need=["Backup", "Restore"])
        self.check(len(self.rows_of(d)) == 2,
                   "the backup menu has %d rows, not 2" % len(self.rows_of(d)))

        # The Backup row, and green on it: the state, not a confirmation
        self.key("GREEN")
        d = self.capture("backup-screen", screen="MENU_BACKUP",
                         need=["channels", "contacts"])
        self.check("Save the stores?" in self.text_of(d),
                   "the backup screen does not say what green does: %s"
                   % self.text_of(d))
        self.key("GREEN")
        d = self.capture("backup-done", screen="MENU_BACKUP")
        self.check("BACKUP OK" in self.text_of(d),
                   "the backup does not report OK: %s" % self.text_of(d))

        # Back to the pair, and out: the record is on flash now
        self.key("RED")
        d = self.capture("backup-menu-after", screen="MENU_BACKUP_RESTORE")
        self.key("RED")
        self.check(self.dump()["screen"]["name"] == "MENU_TOP",
                   "red from the backup menu does not come back to the menu")
        self.home()

        # Change the radio: a setting and the bank
        self.settings_list()
        self.menu_to(0, on="MENU_SETTINGS")
        self.key("GREEN")
        self.key("GREEN")
        self.key("DOWN")
        self.key("GREEN")
        changed = self.dump()["settings"]["brightness"]
        self.check(changed != before["brightness"],
                   "Display -> Brightness did not change (%d)" % changed)
        self.key("RED")
        self.key("RED")

        self.open_channels()
        self.key("P1")            # new channel, ready to save
        self.key("P2")            # SAVE
        d = self.capture("backup-bank-changed")
        self.check(d["radio"]["bank_count"] == before["channels"] + 1,
                   "the extra channel was not saved (%d)"
                   % d["radio"]["bank_count"])
        self.home()

        # Now the restore, and the question it asks first
        self.top_menu(TOP_MENU.index("Backup"))
        self.key("GREEN")
        self.key("DOWN")          # the Restore row
        self.key("GREEN")
        d = self.capture("restore-ask", screen="MENU_RESTORE",
                         need=["Restore it?"])
        self.check("Yes" in self.text_of(d) and "No" in self.text_of(d),
                   "the restore does not offer yes/no: %s" % self.text_of(d))

        # Red is "no", and nothing is written: the radio keeps what it has
        self.key("RED")
        d = self.capture("restore-cancelled", screen="MENU_BACKUP_RESTORE")
        self.check(self.dump()["radio"]["bank_count"] == before["channels"] + 1,
                   "cancelling the restore changed the bank")

        # Green is "yes": the six stores come back
        self.key("GREEN")
        self.key("DOWN")
        self.key("GREEN")
        self.capture("restore-ask-again", screen="MENU_RESTORE")
        self.key("GREEN")
        d = self.capture("restore-done", screen="MENU_RESTORE")
        self.check("RESTORE OK" in self.text_of(d),
                   "the restore does not report OK: %s" % self.text_of(d))

        after = self.dump()
        self.check(after["settings"]["brightness"] == before["brightness"],
                   "the brightness did not come back (%d, was %d)"
                   % (after["settings"]["brightness"], before["brightness"]))
        self.check(after["settings"]["sql"] == before["sql"],
                   "the squelch did not come back")
        self.check(after["radio"]["bank_count"] == before["channels"],
                   "the bank did not come back (%d, was %d)"
                   % (after["radio"]["bank_count"], before["channels"]))
        self.check(after["radio"]["contact_count"] == before["contacts"],
                   "the contact book did not come back (%d, was %d)"
                   % (after["radio"]["contact_count"], before["contacts"]))
        self.check(after["radio"]["name"] == before["name"],
                   "the radio did not come back to its channel (%r, was %r)"
                   % (after["radio"]["name"], before["name"]))

        # The bank on the screen too, not only in the state behind it
        self.home()
        self.open_channels()
        d = self.capture("backup-bank-restored")
        self.check(d["radio"]["bank_count"] == before["channels"],
                   "the restored bank does not list what it had")
        self.home()
        self.assert_alive("after the backup and the restore")

    def phase_low_battery(self):
        """
        The low battery screen, last: leaving it latches `state.emergency`, so
        it cannot be un-done from the keypad and a second one never comes.
        """
        self.step_name = "low battery"
        self.home()
        self.sim.cmd("vbat 1.0")
        self.sim.cmd("sleep 900")

        d = self.capture("low-battery", screen="LOW_BAT")
        self.check("emergency" in self.text_of(d).lower(),
                   "the low battery screen does not tell the operator what to "
                   "do: %s" % self.text_of(d))

        self.key("1")
        d = self.capture("low-battery-dismissed", screen="MAIN_VFO")
        self.check(d["radio"]["vbat"] < 7.0,
                   "the battery voltage went back up by itself (%s)"
                   % d["radio"]["vbat"])
        self.sim.cmd("vbat 8.2")
        self.sim.cmd("sleep 300")

    # ------------------------------------------------------------- coverage --

    def phase_coverage(self):
        """The declaration against what the tour has just walked."""
        self.step_name = "coverage"

        missing = [s for s in SCREENS if s not in self.visited]
        extra = sorted(self.visited - set(SCREENS) - set(UNREACHABLE))
        dead = sorted(self.visited & set(UNREACHABLE))

        if missing:
            self.fail("the tour does not visit %d declared screens: %s"
                      % (len(missing), ", ".join(missing)))
        if extra:
            self.fail("the tour visited %d screens the declaration does not "
                      "list (add them to SCREENS): %s"
                      % (len(extra), ", ".join(extra)))
        if dead:
            self.fail("the tour reached screens declared unreachable: %s"
                      % ", ".join(dead))

        self.log("  coverage: %d of %d screens, %d modes, %d top menu rows, "
                 "%d settings pages"
                 % (len(SCREENS) - len(missing), len(SCREENS), len(MODE_ORDER),
                    len(TOP_MENU), len(SETTINGS_PAGES)))

    # ------------------------------------------------------------------ run --

    def run(self):
        started = time.time()

        self.phase_start()
        self.phase_frequency_entry()
        self.phase_top_menu()
        self.phase_settings()
        self.phase_modes()
        self.phase_quick_menu_fm()
        self.phase_dmr_tx()
        self.phase_tools()
        self.phase_channels()
        self.phase_contacts()
        self.phase_messages()
        self.phase_zones()
        self.phase_aprs()
        self.phase_packet()
        self.phase_spectrum()
        self.phase_backup()
        self.phase_low_battery()

        self.step_name = "end"
        self.home()
        self.assert_alive("at the end of the tour")
        self.capture("end-main-vfo", screen="MAIN_VFO")

        self.phase_coverage()

        elapsed = time.time() - started
        screens = sorted({r["screen"] for r in self.records})

        with open(os.path.join(self.outdir, "tour.json"), "w") as f:
            json.dump({"records": self.records, "failures": self.failures,
                       "findings": self.findings, "seconds": elapsed,
                       "coverage": {"screens": sorted(self.visited),
                                    "declared": SCREENS,
                                    "unreachable": sorted(UNREACHABLE)}}, f,
                      indent=1)

        print("")
        print("screens visited : %d" % len(self.records))
        print("distinct screens: %d (%s)" % (len(screens), ", ".join(screens)))
        print("pictures        : %s" % self.shots)
        print("time            : %.1f s" % elapsed)

        if self.findings:
            print("")
            print("findings (the firmware, not the tour):")
            for f in self.findings:
                print("  - %s" % f)

        if self.failures:
            print("")
            print("FAILURES (%d):" % len(self.failures))
            for f in self.failures:
                print("  - %s" % f)
            return 1

        print("")
        print("the tour passed: %d screens, no blank screen, every row "
              "selectable, every editable value changes" % len(self.records))
        return 0


def coverage_manifest(path):
    """
    The declaration as plain text, one fact per line.

    tests/unit/sim_coverage.cpp reads this and compares it with the firmware:
    the registry of modes, the menu tables of ui.c, the uiScreen enum and the
    screens its own key walk reaches. The format is deliberately dull so the
    C++ side needs no parser:

        mode <TITLE> <tx|rx> <page> <row,row,row>
        top <LABEL>
        settings <LABEL>
        screen <NAME>
        unreachable <NAME>
    """
    lines = ["# generated by tools/simulator/tour.py --coverage-manifest",
             "# the coverage the tour declares; see docs/SIMULATOR.md"]
    for mode in MODE_ORDER:
        spec = MODES[mode]
        lines.append("mode %s %s %s %s"
                     % (mode, "tx" if spec["tx"] else "rx", spec["page"],
                        ",".join(spec["rows"])))
    for label in TOP_MENU:
        lines.append("top %s" % label)
    for label in SETTINGS_PAGES:
        lines.append("settings %s" % label)
    for name in SCREENS:
        lines.append("screen %s" % name)
    for name in sorted(UNREACHABLE):
        lines.append("unreachable %s" % name)

    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="sim-tour",
                    help="directory for the pictures and the dumps")
    ap.add_argument("--bin", default=None, help="path to openrtx_c62_sim")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--coverage-manifest", default=None,
                    help="write the declared coverage to this file and exit")
    args = ap.parse_args()

    if args.coverage_manifest:
        coverage_manifest(args.coverage_manifest)
        return 0

    tour = Tour(args.out, binary=args.bin, quiet=args.quiet)
    try:
        return tour.run()
    except SimError as exc:
        print("simulator error: %s" % exc)
        return 2
    finally:
        tour.sim.quit()
        shutil.rmtree(tour.tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
