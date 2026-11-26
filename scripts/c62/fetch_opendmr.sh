#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetches OpenDMR (GPL-2.0-or-later / GPL-3.0-or-later, the AMBE+2 encoder of
# OP25 by KA1RBI and Pavel Yazev's fixed point IMBE analysis) for the
# optional DMR encoder of the C62. As for mbelib, the AMBE+2 algorithm is
# covered by patents in some countries: check that using it is lawful where
# you are before fetching it.
#
# Usage: fetch_opendmr.sh [west workspace]
# The workspace defaults to the folder holding the OpenRTX checkout. The next
# build finds OpenDMR in <workspace>/modules/lib/opendmr (CMake option
# DMR_ENC, AUTO by default).
#
# The script also patches decoder/mbe_compiler.h: it defines MBE_THREAD_LOCAL
# unconditionally (__thread with GCC), and a static thread-local in this image
# is a call to __aeabi_read_tp, which Zephyr's newlib here does not have (no
# TLS in the C62's image).  With the definition guarded, the build passes
# -DMBE_THREAD_LOCAL= and the variables are plain statics, which is what the
# radio wants: one decoder, one thread.
set -e
TOP=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}
DEST=$TOP/modules/lib/opendmr
REPO=https://github.com/MW0MWZ/OpenDMR
COMMIT=d28164b39ba4d91ad5948ff22707937f8944f70f # version tested with OpenRTX

if [ ! -d "$TOP/.west" ]; then
    echo "$TOP is not a west workspace (no .west folder): pass its path" >&2
    exit 1
fi
if [ -d "$DEST/.git" ]; then
    echo "OpenDMR already present in $DEST"
else
    git clone -q "$REPO" "$DEST"
fi
git -C "$DEST" checkout -q "$COMMIT"

H=$DEST/decoder/mbe_compiler.h
if grep -q '^#ifndef MBE_THREAD_LOCAL' "$H"; then
    echo "mbe_compiler.h already patched"
else
    python3 - "$H" <<'EOF'
import sys

path = sys.argv[1]
with open(path) as f:
    text = f.read()

block = """#if defined(_MSC_VER)
#define MBE_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define MBE_THREAD_LOCAL __thread
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define MBE_THREAD_LOCAL _Thread_local
#else
/* Fallback for non-C11 compilers: most support __thread as an extension */
#define MBE_THREAD_LOCAL __thread
#endif
"""

if block not in text:
    sys.exit("mbe_compiler.h does not have the MBE_THREAD_LOCAL block "
             "this script knows how to guard")

text = text.replace(block, "#ifndef MBE_THREAD_LOCAL\n" + block + "#endif\n", 1)

with open(path, "w") as f:
    f.write(text)
EOF
    echo "mbe_compiler.h: MBE_THREAD_LOCAL guarded (no TLS in this image)"
fi

echo "OpenDMR ready in $DEST: the next build has the DMR encoder."
