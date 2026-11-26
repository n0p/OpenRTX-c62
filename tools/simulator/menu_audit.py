#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Walk every menu of the C62 simulator and report, row by row, which key does
something.

    python3 tools/simulator/menu_audit.py --out /tmp/menu-audit

It drives the same control channel as the menu tour (docs/SIMULATOR.md) and
writes `audit.json` (every screen, its rows, their values, and what ENTER, P1
and P2 did to each) plus `audit.md`, the table this pass is built on.  A row
whose three keys change nothing on the screen, in `settings` and in `radio`
is reported as dead.

Every screen is reached with the keys the operator uses, from a clean radio
with a store of its own, so a run is reproducible.
"""

import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simlib import Simulator  # noqa: E402

MODES = ["FM", "M17", "TETRA", "DMR", "NXDN", "dPMR", "P25", "YSF",
         "DSTAR", "AM", "WFM"]

# Pages of Settings that are not a mode's own page. The mode pages (DMR,
# NXDN, ...) are drawn from the same table as the mode's popup and are walked
# right after its popup instead.
FIXED_PAGES = ["Display", "Time & Date", "Radio", "Identity", "Audio", "M17",
               "TETRA", "FM", "Accessibility"]

MODE_PAGE_TITLE = {"FM": "FM", "M17": "M17", "TETRA": "TETRA"}

ROW_TOP = 20
ROW_BOTTOM = 116


def now_iso():
    import datetime
    return datetime.datetime.now().isoformat(timespec="seconds")


class Audit:
    def __init__(self, binary=None):
        self.tmp = tempfile.mkdtemp(prefix="c62-menu-audit-")
        env = {
            "XDG_STATE_HOME": os.path.join(self.tmp, "state"),
            "OPENRTX_SIM_STORE": os.path.join(self.tmp, "store.img"),
        }
        os.makedirs(env["XDG_STATE_HOME"], exist_ok=True)
        self.sim = Simulator(binary=binary, shots=os.path.join(self.tmp, "shots"),
                             env=env)
        self.screens = []
        self.dead = []
        self.findings = []

    # ------------------------------------------------------------- plumbing --
    def d(self):
        return self.sim.dump()

    def key(self, *names):
        self.sim.key(*names)

    def down(self, n):
        """n presses of DOWN in one command (each still waits for the UI)."""
        if n > 0:
            self.sim.cmd("keys " + " ".join(["DOWN"] * n))

    def key_long(self, name):
        self.sim.key_long(name)

    @staticmethod
    def sig(d):
        """What a key press can change, in one comparable string."""
        return json.dumps({
            "screen": d["screen"]["name"],
            "qm": d["quickmenu"],
            "lines": [[l["y"], l["x"], l["text"], l["selected"]]
                      for l in d["lines"]],
            "radio": {k: d["radio"][k] for k in
                      ("mode", "rx", "tx", "power", "squelch", "volume",
                       "tx_disable", "ptt", "name", "channel_index",
                       "bandwidth", "bank_count", "contact_count")},
            "settings": d["settings"],
            "modules": d["modules"],
            "tools": d["tools"],
        }, sort_keys=True)

    def unlock(self):
        if self.d()["input"]["locked"]:
            self.key_long("RED")

    def at_home(self):
        d = self.d()
        # A mode page stays "loaded" after its MSGS/MSG row leaves it
        # (qm_page_state.qm is kept on purpose, see qm_page_enter), so a page
        # in the dump is not a popup on top of the main screen.
        qm = d["quickmenu"]
        popup = (qm is not None) and (qm.get("kind") != "page")
        return (d["screen"]["name"] in ("MAIN_VFO", "MAIN_MEM")
                and not any(d["modules"].values())
                and not popup and not d["tools"]["open"])

    def home(self):
        for _ in range(16):
            self.unlock()
            if self.at_home():
                return self.d()
            self.key("RED")
        d = self.d()
        raise RuntimeError("stuck on %s (modules %s, qm %s/%s, tools %s, lock %s)"
                           % (d["screen"]["name"], d["modules"],
                              d["quickmenu"] is not None,
                              (d["quickmenu"] or {}).get("title"),
                              d["tools"]["open"], d["input"]["locked"]))

    def mode(self, name):
        """Main screen in an operating mode, through the tools menu."""
        self.home()
        if self.d()["radio"]["mode"] == name:
            return self.d()
        self.key_long("P1")
        self.tools_to("Mode")
        self.key("GREEN")
        self.tools_to(name)
        self.key("GREEN")
        self.home()
        if self.d()["radio"]["mode"] != name:
            raise RuntimeError("mode %s did not apply (%s)"
                               % (name, self.d()["radio"]["mode"]))
        return self.d()

    def tools_to(self, label):
        for _ in range(14):
            t = self.d()["tools"]
            if not t["open"]:
                raise RuntimeError("tools menu closed")
            if t["rows"][t["selected"]]["label"] == label:
                return
            self.key("DOWN")
        raise RuntimeError("no %r row in the tools menu" % label)

    def open_menu(self):
        self.home()
        self.key("GREEN")
        if self.d()["screen"]["name"] != "MENU_TOP":
            raise RuntimeError("green does not open the top menu")

    def menu_to(self, index, on=None):
        for _ in range(30):
            d = self.d()
            if on and d["screen"]["name"] != on:
                raise RuntimeError("left %s" % on)
            cur = d["input"]["menu"]
            if cur == index:
                return
            n = abs(index - cur)
            if index > cur:
                self.down(n)
            else:
                self.key(*["UP"] * n)
        raise RuntimeError("cursor does not reach %d" % index)

    def open_settings_page(self, label):
        self.home()
        self.open_menu()
        self.menu_to(6)
        self.key("GREEN")
        self.menu_to(0, on="MENU_SETTINGS")
        for _ in range(24):
            d = self.d()
            if d["input"]["menu"] >= 0:
                pass
            idx = self.page_cursor_label()
            if idx == label:
                self.key("GREEN")
                return self.d()
            self.key("DOWN")
        raise RuntimeError("no %r page in Settings" % label)

    def page_cursor_label(self):
        """Label under the cursor on a Settings list or page."""
        d = self.d()
        sel = d["selection"]
        if not sel["present"]:
            return None
        for l in d["lines"]:
            if l["x"] == 2 and l["font"] == 1 and \
                    sel["top"] <= l["y"] <= sel["top"] + sel["height"]:
                return l["text"].lstrip("* ")
        return None

    @staticmethod
    def list_rows(d):
        rows = []
        for l in d["lines"]:
            if l["x"] == 2 and ROW_TOP <= l["y"] <= ROW_BOTTOM and l["font"] == 1:
                value = ""
                for v in d["lines"]:
                    if v["y"] == l["y"] and v["x"] >= 100 and v is not l:
                        value = v["text"]
                rows.append({"label": l["text"].lstrip("* "), "value": value,
                             "y": l["y"], "selected": l["selected"]})
        return rows

    def qm(self):
        return self.d()["quickmenu"]

    def open_qm(self):
        self.home()
        self.key("P2")
        if self.qm() is None:
            raise RuntimeError("P2 does not open the quick menu")
        return self.qm()

    def qm_to(self, index):
        for _ in range(30):
            q = self.qm()
            if q is None:
                raise RuntimeError("the quick menu closed")
            if q["selected"] == index:
                return
            if index > q["selected"]:
                self.down(index - q["selected"])
            else:
                self.key(*["UP"] * (q["selected"] - index))
        raise RuntimeError("quick menu cursor does not reach %d" % index)

    def qm_at(self, index):
        self.open_qm()
        self.qm_to(index)
        return self.qm()

    # --------------------------------------------------------------- probes --
    def probe_qm_row(self, mode, index, label):
        out = {"label": label}
        q0 = self.qm_at(index)
        out["value"] = q0["rows"][index]["value"]
        out["type"] = q0["rows"][index].get("type", "?")

        # One popup, three keys in order: P2, P1, GREEN last because a GOTO row
        # leaves the popup and the two before it need it open.
        for key, name in (("P2", "p2"), ("P1", "p1"), ("GREEN", "enter")):
            before = self.d()
            value = out["value"]
            if before["quickmenu"] is not None and \
                    before["quickmenu"]["selected"] < len(before["quickmenu"]["rows"]):
                value = before["quickmenu"]["rows"][before["quickmenu"]["selected"]]["value"]
            self.key(key)
            after = self.d()
            changed = []
            if after["screen"]["name"] != before["screen"]["name"]:
                changed.append("opens %s" % after["screen"]["name"])
            qa = after["quickmenu"]
            if qa is not None and index < len(qa["rows"]):
                va = qa["rows"][index]["value"]
                if va != value:
                    changed.append("%s -> %s" % (value, va))
            if after["settings"] != before["settings"]:
                changed.append("settings")
            if (after["radio"]["rx"], after["radio"]["tx"]) != \
                    (before["radio"]["rx"], before["radio"]["tx"]):
                changed.append("retune")
            if after["input"]["locked"] != before["input"]["locked"]:
                changed.append("lock %s" % after["input"]["locked"])
            if not changed and self.sig(before) != self.sig(after):
                pass                 # a redraw is not an effect on a value
            out[name] = changed or ["no change"]

            if after["screen"]["name"] != before["screen"]["name"]:
                return out           # GREEN opened a screen: nothing left to probe
        self.home()
        return out

    def probe_page(self, page_label):
        """Rows of a Settings page, and what green / P1 / P2 do to each."""
        self.open_settings_page(page_label)
        d = self.d()
        rows = self.list_rows(d)
        screen = d["screen"]["name"]
        result = {"screen": screen, "title": page_label, "rows": [],
                  "probed": "rows"}

        for i, row in enumerate(rows):
            # Every row starts from the page, cursor on it, nothing open: a
            # probe that leaves an edit mode behind would make the next row's
            # keys mean something else.
            self.leave_to("MENU_SETTINGS")
            self.open_settings_page(page_label)
            if self.d()["screen"]["name"] != screen:
                raise RuntimeError("%s does not reopen" % page_label)
            if self.d()["input"]["menu"] != i:
                self.menu_to(i, on=screen)

            item = {"label": row["label"], "value": row["value"]}
            b = self.d()
            self.key("GREEN")
            a = self.d()

            if a["screen"]["name"] != screen:
                item["enter"] = ["opens %s" % a["screen"]["name"]]
                item["p1"] = item["p2"] = ["no change"]
                item["down"] = item["up"] = ["no change"]
            else:
                ent = []
                if a["input"].get("edit"):
                    ent.append("edit")
                v_after = self.row_value(a, row["label"])
                if v_after and v_after != row["value"]:
                    ent.append("%s -> %s" % (row["value"], v_after))
                if a["settings"] != b["settings"] and not ent:
                    ent.append("settings")
                item["enter"] = ent or ["no change"]

                for key, name in (("P2", "p2"), ("P1", "p1"),
                                  ("DOWN", "down"), ("UP", "up")):
                    b2 = self.d()
                    self.key(key)
                    a2 = self.d()
                    ch = []
                    if a2["screen"]["name"] != b2["screen"]["name"]:
                        ch.append("opens %s" % a2["screen"]["name"])
                    v1 = self.row_value(b2, row["label"])
                    v2 = self.row_value(a2, row["label"])
                    if v2 and v1 and v2 != v1:
                        ch.append("%s -> %s" % (v1, v2))
                    if a2["settings"] != b2["settings"]:
                        ch.append("settings")
                    if (a2["radio"]["rx"], a2["radio"]["tx"]) != \
                            (b2["radio"]["rx"], b2["radio"]["tx"]):
                        ch.append("retune")
                    if a2["input"]["edit"] != b2["input"]["edit"] and not ch:
                        ch.append("edit %s" % a2["input"]["edit"])
                    item[name] = ch or ["no change"]

            result["rows"].append(item)

        self.leave_to("MENU_SETTINGS")
        return result

    def leave_to(self, screen):
        """Red until `screen` (or a main screen, where a caller reopens)."""
        for _ in range(6):
            now = self.d()["screen"]["name"]
            if now == screen or now in ("MAIN_VFO", "MAIN_MEM"):
                return
            self.unlock()
            self.key("RED")
        now = self.d()["screen"]["name"]
        if now != screen:
            raise RuntimeError("cannot get back to %s (on %s)" % (screen, now))


    @staticmethod
    def row_value(d, label):
        for l in d["lines"]:
            if l["x"] == 2 and l["font"] == 1 and l["text"].lstrip("* ") == label:
                for v in d["lines"]:
                    if v["y"] == l["y"] and v["x"] >= 100:
                        return v["text"]
        return None

    # ---------------------------------------------------------------- walks --
    def walk(self, out=None):
        self.home()
        for mode in MODES:
            self.mode(mode)
            self.walk_popup(mode)
            self.walk_mode_page(mode)
            if out:
                self.save(out)
        for page in FIXED_PAGES:
            self.screens.append(self.probe_page(page))
            if out:
                self.save(out)
        self.walk_tools()
        if out:
            self.save(out)
        self.walk_top_menu()
        if out:
            self.save(out)
        self.mode("FM")
        self.walk_screens()
        if out:
            self.save(out)

    def walk_popup(self, mode):
        q = self.open_qm()
        rows = list(q["rows"])
        screen = {"screen": "POPUP", "title": "quick menu, mode %s" % mode,
                  "rows": [], "probed": "rows"}
        for i, row in enumerate(rows):
            screen["rows"].append(self.probe_qm_row(mode, i, row["label"]))
        # values as the popup opens, before anything is touched
        self.open_qm()
        screen["values_open"] = [dict(r) for r in self.qm()["rows"]]
        self.home()
        self.screens.append(screen)

    def walk_mode_page(self, mode):
        label = MODE_PAGE_TITLE.get(mode, mode)
        try:
            self.open_settings_page(label)
        except RuntimeError:
            self.findings.append("mode %s has no Settings page" % mode)
            return
        d = self.d()
        if d["quickmenu"] is None:
            # FM, M17 and TETRA have a hand-written page: walk it like any
            # other Settings page.
            for _ in range(4):
                if self.d()["screen"]["name"] == "MENU_SETTINGS":
                    break
                self.key("RED")
            result = self.probe_page(label)
            result["screen"] = "MODE_PAGE"
            self.screens.append(result)
            return
        q = d["quickmenu"]
        names = [r["label"] for r in q["rows"]]
        screen = {"screen": "MODE_PAGE", "title": "%s (Settings)" % label,
                  "rows": [], "probed": "rows"}
        for i, name in enumerate(names):
            # Fresh page per row: probing LOCK turns the keypad off, and the
            # next row would be read through a deaf one.
            self.leave_to("MENU_SETTINGS")
            self.open_settings_page(label)
            self.unlock()
            for _ in range(24):
                if self.d()["quickmenu"]["selected"] == i:
                    break
                self.key("DOWN")
            row = self.d()["quickmenu"]["rows"][i]
            val = row["value"]
            item = {"label": name, "value": val, "type": row.get("type", "?")}

            if name == "LOCK":
                # The row is the keypad lock: the first key that flips it
                # makes the rest of the probe deaf, so it is measured once and
                # the lock is released with the red held down, as an operator
                # would.
                b = self.d()
                self.key("P2")
                a = self.d()
                item["p2"] = ["lock %s" % a["input"]["locked"]] \
                    if a["input"]["locked"] != b["input"]["locked"] else ["no change"]
                item["enter"] = ["toggles the keypad lock"]
                item["p1"] = ["toggles the keypad lock"]
                self.unlock()
                screen["rows"].append(item)
                continue

            # GREEN last: it is the only key that can leave the page.
            for key, kname in (("P2", "p2"), ("P1", "p1"), ("GREEN", "enter")):
                b = self.d()
                self.key(key)
                a = self.d()
                ch = []
                qa = a["quickmenu"]
                if a["screen"]["name"] != b["screen"]["name"]:
                    ch.append("opens %s" % a["screen"]["name"])
                elif qa is not None and qa["selected"] < len(qa["rows"]):
                    va = qa["rows"][qa["selected"]]["value"]
                    if va != b["quickmenu"]["rows"][qa["selected"]]["value"]:
                        ch.append("%s -> %s" % (
                            b["quickmenu"]["rows"][qa["selected"]]["value"], va))
                if a["settings"] != b["settings"]:
                    ch.append("settings")
                if a["input"]["locked"] != b["input"]["locked"]:
                    ch.append("lock %s" % a["input"]["locked"])
                item[kname] = ch or ["no change"]
                if a["screen"]["name"] != b["screen"]["name"]:
                    break
                self.unlock()
                for _ in range(24):
                    if self.d()["quickmenu"] is not None and \
                            self.d()["quickmenu"]["selected"] == i:
                        break
                    self.key("DOWN")
            screen["rows"].append(item)
        self.leave_to("MENU_SETTINGS")
        self.screens.append(screen)

    def walk_tools(self):
        self.mode("FM")
        self.home()
        self.key_long("P1")
        d = self.d()
        rows = list(d["tools"]["rows"])
        screen = {"screen": "TOOLS_MENU", "title": "tools (P1 long)", "rows": [],
                  "probed": "rows"}
        for i, row in enumerate(rows):
            self.tools_to(row["label"])
            item = {"label": row["label"], "value": row["value"]}
            b = self.d()
            self.key("P2")
            a = self.d()
            item["p2"] = (["cursor %d" % a["tools"]["selected"]]
                          if a["tools"]["selected"] != b["tools"]["selected"]
                          else ["no change"])
            self.key("P1")
            a = self.d()
            item["p1"] = (["cursor %d" % a["tools"]["selected"]]
                          if a["tools"]["selected"] != b["tools"]["selected"]
                          else ["no change"])
            # green, from this row
            self.tools_to(row["label"])
            b = self.d()
            self.key("GREEN")
            a = self.d()
            ch = []
            if a["screen"]["name"] != b["screen"]["name"]:
                ch.append("opens %s" % a["screen"]["name"])
            if bool(a["tools"].get("modes")) != bool(b["tools"].get("modes")):
                ch.append("mode submenu")
            if a["tools"].get("open") and b["tools"].get("open"):
                ra = [r["value"] for r in a["tools"]["rows"]]
                rb = [r["value"] for r in b["tools"]["rows"]]
                if ra != rb:
                    ch.append("%s -> %s" % (rb[b["tools"]["selected"]],
                                            ra[a["tools"]["selected"]]))
            if a["settings"] != b["settings"]:
                ch.append("settings")
            if a["modules"] != b["modules"]:
                ch.append("modules %s" % {k: v for k, v in
                                          a["modules"].items() if v})
            item["enter"] = ch or ["no change"]
            screen["rows"].append(item)
            self.home()
            self.key_long("P1")

        # the mode submenu, row by row: green applies and leaves
        self.tools_to("Mode")
        self.key("GREEN")
        d = self.d()
        sub = {"screen": "TOOLS_MODES", "title": "tools > Mode", "rows": [],
               "probed": "rows"}
        for row in d["tools"]["rows"]:
            item = {"label": row["label"], "value": row["value"]}
            self.tools_to(row["label"])
            b = self.d()
            self.key("GREEN")
            a = self.d()
            ch = []
            if a["screen"]["name"] != b["screen"]["name"]:
                ch.append("opens %s" % a["screen"]["name"])
            if a["radio"]["mode"] != b["radio"]["mode"]:
                ch.append("mode %s -> %s" % (b["radio"]["mode"],
                                             a["radio"]["mode"]))
            item["enter"] = ch or ["no change"]
            sub["rows"].append(item)
            self.home()
            self.key_long("P1")
            self.tools_to("Mode")
            self.key("GREEN")
        self.key("RED")
        self.home()
        self.screens.append(screen)
        self.screens.append(sub)

    # ------------------------------------------------- data and tool screens --
    def menu_row(self, label):
        """Home -> Menu -> the row with this label -> green."""
        self.home()
        self.open_menu()
        for _ in range(14):
            lab = self.page_cursor_label()
            # "Messages !2" carries the unread badge (ui_menu.c), so the row
            # is matched by its name and not by the whole string.
            if lab == label or (lab is not None and
                                lab.startswith(label + " ")):
                self.key("GREEN")
                return
            self.key("DOWN")
        raise RuntimeError("no %r row in the top menu" % label)

    def popup_row(self, label):
        """Home -> P2 -> the popup row with this label -> green."""
        q = self.open_qm()
        for i, r in enumerate(q["rows"]):
            if r["label"] == label:
                self.qm_to(i)
                self.key("GREEN")
                return
        raise RuntimeError("no %r row in the %s popup" % (label, q["title"]))

    def module_screen(self, title, reach, keys=("GREEN", "P1", "P2")):
        """A screen whose rows are data: its rows, its hints, what a key does."""
        self.home()
        reach()
        d = self.d()
        rows = self.list_rows(d)
        hints = [l["text"] for l in d["lines"] if l["y"] >= 116]
        out = {"screen": d["screen"]["name"], "title": title,
               "rows": [{"label": r["label"], "value": r["value"]}
                        for r in rows],
               "hints": " ".join(hints)}
        for key in keys:
            self.home()
            try:
                reach()
            except RuntimeError:
                out[key.lower()] = ["cannot re-enter"]
                continue
            b = self.d()
            self.key(key)
            a = self.d()
            ch = []
            if a["screen"]["name"] != b["screen"]["name"]:
                ch.append("opens %s" % a["screen"]["name"])
            if a["modules"] != b["modules"]:
                ch.append("modules %s" % {k: v for k, v in
                                          a["modules"].items() if v})
            ra, rb = self.list_rows(a), self.list_rows(b)
            if [(r["label"], r["value"]) for r in ra] != \
                    [(r["label"], r["value"]) for r in rb]:
                ch.append("rows changed")
            if a["settings"] != b["settings"]:
                ch.append("settings")
            if (a["radio"]["channel_index"], a["radio"]["name"]) != \
                    (b["radio"]["channel_index"], b["radio"]["name"]):
                ch.append("tunes %s" % a["radio"]["name"])
            out[{"GREEN": "enter", "P1": "p1", "P2": "p2"}[key]] = \
                ch or ["no change"]
        self.home()
        self.screens.append(out)

    def walk_screens(self):
        # One message in the shared store, so the inbox, its view and the
        # composer exist (docs/SIMULATOR.md, "inject").
        self.sim.cmd("inject message 1234 hola desde el aire")

        self.mode("FM")
        self.module_screen("Info (menu)", lambda: self.menu_row("Info"))
        self.module_screen("About (menu)", lambda: self.menu_row("About"))
        self.module_screen("Banks (menu, CPS)", lambda: self.menu_row("Banks"))
        self.module_screen("Channels (menu, CPS)",
                           lambda: self.menu_row("Channels"))
        self.module_screen("Zones (menu)", lambda: self.menu_row("Zones"))
        self.module_screen("Contacts (menu)", lambda: self.menu_row("Contacts"))
        self.module_screen("Contacts editor",
                           lambda: (self.menu_row("Contacts"), self.key("P1")))
        self.module_screen("Messages inbox",
                           lambda: self.menu_row("Messages"))
        self.module_screen("Messages sent",
                           lambda: (self.menu_row("Messages"), self.key("P1")))
        self.module_screen("Message view",
                           lambda: (self.menu_row("Messages"),
                                    self.key("GREEN")))
        self.module_screen("Composer",
                           lambda: (self.menu_row("Messages"),
                                    self.key("P2")))

        # the in-radio channel bank, through the tools menu
        self.mode("FM")
        self.home()
        self.key_long("P1")
        self.tools_to("Channels")
        self.key("GREEN")
        self.module_screen("Channels (in-radio bank)",
                           lambda: (self.home(), self.key_long("P1"),
                                    self.tools_to("Channels"), self.key("GREEN")))
        self.module_screen("Channel editor",
                           lambda: (self.home(), self.key_long("P1"),
                                    self.tools_to("Channels"), self.key("GREEN"),
                                    self.key("P1")))

        # APRS, long P2 in FM
        self.mode("FM")
        self.home()
        self.key_long("P2")
        self.module_screen("APRS stations", lambda: (self.home(),
                                                     self.key_long("P2")))
        self.module_screen("APRS inbox",
                           lambda: (self.home(), self.key_long("P2"),
                                    self.key("P1")))
        self.module_screen("APRS setup",
                           lambda: (self.home(), self.key_long("P2"),
                                    self.key("P2")))

        # packet modem, from the FM popup
        self.module_screen("Packet setup", lambda: self.popup_row("PKT"))
        self.module_screen("Packet counters",
                           lambda: (self.home(), self.popup_row("PKT"),
                                    self.packet_last()))
        self.module_screen("Spectrum", lambda: self.popup_row("ESPECTRO"))

        self.mode("FM")
        self.home()

    def packet_last(self):
        """The PKT row that opens the counters: the last one."""
        for _ in range(8):
            if self.page_cursor_label_is(">"):
                self.key("GREEN")
                return
            self.key("DOWN")

    def page_cursor_label_is(self, want):
        d = self.d()
        sel = d["selection"]
        for l in d["lines"]:
            if sel["present"] and sel["top"] <= l["y"] <= sel["top"] + sel["height"]:
                if l["text"].strip() == want:
                    return True
        return False

    def walk_top_menu(self):
        self.open_menu()
        d = self.d()
        rows = self.list_rows(d)
        screen = {"screen": "MENU_TOP", "title": "top menu", "rows": [],
                  "probed": "rows"}
        for i, row in enumerate(rows):
            self.open_menu()
            self.menu_to(i)
            item = {"label": row["label"], "value": row["value"]}
            b = self.d()
            self.key("GREEN")
            a = self.d()
            ch = []
            if a["screen"]["name"] != b["screen"]["name"]:
                ch.append("opens %s" % a["screen"]["name"])
            if a["modules"] != b["modules"]:
                ch.append("modules %s" % {k: v for k, v in
                                          a["modules"].items() if v})
            item["enter"] = ch or ["no change"]
            screen["rows"].append(item)
            self.home()
        # the rest of the list, below the visible window
        self.open_menu()
        for _ in range(24):
            cur = self.d()["input"]["menu"]
            self.key("DOWN")
            if self.d()["input"]["menu"] == cur == 9:
                break
            lab = self.page_cursor_label()
            if lab and all(r["label"] != lab for r in screen["rows"]):
                b = self.d()
                self.key("GREEN")
                a = self.d()
                ch = ["opens %s" % a["screen"]["name"]] \
                    if a["screen"]["name"] != b["screen"]["name"] else ["no change"]
                screen["rows"].append({"label": lab, "value": "", "enter": ch})
                self.home()
                self.open_menu()
        self.home()
        self.screens.append(screen)

    # ---------------------------------------------------------------- output --
    def report(self):
        self.dead = []
        self.status = []
        for s in self.screens:
            if s.get("probed") != "rows":
                for r in s["rows"]:
                    r["actuated"] = True     # measured at screen level
                continue
            for r in s["rows"]:
                did = []
                for k in ("enter", "p1", "p2", "down", "up"):
                    if k in r and any(v != "no change" for v in r[k]):
                        did.append(k)
                r["actuated"] = bool(did)
                if not did and r.get("type") == "info":
                    # Read-only by design in a popup: the receiver's state.
                    # In a Settings page it would be a dead row, and the page
                    # must not have any (qm_page_open drops them).
                    self.status.append("%s / %s" % (s["title"], r["label"]))
                    if s["screen"] not in ("POPUP",):
                        self.dead.append("%s / %s (read-only row in a page)"
                                         % (s["title"], r["label"]))
                    continue
                if not did:
                    self.dead.append("%s / %s (%s)" % (s["title"], r["label"],
                                                      r.get("value", "")))
        # The accessory port is one row of the whole UI: the tools menu's
        # (docs/ACCESSORY.md). Counted over every screen this walk opened.
        ports = [(s["title"], r["label"]) for s in self.screens for r in s["rows"]
                 if r["label"] == "Puerto"]
        walked_tools = any(s["screen"] == "TOOLS_MENU" for s in self.screens)
        if walked_tools and (len(ports) != 1):
            self.findings.append("the accessory port row appears %d times: %s"
                                 % (len(ports), ports))

        return {"when": now_iso(), "screens": self.screens,
                "dead": self.dead, "status": self.status,
                "port_rows": ports, "findings": self.findings}

    def save(self, out):
        data = self.report()
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "audit.json"), "w") as f:
            json.dump(data, f, indent=1, ensure_ascii=False)
        with open(os.path.join(out, "audit.md"), "w") as f:
            f.write(to_markdown(data))
        return data


def to_markdown(data):
    out = ["# Menu audit", "",
           "Generated by `tools/simulator/menu_audit.py` on %s." % data["when"],
           "",
           "`enter` is GREEN, `p1`/`p2` are F1/F2. A dash means the key changed",
           "nothing on the screen, in `settings` or in `radio`.", ""]
    for s in data["screens"]:
        out.append("## %s" % s["title"])
        out.append("")
        out.append("Screen `%s`." % s["screen"])
        out.append("")
        if s.get("hints"):
            out.append("Hint bar: `%s`." % s["hints"])
            out.append("")
        if s.get("probed") != "rows":
            for k, name in (("enter", "ENTER"), ("p1", "P1"), ("p2", "P2")):
                if k in s:
                    v = [x for x in s[k] if x != "no change"]
                    out.append("- **%s on the selected row:** %s"
                               % (name, ", ".join(v) if v else "nothing"))
            out.append("")
            out.append("| Row | Value |")
            out.append("|---|---|")
            for r in s["rows"]:
                out.append("| `%s` | %s |" % (r["label"], r.get("value", "")))
            out.append("")
            continue
        out.append("| Row | Kind | Value | ENTER | P1 | P2 |")
        out.append("|---|---|---|---|---|---|")
        for r in s["rows"]:
            def fmt(k):
                v = r.get(k)
                if v is None:
                    return "—"
                v = [x for x in v if x != "no change"]
                if v:
                    return ", ".join(v)
                if r.get("type") == "info":
                    return "status"
                return "—" if r.get("actuated") else "**dead**"
            out.append("| `%s` | %s | %s | %s | %s | %s |"
                       % (r["label"], r.get("type", ""), r.get("value", ""),
                          fmt("enter"), fmt("p1"), fmt("p2")))
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="menu-audit")
    ap.add_argument("--binary", default=None)
    ap.add_argument("--only", default=None,
                    help="comma separated: popup,page,fixed,tools,top")
    ap.add_argument("--modes", default=None,
                    help="comma separated subset of the eleven modes")
    args = ap.parse_args()

    modes = args.modes.split(",") if args.modes else MODES

    a = Audit(args.binary)
    try:
        a.home()
        if args.only:
            what = args.only.split(",")
            if "popup" in what or "page" in what:
                for m in modes:
                    print("mode", m, flush=True)
                    a.mode(m)
                    if "popup" in what:
                        a.walk_popup(m)
                    if "page" in what:
                        a.walk_mode_page(m)
                    a.save(args.out)        # keep what is measured so far
            if "fixed" in what:
                for p in FIXED_PAGES:
                    print("page", p, flush=True)
                    a.screens.append(a.probe_page(p))
                    a.save(args.out)
            if "tools" in what:
                a.walk_tools()
                a.save(args.out)
            if "top" in what:
                a.walk_top_menu()
                a.save(args.out)
            if "screens" in what:
                a.walk_screens()
                a.save(args.out)
        else:
            a.walk(args.out)
    finally:
        a.sim.quit()

    data = a.save(args.out)
    print("screens: %d, rows: %d, dead: %d"
          % (len(data["screens"]), sum(len(s["rows"]) for s in data["screens"]),
             len(data["dead"])))
    for d in data["dead"]:
        print("DEAD", d)
    print("port rows:", data["port_rows"])
    for f in data["findings"]:
        print("NOTE", f)


if __name__ == "__main__":
    main()
