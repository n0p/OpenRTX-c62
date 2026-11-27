#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Check that nothing registered as an interrupt/timer/codec callback locks or logs.

Why this exists
---------------
On the radio, a callback given to a kernel timer, a GPIO interrupt or the audio
driver runs in interrupt context.  In that context Zephyr forbids two things the
rest of this codebase does freely: taking a mutex (`!arch_is_in_isr()` in
kernel/mutex.c) and writing a log line with the UART backend in immediate mode.
Either one aborts the kernel, and the radio reboots.

No host test can see that: on Linux a pthread_mutex_lock from a signal handler
is legal, and the simulator has no interrupts at all.  This check is the only
one that can, so it is a test and not a comment: it fails the build when a
callback that runs in interrupt context starts locking or logging.

A function that really is safe to call from a callback can say so in its body
with `isr-safe:` in a comment, which is the exception this looks for.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# How a function is made a callback.  Each pattern captures the function name.
REGISTRATIONS = [
    r"k_timer_init\s*\(\s*&?\w+\s*,\s*(\w+)\s*,",
    r"K_TIMER_DEFINE\s*\(\s*\w+\s*,\s*(\w+)\s*,",
    r"irq_connect_dynamic\s*\([^;]*?,\s*(\w+)\s*,",
    r"gpio_add_callback\s*\([^;]*?,\s*&?\w*(?:cb|callback)\w*",
    r"csk6codec_setRxProcessor\s*\(\s*(\w+)\s*\)",
    r"csk6codec_setTxProcessor\s*\(\s*(\w+)\s*\)",
    r"audioStream_set\w*Callback\s*\(\s*(\w+)\s*\)",
]

# What a callback must not do.
FORBIDDEN = [
    (re.compile(r"\bk_mutex_lock\s*\("), "takes a mutex"),
    (re.compile(r"\bpthread_mutex_lock\s*\("), "takes a mutex"),
    (re.compile(r"\bk_sem_take\s*\([^;]*K_FOREVER"), "waits for ever on a semaphore"),
    (re.compile(r"\bk_sleep\s*\("), "sleeps"),
    (re.compile(r"\bpthread_cond_wait\s*\("), "waits on a condition variable"),
    (re.compile(r"\bLOG_(ERR|WRN|INF|DBG)\s*\("), "logs"),
    (re.compile(r"\bprintk\s*\("), "logs"),
]

SAFE_MARK = "isr-safe:"

SOURCES = list((ROOT / "openrtx").rglob("*.c")) \
        + list((ROOT / "openrtx").rglob("*.cpp")) \
        + list((ROOT / "platform").rglob("*.c")) \
        + list((ROOT / "platform").rglob("*.cpp"))


def function_body(text, name):
    """The body of `name` in `text`, by brace matching, or None."""
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text)
    if match is None:
        return None

    start = text.index("{", match.start())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def main():
    failures = []

    for path in SOURCES:
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue

        names = set()
        for pattern in REGISTRATIONS:
            names.update(re.findall(pattern, text))

        for name in sorted(names):
            body = function_body(text, name)
            if body is None or SAFE_MARK in body:
                continue

            for rule, what in FORBIDDEN:
                if rule.search(body):
                    rel = path.relative_to(ROOT)
                    failures.append("%s: %s() runs in interrupt context and %s"
                                    % (rel, name, what))
                    break

    for failure in failures:
        print(failure)

    if failures:
        print("\n%d callback(s) break the interrupt-context rule.\n"
              "Either move the work to a thread (k_work_submit) or, when the "
              "call really is safe,\nmark the body with `isr-safe: <why>`."
              % len(failures))
        return 1

    print("interrupt-context rules: %d source files, no callback locks or logs"
          % len(SOURCES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
