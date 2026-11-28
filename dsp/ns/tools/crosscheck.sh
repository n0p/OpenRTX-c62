#!/bin/sh
# Three-way cross-check of the denoiser's output: x86 (glibc), Cortex-M33
# (newlib) and HiFi4 (Xtensa libm), same sources, same frames.
#
#   tools/crosscheck.sh [nframes]
#
# The audio path is supposed to be independent of the C library: the tables and
# the whole kiss_fft state come from ns_tables.h and log10f is the vendored
# fdlibm one, so the three builds must agree bit for bit.  This is the check
# that says whether they do; it is also how the host was found to drift from
# frame 7 before the tables were pinned.
set -eu
N=${1:-32}
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"
R=../../lib/rnnoise
FD=third_party/newlib_fdlibm
B=build/cross
F="-DNS_DUMP=1 -DNS_ITERS=$N -D_GNU_SOURCE"
IL="-I$R/include -I$R/src -I. -I$HERE"


mkdir -p "$B"
for f in denoise rnn rnn_data pitch celt_lpc kiss_fft; do
    python3 tools/fp_literals.py "$R/src/$f.c" "$B/$f.c" >/dev/null
done

echo "== x86 ($N frames)"
env -i PATH=/usr/bin:/bin HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" \
    cc -O2 -fsingle-precision-constant -fno-math-errno -ffp-contract=off \
    -DRNNOISE_FIXED_TABLES '-Dlog10f(x)=ns_log10f(x)' -DNS_OWN_SBRK=1 \
    $F -DM_PI=3.14159265358979323846 -I$FD \
    -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f \
    $IL "$B"/*.c $FD/ef_log10.c $FD/ef_log.c bench/bench_main.c \
    -o "$B"/host -lm
"$B"/host > "$B"/host.txt

echo "== venus_hifi4 ($N frames)"
"$XTENSA_TOOLS_ROOT"/bin/xt-clang --xtensa-core=venus_hifi4 -O2 -fno-math-errno \
    -ffp-contract=off -w -DRNNOISE_FIXED_TABLES "-Dlog10f(x)=ns_log10f(x)" $F -DM_PI=3.14159265358979323846f $IL \
    $(for f in denoise rnn rnn_data pitch celt_lpc kiss_fft; do echo "$B/$f.c"; done) \
    bench/bench_main.c build/ns_log10f.o build/ns_logf.o -lsim -lm -o "$B"/dsp
"$XTENSA_TOOLS_ROOT"/bin/xt-run "$B"/dsp > "$B"/dsp.txt

echo "== Cortex-M33 ($N frames)"
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2 \
     -fsingle-precision-constant -fno-math-errno -ffp-contract=off"
arm-none-eabi-gcc $ARM -DRNNOISE_FIXED_TABLES "-Dlog10f(x)=ns_log10f(x)" -DNS_RDIMON=1 -DNS_OWN_SBRK=1 $F $IL \
    -c bench/bench_main.c -o "$B"/bench.o
for f in denoise rnn rnn_data pitch celt_lpc kiss_fft; do
    arm-none-eabi-gcc $ARM -DRNNOISE_FIXED_TABLES "-Dlog10f(x)=ns_log10f(x)" $F $IL -c "$R/src/$f.c" -o "$B"/m_$f.o
done
arm-none-eabi-gcc $ARM -c common/startup.c -o "$B"/startup.o
arm-none-eabi-gcc $ARM -T common/mps2.ld -specs=rdimon.specs -o "$B"/m33.elf \
    "$B"/startup.o "$B"/m_*.o "$B"/bench.o build/ns_log10f_arm.o \
    build/ns_logf_arm.o -lrdimon -lm -lc
qemu-system-arm -M mps2-an505 -nographic -semihosting-config enable=on,target=native \
    -kernel "$B"/m33.elf > "$B"/m33.txt 2>&1 || true

for p in "host m33" "host dsp" "m33 dsp"; do
    set -- $p
    echo "== $1 vs $2"
    python3 tools/cmp_frames.py "$B/$1.txt" "$B/$2.txt" | tail -4
done
