#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The menu tour of the C62 simulator: every screen, walked with keys.

    meson compile -C build_sim openrtx_c62_sim
    python3 tools/simulator/tour.py [--out DIR]

It drives the real firmware in the host emulator through the control channel
(docs/SIMULATOR.md), the way the operator drives the radio: green for OK, red
for back, P1/P2, UP/DOWN, digits, a long press where a screen only offers one.
For every screen it writes a PNG and the text dump of what the screen shows, and
it fails — loudly, with the step and the reason — when

  * a screen comes out blank (no ink, or no text in the dump);
  * a row of a list or of the quick menu cannot be selected;
  * a value that should be editable does not change when the key that edits it
    is pressed;
  * a key press is not claimed by any block of the UI (the "deaf screen"
    defect, see enum uiKeyClaim);
  * after leaving a screen the main screen no longer answers the keys.

The cases that failed on hardware are asserted on purpose: entering a frequency
with the keypad, the quick menu of P2, editing and saving a channel, walking the
contact book and the messages, changing mode and coming back, entering and
leaving AM, and every screen still answering after it closes.

The run is reproducible: it gets its own settings record and its own channel /
contact store (XDG_STATE_HOME and OPENRTX_SIM_STORE point into a temporary
directory), so what it sees does not depend on what another run of the emulator
left behind.
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

# The eleven modes of the registry, in the order the side key walks them.
MODES = ["FM", "M17", "TETRA", "DMR", "NXDN", "dPMR", "P25", "YSF", "DSTAR",
         "AM", "WFM"]

# Rows of the top menu, in menu order.
TOP_MENU = ["Banks", "Channels", "Zones", "Contacts", "Messages", "GPS",
            "Settings", "Info", "About"]

# Settings pages, in menu order.
SETTINGS_PAGES = ["Display", "Time & Date", "GPS", "Radio", "Audio", "M17",
                  "TETRA", "FM", "DMR", "NXDN", "dPMR", "P25", "YSF", "DSTAR",
                  "AM", "WFM", "Accessibility", "Default Settings"]

# Fraction of lit pixels below which a screen is considered blank.
MIN_INK = 0.005


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

        env = {
            # A settings record of this run, not of the last one
            "XDG_STATE_HOME": os.path.join(self.tmp, "state"),
            # The channel bank and the contact book of this run
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

    def capture(self, name, screen=None, need=(), allow_blank=False):
        """Take the picture and the dump of the screen, and check it is a screen."""
        self.n += 1
        stem = "%03d-%s" % (self.n, name)
        png = self.sim.shot(stem + ".png")
        d = self.dump()
        text = self.sim.dump_text()

        with open(os.path.join(self.shots, stem + ".txt"), "w") as f:
            f.write(text)

        self.records.append({"step": stem, "screen": d["screen"]["name"],
                             "png": png, "dump": d})

        if screen is not None and d["screen"]["name"] != screen:
            self.fail("%s: expected screen %s, got %s"
                      % (stem, screen, d["screen"]["name"]))

        ink, colours = png_ink(png)
        if not allow_blank and (ink < MIN_INK or not d["lines"]):
            self.fail("%s: blank screen (ink %.3f, %d text lines)"
                      % (stem, ink, len(d["lines"])))

        blob = "\n".join(l["text"] for l in d["lines"]) + "\n" + d["header"]["title"]
        for fragment in need:
            if fragment not in blob:
                self.fail("%s: %r is not on the screen (screen: %s)"
                          % (stem, fragment, " | ".join(l["text"] for l in d["lines"])))

        self.step_name = stem
        return d

    # ----------------------------------------------------------- navigation --

    def at_home(self):
        d = self.dump()
        return (d["screen"]["name"] in ("MAIN_VFO", "MAIN_MEM")
                and not any(d["modules"].values()) and d["quickmenu"] is None)

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
        self.top_menu(6)
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

    # --------------------------------------------------------------- phases --

    def phase_main(self):
        self.step_name = "main screen"
        d = self.capture("main-vfo", screen="MAIN_VFO", need=["FM"])
        self.check(d["radio"]["rx"] > 0, "the VFO has no frequency")
        self.check(d["radio"]["mode"] == "FM", "the radio does not start in FM")

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
        start = self.dump()["radio"]["rx"]

        # The first digit opens the input screen, the rest fill it.
        self.key("4")
        d = self.capture("vfo-input-open", screen="MAIN_VFO_INPUT")
        self.check("Rx:" in " ".join(l["text"] for l in d["lines"]),
                   "the input screen does not show the frequency being typed")

        for digit in ("3", "3", "5", "0", "0"):
            self.key(digit)
        d = self.capture("vfo-input-433500", screen="MAIN_VFO_INPUT")

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

    def phase_quick_menu(self):
        """The popup of P2 in every mode: rows selectable, values editable."""
        self.step_name = "quick menu"
        self.home()

        # FM first, which is the table with the most rows and the one the
        # operator uses: every row selectable, every value row editable.
        q = self.open_qm()
        rows = q["rows"]
        self.capture("qm-fm", need=["SQL", "PWR", "CTCSS"])
        self.check(len(rows) >= 13, "the FM table has %d rows" % len(rows))

        for i in range(len(rows)):
            self.qm_to(i)
            d = self.dump()
            if d["quickmenu"]["selected"] != i:
                self.fail("quick menu row %d cannot be selected" % i)

        self.capture("qm-fm-last-row")

        # Every row that shows a value must change when P2 is pressed, except
        # the ones that are read-only by design (the receiver's status) and the
        # two that are exercised on their own:
        #  - LOCK locks the keypad, which is the point, and would make every
        #    later step deaf;
        #  - 1750 is a momentary burst: the FSM clears it on the next key that
        #    is not '#', so it never reads back as ON.
        special = {"LOCK", "1750"}
        read_only = {"SYNC", "CALL", "TYPE", "NAC", "MY", "UR", "RPT", "TXT",
                     "CC", "LVL", "SNR", "CH", "DEST"}
        changed = []
        for i, row in enumerate(rows):
            if row["value"] == "" or row["label"] in read_only \
                    or row["label"] in special:
                continue
            self.qm_to(i)
            before = self.dump()["quickmenu"]["rows"][i]["value"]
            self.key("P2")
            after = self.dump()["quickmenu"]["rows"][i]["value"]
            if before == after:
                # An integer at its maximum only moves the other way
                self.key("P1")
                after = self.dump()["quickmenu"]["rows"][i]["value"]
            if before == after:
                self.fail("quick menu row %s (%s) does not change with P1/P2"
                          % (row["label"], before))
            else:
                changed.append("%s %s->%s" % (row["label"], before, after))

        self.log("  FM popup, values changed: %s" % ", ".join(changed))
        self.check(len(changed) >= 8,
                   "only %d FM rows change a value" % len(changed))

        # P1 lowers a value: SQL is an integer with a minimum of 0
        sql = self.qm_row("SQL")
        self.qm_to(sql)
        before = self.dump()["quickmenu"]["rows"][sql]["value"]
        self.key("P1")
        after = self.dump()["quickmenu"]["rows"][sql]["value"]
        self.check(before != after, "P1 does not lower the squelch")
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

        # The popup of each of the eleven modes, and one editable row of it
        for mode in MODES:
            self.goto_mode(mode)
            q = self.open_qm()
            if q is None:
                self.fail("no quick menu in %s" % mode)
                continue
            self.capture("qm-%s" % mode.lower(), need=[q["title"]])

            for i in range(len(q["rows"])):
                self.qm_to(i)
                if self.dump()["quickmenu"]["selected"] != i:
                    self.fail("%s: quick menu row %d cannot be selected"
                              % (mode, i))

            editable = [i for i, r in enumerate(q["rows"])
                        if r["value"] != "" and r["label"] not in read_only]
            ok = False
            for i in editable:
                self.qm_to(i)
                label = q["rows"][i]["label"]
                before = self.dump()["quickmenu"]["rows"][i]["value"]
                self.key("P2")
                after = self.dump()["quickmenu"]["rows"][i]["value"]
                if before != after:
                    ok = True
                    self.capture("qm-%s-%s-edited" % (mode.lower(),
                                                      label.lower().strip()))
                    break
            if editable and not ok:
                self.fail("%s: no quick menu row changes its value" % mode)

            self.key("RED")
        self.home()

    def goto_mode(self, mode):
        """Reach a mode the way the operator does: the side key and a digit."""
        for _ in range(len(MODES) + 1):
            d = self.dump()
            if d["radio"]["mode"] == mode:
                return
            self.home()
            self.combo("MONI", "5")
        self.fail("cannot reach mode %s" % mode)

    def phase_top_menu(self):
        self.step_name = "top menu"
        self.home()
        self.key("GREEN")
        self.capture("menu-top", screen="MENU_TOP", need=["Menu"])

        for i, label in enumerate(TOP_MENU):
            self.menu_to(i)
            d = self.capture("menu-top-%02d-%s" % (i, label.lower()))
            if label not in " | ".join(l["text"] for l in d["lines"]):
                self.fail("row %s is not drawn in the top menu" % label)

            self.key("GREEN")
            d = self.dump()
            self.capture("menu-top-%02d-%s-open" % (i, label.lower()),
                         allow_blank=(label in ("Banks", "Channels")))
            self.check(d["screen"]["name"] != "MENU_TOP",
                       "green on %s does not open anything" % label)

            self.home()
            self.assert_alive("after leaving %s" % label)
            self.key("GREEN")

        self.key("RED")
        self.check(self.dump()["screen"]["name"] == "MAIN_VFO",
                   "red does not leave the top menu")

    def phase_settings(self):
        self.step_name = "settings"
        self.settings_list()
        self.capture("menu-settings", screen="MENU_SETTINGS", need=["Display"])

        for i, label in enumerate(SETTINGS_PAGES):
            self.menu_to(i, on="MENU_SETTINGS")
            d = self.capture("settings-%02d-%s" % (i, label.lower().replace(" ", "-").replace("&", "and")))
            if label not in " | ".join(l["text"] for l in d["lines"]):
                self.fail("page %s is not in the Settings list" % label)

            self.key("GREEN")
            inside = self.capture("settings-%02d-%s-page"
                                  % (i, label.lower().replace(" ", "-").replace("&", "and")))

            # Walk two rows of every page, and back out
            self.key("DOWN")
            self.key("DOWN")
            self.capture("settings-%02d-%s-page-rows"
                         % (i, label.lower().replace(" ", "-").replace("&", "and")))

            for _ in range(4):
                if self.dump()["screen"]["name"] == "MENU_SETTINGS":
                    break
                self.key("RED")
            if self.dump()["screen"]["name"] != "MENU_SETTINGS":
                self.fail("red does not come back from %s" % label)
                self.home()
                self.top_menu(6)

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
        self.menu_to(3, on="MENU_SETTINGS")
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

        # The page of every mode. DMR, NXDN, dPMR, P25, YSF, DSTAR, AM and WFM
        # draw their own quick menu rows as a page (SETTINGS_MODE); TETRA and
        # FM have a page of their own, written by hand.
        own_page = {"TETRA": "SETTINGS_TETRA", "FM": "SETTINGS_FM"}

        for label in ("DMR", "NXDN", "dPMR", "P25", "YSF", "DSTAR", "AM",
                      "WFM", "TETRA", "FM"):
            if not self.settings_list():
                continue
            self.menu_to(SETTINGS_PAGES.index(label), on="MENU_SETTINGS")
            self.key("GREEN")

            want = own_page.get(label, "SETTINGS_MODE")
            d = self.capture("settings-page-%s" % label.lower(), screen=want)
            q = d["quickmenu"]

            if q is not None:
                # Every row of the page selectable, then one of them edited
                for row in range(len(q["rows"])):
                    self.qm_to(row)
                    if self.dump()["quickmenu"]["selected"] != row:
                        self.fail("%s: row %d of the page cannot be selected"
                                  % (label, row))
                self.capture("settings-page-%s-rows" % label.lower())

                skip = ("SYNC", "CALL", "TYPE", "NAC", "MY", "UR", "RPT",
                        "TXT", "CH", "LVL", "SNR", "LOCK", "MSGS", "MSG")
                for row, item in enumerate(q["rows"]):
                    if item["value"] == "" or item["label"] in skip:
                        continue
                    self.qm_to(row)
                    before = self.dump()["quickmenu"]["rows"][row]["value"]
                    self.key("P2")
                    after = self.dump()["quickmenu"]["rows"][row]["value"]
                    if before != after:
                        self.capture("settings-page-%s-%s-edited"
                                     % (label.lower(), item["label"].lower()))
                        break
                else:
                    if any(r["value"] for r in q["rows"]):
                        self.fail("%s: no row of the page changes a value"
                                  % label)
            else:
                # A hand-written page: walk it and edit its first row
                before = " | ".join(l["text"] for l in d["lines"])
                for _ in range(3):
                    self.key("DOWN")
                self.capture("settings-page-%s-rows" % label.lower())
                self.menu_to(0, on=want)
                self.key("GREEN")
                self.key("DOWN")
                self.key("GREEN")
                after = " | ".join(l["text"] for l in self.dump()["lines"])
                self.capture("settings-page-%s-edited" % label.lower())
                self.check(before != after,
                           "%s: no row of the page changes a value" % label)

            for _ in range(3):
                if self.dump()["screen"]["name"] == "MENU_SETTINGS":
                    break
                self.key("RED")
        self.home()

    def phase_channels(self):
        """The in-radio channel bank: create, edit the frequency, save, tune."""
        self.step_name = "channels"
        self.goto_mode("FM")
        self.home()

        self.key_long("P1")
        d = self.capture("channels-list-empty")
        self.check(d["modules"]["channels"], "P1 long does not open the bank")
        self.check("No channels" in " ".join(l["text"] for l in d["lines"]),
                   "the empty bank does not say so")

        self.key("P1")
        d = self.capture("channels-editor-new", need=["NEW CHANNEL"])
        self.check(d["header"]["title"] == "NEW CHANNEL",
                   "P1 in the list does not open a new channel")

        # RX frequency: green enters the field, UP/DOWN walk the digits
        self.key("DOWN")
        self.key("DOWN")
        rx_before = " ".join(l["text"] for l in d["lines"])
        self.key("GREEN")
        self.key("DOWN")
        self.key("DOWN")
        self.key("DOWN")
        self.key("GREEN")
        d = self.capture("channels-editor-rx")
        rx_text = " ".join(l["text"] for l in d["lines"])
        self.check(rx_before != rx_text,
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

        # Tune it: green on the entry loads it and closes the list. The screen
        # stays MAIN_VFO: tuning from the in-radio bank loads state.channel and
        # state.channel_index but does not switch the display to the memory
        # screen, so the channel's name is never shown on the main screen.
        self.key("GREEN")
        d = self.capture("channels-tuned", screen="MAIN_MEM")
        self.check(d["radio"]["mode"] == "FM", "the tuned channel is not FM")
        self.check(d["radio"]["rx"] > 0, "the tuned channel has no frequency")
        self.check(d["radio"]["name"].startswith("CH"),
                   "the tuned channel did not land in state.channel: %r"
                   % d["radio"]["name"])
        self.check("CH 1" in " ".join(l["text"] for l in d["lines"]),
                   "the memory screen does not show the channel name")

        # And the editor opens the stored record: P2 on the entry
        self.key_long("P1")
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
        self.top_menu(3)
        self.key("GREEN")
        d = self.capture("contacts-empty", need=["CONTACTS"])
        self.check("No contacts" in " ".join(l["text"] for l in d["lines"]),
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

    def phase_messages(self):
        self.step_name = "messages"
        self.home()
        self.top_menu(4)
        self.key("GREEN")
        self.capture("messages-inbox", need=["INBOX"])

        self.key("P1")
        self.capture("messages-sent", need=["SENT"])
        self.key("P1")

        # The composer, from the quick menu's MSG row (a reply needs a message)
        self.home()
        if self.qm_goto("MSG"):
            d = self.capture("messages-compose", screen="MSG_COMPOSE",
                             need=["COMPOSE", "TP", "TO", "TXT", "SEND"])
            for _ in range(3):
                self.key("DOWN")
            self.capture("messages-compose-send")

            # Typing: green on TXT starts the multi-tap composer
            self.key("UP")
            self.key("UP")
            self.key("GREEN")
            self.key("2")
            self.key("2")
            self.key("2")
            d = self.capture("messages-compose-typed")
            self.check("ABC".lower() in " ".join(l["text"] for l in d["lines"]).lower()
                       or "A" in " ".join(l["text"] for l in d["lines"]),
                       "the composer does not take characters")
            self.key("GREEN")
            for _ in range(3):
                self.key("RED")
        self.home()

    def phase_zones(self):
        self.step_name = "zones"
        self.home()
        self.top_menu(2)
        self.key("GREEN")
        d = self.capture("zones-list", need=["ZONES"])
        self.check("R" in " ".join(l["text"] for l in d["lines"]),
                   "the zone list does not show the repeater zone")

        self.key("GREEN")
        d = self.capture("zones-entries", need=["145.60000"])
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
        self.step_name = "aprs"
        self.home()

        # The station list is reached with a long P2 in FM (docs/UI.md). The
        # keypad reports the press and the long press as two events, so the
        # popup opens first and the long press has to take the keypad back.
        self.sim.cmd("hold P2 1500")
        d = self.capture("aprs-stations", screen="MENU_APRS")
        self.check(d["quickmenu"] is None, "the popup stayed open over APRS")
        self.key("RED")

        if self.qm_goto("APRS"):
            d = self.capture("aprs-setup", screen="APRS_CFG", need=["Decode"])
            self.check("APRS" in d["header"]["title"].upper(),
                       "the APRS row does not open the setup page")

            # The fields change with P1/P2 and land in the settings record;
            # the page marks the selected row with a hollow rectangle and
            # yellow text, not with the black-on-white of the other lists.
            before = (d["aprs"]["decode"], d["aprs"]["beacon"], d["aprs"]["ssid"])
            changed = False
            for _ in range(4):
                self.key("DOWN")
                self.key("P2")
                d = self.dump()
                after = (d["aprs"]["decode"], d["aprs"]["beacon"],
                         d["aprs"]["ssid"])
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

    def phase_spectrum(self):
        self.step_name = "spectrum"
        self.home()
        if self.qm_goto("ESPECTRO"):
            d = self.capture("spectrum", screen="SPECTRUM")
            self.check(any("ESPECTRO" in l["text"] for l in d["lines"]),
                       "the analyser is not on the screen")

            self.key("P2")
            q = self.dump()["quickmenu"]
            if q is None:
                self.fail("P2 does not open the analyser's own quick menu")
            else:
                self.capture("spectrum-menu", need=["SPAN"])
                for i in range(len(q["rows"])):
                    self.qm_to(i)
                self.key("RED")
            self.key("RED")
        self.home()
        self.assert_alive("after the analyser")

    def phase_modes(self):
        """Every mode of the registry: the main screen, its popup, and back."""
        self.step_name = "modes"
        self.home()
        seen = []
        for mode in MODES + [MODES[0]]:
            self.goto_mode(mode)
            d = self.capture("mode-%s" % mode.lower())
            self.check(d["radio"]["mode"] == mode,
                       "the side key does not reach %s (got %s)"
                       % (mode, d["radio"]["mode"]))
            seen.append(d["radio"]["mode"])

            # AM: its own main screen, popup and settings page, and out again
            if mode == "AM":
                self.check("AM" in " ".join(l["text"] for l in d["lines"]),
                           "the AM main screen does not label the mode")
                self.key("P2")
                self.capture("mode-am-quickmenu", need=["AGC", "STEP"])
                self.key("RED")
                self.top_menu(6)
                self.key("GREEN")
                self.menu_to(SETTINGS_PAGES.index("AM"), on="MENU_SETTINGS")
                self.key("GREEN")
                self.capture("mode-am-page", screen="SETTINGS_MODE")
                for _ in range(3):
                    self.key("RED")
                self.assert_alive("after the AM page")

        self.check(seen[0] == seen[-1] == "FM",
                   "a full turn of the side key does not come back to FM: %s"
                   % seen)
        self.log("  modes walked: %s" % " -> ".join(seen))

    # ------------------------------------------------------------------ run --

    def run(self):
        started = time.time()

        self.phase_main()
        self.phase_frequency_entry()
        self.phase_modes()
        self.phase_quick_menu()
        self.phase_top_menu()
        self.phase_settings()
        self.phase_channels()
        self.phase_contacts()
        self.phase_messages()
        self.phase_zones()
        self.phase_aprs()
        self.phase_packet()
        self.phase_spectrum()

        self.step_name = "end"
        self.home()
        self.assert_alive("at the end of the tour")
        self.capture("end-main-vfo", screen="MAIN_VFO")

        elapsed = time.time() - started
        screens = sorted({r["screen"] for r in self.records})

        with open(os.path.join(self.outdir, "tour.json"), "w") as f:
            json.dump({"records": self.records, "failures": self.failures,
                       "findings": self.findings, "seconds": elapsed}, f,
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="sim-tour",
                    help="directory for the pictures and the dumps")
    ap.add_argument("--bin", default=None, help="path to openrtx_c62_sim")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

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
