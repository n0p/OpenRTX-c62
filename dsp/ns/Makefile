# dsp/ns — RNNoise speech enhancement on the CSK6011B HiFi4 (venus_hifi4)
#
#   make dsp-dump    venus_hifi4 build that prints every output frame as hex
#   make m33-dump    Cortex-M33 build (the firmware's flags) that does the same
#   make compare     frame-by-frame comparison of the two dumps
#   make cost        per-10 ms-frame cost on the ISS: instructions and cycles
#   make m33-cost    the same measurement on the Cortex-M33 (QEMU)
#   make tables      dump and diff the init-time tables of both builds
#   make trace       stage-by-stage trace of one frame, to find a divergence
#   make host        x86 build of the harness, to sanity-check the harness
#
# The DSP toolchain is the ListenAI/Cadence bundle installed outside this tree;
# `source ~/xtensa/env.sh` first.  Nothing here needs the docker image.
#
# Numerics.  The two builds must agree sample for sample, which constrains how
# each one is compiled:
#
#   * both use the firmware's own flags for lib/rnnoise (-O2
#     -fsingle-precision-constant -fno-math-errno, lib/rnnoise/CMakeLists.txt)
#     plus an explicit -ffp-contract=off, so neither side fuses a multiply-add
#     of its own choosing (VFMA on the M33, MADD.S on the HiFi4);
#   * the Xtensa clang in the bundle ignores -fsingle-precision-constant, so the
#     DSP compiles a copy of the sources whose literals are already float
#     (tools/fp_literals.py).  Without it every `x*1.3` is a soft-double call:
#     101 of them per frame;
#   * the window/DCT/twiddle tables are the M33's (ns_tables.h), because the two
#     libms disagree in the last bit on a third of the entries;
#   * log10f is newlib's fdlibm implementation compiled for the HiFi4 (that is
#     the code the M33 runs); the Xtensa libm one is not correctly rounded.

RN      := ../../lib/rnnoise
RNSRC   := $(RN)/src/denoise.c $(RN)/src/rnn.c $(RN)/src/rnn_data.c \
           $(RN)/src/pitch.c $(RN)/src/celt_lpc.c $(RN)/src/kiss_fft.c
NSINC   := -I$(RN)/include -I$(RN)/src -I. -I$(CURDIR)
DEFS    := -D_GNU_SOURCE -DM_PI=3.14159265358979323846
# LIBFIX=0 builds lib/rnnoise as published, with its own libm: useful to measure
# what the tables and log10f cost in bit-exactness terms, and to prove that
# taking them from ns_tables.h does not change the M33 build's output.
LIBFIX  ?= 1
ifeq ($(LIBFIX),1)
LIBFLAGS := -DRNNOISE_FIXED_TABLES '-Dlog10f(x)=ns_log10f(x)'
endif
NUM     := -fsingle-precision-constant -fno-math-errno
FPCONTRACT ?= off
NUM     += -ffp-contract=$(FPCONTRACT)
B       := build

# Cost budget for one 10 ms frame, checked by `make cost-check` (and by `make
# cost`, which runs it):
#
#     instructions <= 700 000   (measured 671 441, 4.1 % of headroom)
#     cycles       <= 1 000 000 (measured 933 066, 6.7 % of headroom)
#
# The headroom is small on purpose: these are the numbers that decide whether
# the stage fits, and a threshold 20 % above the measurement would never fire.
# 700 000 instructions is 23.3 % of a 300 MHz core and 1 000 000 cycles is
# 33.3 %, i.e. the check fails a change that pushes the stage past the "it fits
# with room to spare" line even if it does not fail the 25 % criterion outright.
# Override for an experiment: make cost-check COST_MAX_INSN=... COST_MAX_CYCLES=...
COST_MAX_INSN   ?= 700000
COST_MAX_CYCLES ?= 1000000

# ---------------------------------------------------------------- HiFi4
XT      ?= $(HOME)/xtensa
TOOLS   := $(XT)/RI-2021.7-linux/XtensaTools
CORE    := $(XT)/RI-2021.7-linux/venus_hifi4
XTCLANG := $(TOOLS)/bin/xt-clang
XTRUN   := $(TOOLS)/bin/xt-run
XTGPROF := $(TOOLS)/bin/xt-gprof
FDLIBM  := third_party/newlib_fdlibm
NSLOG   := -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f
# SIMD=1 selects the 2-lane HiFi4 matrix-vector kernel (dsp/ns/rnn_dsp.c).  It
# is bit-exact but, with int8 weights, slower than the scalar code: see README.
SIMD    ?= 0
ifeq ($(SIMD),1)
V       := -simd
SIMDFLAG := -DRNNOISE_DSP_KERNELS
endif
XTRA    ?= $(LIBFLAGS) $(SIMDFLAG)
XTF     := --xtensa-core=venus_hifi4 -O2 -fno-math-errno \
           -ffp-contract=$(FPCONTRACT) -g -w $(XTRA)
XTDEFS  := -D_GNU_SOURCE -DM_PI=3.14159265358979323846f
export XTENSA_CORE   = venus_hifi4
export XTENSA_SYSTEM = $(CORE)/config

DSP_SRC := $(patsubst $(RN)/src/%.c,$(B)/src_f32/%.c,$(RNSRC))

# ---------------------------------------------------------------- Cortex-M33
ARMCC   := arm-none-eabi-gcc
# -ffp-contract=off is what makes this build the reference: with contraction
# left on (GCC's default, so the shipped firmware's), the M33 fuses
# multiplications and additions into VFMA where it likes and the port cannot
# follow.  GCC's -O2 vectoriser, on the other hand, changes nothing here:
# measured bit-exact against the port with and without -fno-tree-vectorize.
# See README, "what bit-exact is measured against".
ARMFLAGS := -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2 \
            $(NUM) $(LIBFLAGS) -fno-strict-aliasing -g
QEMU_ARM := qemu-system-arm -M mps2-an505 -nographic \
            -semihosting-config enable=on,target=native

.PHONY: all host dsp dsp-dump m33-dump tables tables-dsp tables-m33 compare \
        cost cost-check m33-cost trace test test-dsp test-host test-path test-path-mutations crosscheck clean

all: dsp-dump compare

$(B):
	mkdir -p $(B) $(B)/src_f32

# --- the float-typed copy of the sources the DSP build compiles
$(B)/src_f32/%.c: $(RN)/src/%.c tools/fp_literals.py | $(B)
	python3 tools/fp_literals.py $< $@

# --- the one libm function on the per-frame path: newlib's fdlibm log10f,
#     compiled for the HiFi4 so both cores round the band energies alike.
#     sqrtf and floorf are IEEE-exact and need no such treatment.
$(B)/ns_log10f.o: $(FDLIBM)/ef_log10.c $(FDLIBM)/math_config.h | $(B)
	$(XTCLANG) $(XTF) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_logf.o: $(FDLIBM)/ef_log.c $(FDLIBM)/math_config.h | $(B)
	$(XTCLANG) $(XTF) -I$(FDLIBM) $(NSLOG) -c $< -o $@

# HiFi4 matrix-vector kernel (two neurons per instruction)
$(B)/rnn_dsp.o: rnn_dsp.c $(RN)/src/rnn.h | $(B)
	$(XTCLANG) $(XTF) $(NSINC) -c $< -o $@

ifeq ($(SIMD),1)
$(B)/ns_log10f_arm.o: $(FDLIBM)/ef_log10.c $(FDLIBM)/math_config.h | $(B)
	$(ARMCC) $(ARMFLAGS) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_logf_arm.o: $(FDLIBM)/ef_log.c $(FDLIBM)/math_config.h | $(B)
	$(ARMCC) $(ARMFLAGS) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_log10f_host.o: $(FDLIBM)/ef_log10.c $(FDLIBM)/math_config.h | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_logf_host.o: $(FDLIBM)/ef_log.c $(FDLIBM)/math_config.h | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) -I$(FDLIBM) $(NSLOG) -c $< -o $@

XTLIBS := $(B)/ns_log10f.o $(B)/ns_logf.o $(B)/rnn_dsp.o
else
$(B)/ns_log10f_arm.o: $(FDLIBM)/ef_log10.c $(FDLIBM)/math_config.h | $(B)
	$(ARMCC) $(ARMFLAGS) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_logf_arm.o: $(FDLIBM)/ef_log.c $(FDLIBM)/math_config.h | $(B)
	$(ARMCC) $(ARMFLAGS) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_log10f_host.o: $(FDLIBM)/ef_log10.c $(FDLIBM)/math_config.h | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) -I$(FDLIBM) $(NSLOG) -c $< -o $@

$(B)/ns_logf_host.o: $(FDLIBM)/ef_log.c $(FDLIBM)/math_config.h | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) -I$(FDLIBM) $(NSLOG) -c $< -o $@

XTLIBS := $(B)/ns_log10f.o $(B)/ns_logf.o
endif
XTLINK := $(XTLIBS) -lsim -lm

# hardware DIV.S behind the compiler's __divsf3 call (see ns_fpu.c)
$(B)/ns_fpu.o: ns_fpu.c | $(B)
	$(XTCLANG) $(XTF) -c $< -o $@

dsp-dump: $(DSP_SRC) $(XTLIBS) | $(B)
	$(XTCLANG) $(XTF) -DNS_DUMP=1 -DNS_ITERS=8 $(XTDEFS) $(NSINC) $(DSP_SRC) \
	   bench/bench_main.c $(XTLINK) -o $(B)/dsp_dump$(V).elf
	$(XTRUN) $(B)/dsp_dump$(V).elf > $(B)/out_dsp$(V).txt 2>&1
	@grep -c '^F' $(B)/out_dsp$(V).txt

dsp: $(B)/dsp_cost$(V).elf

$(B)/dsp_cost$(V).elf: $(DSP_SRC) $(XTLIBS) bench/bench_main.c | $(B)
	$(XTCLANG) $(XTF) -DNS_ITERS=8 $(XTDEFS) $(NSINC) $(DSP_SRC) \
	   bench/bench_main.c $(XTLINK) -o $@

$(B)/dsp_cost0$(V).elf: $(DSP_SRC) $(XTLIBS) bench/bench_main.c | $(B)
	$(XTCLANG) $(XTF) -DNS_ITERS=0 $(XTDEFS) $(NSINC) $(DSP_SRC) \
	   bench/bench_main.c $(XTLINK) -o $@

# --- Cortex-M33: the firmware's own build, dumped
M33OBJ := $(B)/m33_denoise.o $(B)/m33_rnn.o $(B)/m33_rnn_data.o $(B)/m33_pitch.o \
          $(B)/m33_celt_lpc.o $(B)/m33_kiss_fft.o

$(B)/m33_%.o: $(RN)/src/%.c | $(B)
	$(ARMCC) $(ARMFLAGS) $(DEFS) $(NSINC) -c $< -o $@

m33-dump: $(M33OBJ) $(B)/ns_log10f_arm.o $(B)/ns_logf_arm.o | $(B)
	$(ARMCC) $(ARMFLAGS) -c common/startup.c -o $(B)/startup.o
	$(ARMCC) $(ARMFLAGS) -DNS_DUMP=1 -DNS_ITERS=8 -DNS_RDIMON=1 -DNS_OWN_SBRK=1 \
	   $(DEFS) $(NSINC) -c bench/bench_main.c -o $(B)/bench_m33.o
	$(ARMCC) $(ARMFLAGS) -T common/mps2.ld -specs=rdimon.specs -o $(B)/m33_dump.elf \
	   $(B)/startup.o $(B)/bench_m33.o $(M33OBJ) $(B)/ns_log10f_arm.o \
	   $(B)/ns_logf_arm.o -lrdimon -lm -lc
	$(QEMU_ARM) -kernel $(B)/m33_dump.elf > $(B)/out_m33.txt 2>&1 || true
	@grep -c '^F' $(B)/out_m33.txt

m33-cost: $(M33OBJ) $(B)/ns_log10f_arm.o $(B)/ns_logf_arm.o | $(B)
	$(ARMCC) $(ARMFLAGS) -c common/startup.c -o $(B)/startup.o
	$(ARMCC) $(ARMFLAGS) -DNS_ITERS=8 -DNS_RDIMON=1 -DNS_OWN_SBRK=1 \
	   $(DEFS) $(NSINC) -c bench/bench_main.c -o $(B)/bench_m33_cost.o
	$(ARMCC) $(ARMFLAGS) -T common/mps2.ld -specs=rdimon.specs \
	   -o $(B)/m33_cost.elf $(B)/startup.o $(B)/bench_m33_cost.o $(M33OBJ) \
	   -lrdimon -lm -lc
	@python3 tools/m33_cost.py $(B)/m33_cost.elf

# --- the service body (ns_dsp.c) end to end on the 8 frames, int16 in and out
test-dsp: $(DSP_SRC) $(XTLIBS) | $(B)
	$(XTCLANG) $(XTF) $(XTDEFS) $(NSINC) $(DSP_SRC) ns_dsp.c \
	   tests/test_ns_dsp.c $(XTLINK) -o $(B)/test_ns_dsp$(V).elf
	$(XTRUN) $(B)/test_ns_dsp$(V).elf > $(B)/out_ns_dsp$(V).txt 2>&1
	@grep -c '^I' $(B)/out_ns_dsp$(V).txt

test-host: | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) $(LIBFLAGS) -DNS_OWN_SBRK=1 $(DEFS) $(NSINC) -I$(FDLIBM) \
	   $(RNSRC) $(FDLIBM)/ef_log10.c $(FDLIBM)/ef_log.c \
	   -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f \
	   ns_dsp.c tests/test_ns_dsp.c -o $(B)/test_ns_dsp_host -lm
	./$(B)/test_ns_dsp_host > $(B)/out_ns_dsp_host.txt
	@python3 tools/cmp_int16.py $(B)/out_host.txt $(B)/out_ns_dsp_host.txt

test: test-dsp
	@python3 tools/cmp_int16.py $(B)/out_m33.txt $(B)/out_ns_dsp$(V).txt

# --- §4 and §5 of docs/TEST_DSP.md: continuity and latency of a 60 s stream,
#     and what the AP does when the DSP never answers.  Host builds only: the
#     model of the AP path is the point, and it has to run 6 000 frames.
PATH_TEST_SRC := tests/test_ns_path.c ns_dsp.c
PATH_TEST_LIB := $(RNSRC) $(FDLIBM)/ef_log10.c $(FDLIBM)/ef_log.c

$(B)/test_ns_path: $(PATH_TEST_SRC) $(PATH_TEST_LIB) | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) $(LIBFLAGS) -DNS_OWN_SBRK=1 $(DEFS) -I$(FDLIBM) \
	  -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f \
	  $(NSINC) $(PATH_TEST_LIB) $(PATH_TEST_SRC) -o $@ -lm

test-path: $(B)/test_ns_path
	./$(B)/test_ns_path

# The two mutations the tests have to catch.  Each is a build with one line
# changed, and both have to fail: a test that cannot fail is not a test.
$(B)/test_ns_path_drop: $(PATH_TEST_SRC) $(PATH_TEST_LIB) | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) $(LIBFLAGS) -DNS_OWN_SBRK=1 -DNS_DROP_FRAME=1 $(DEFS) \
	  -I$(FDLIBM) -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f \
	  $(NSINC) $(PATH_TEST_LIB) $(PATH_TEST_SRC) -o $@ -lm

$(B)/test_ns_path_nofb: $(PATH_TEST_SRC) $(PATH_TEST_LIB) | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) $(LIBFLAGS) -DNS_OWN_SBRK=1 -DNS_NO_FALLBACK=1 $(DEFS) \
	  -I$(FDLIBM) -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f \
	  $(NSINC) $(PATH_TEST_LIB) $(PATH_TEST_SRC) -o $@ -lm

test-path-mutations: $(B)/test_ns_path_drop $(B)/test_ns_path_nofb
	@echo "== mutation: one frame dropped by the stage (must FAIL)"
	@if ./$(B)/test_ns_path_drop; then \
	  echo "FAILED: the continuity test did not notice the dropped frame"; exit 1; \
	else echo "OK: continuity caught it"; fi
	@echo "== mutation: no fallback when the DSP is absent (must FAIL)"
	@if ./$(B)/test_ns_path_nofb; then \
	  echo "FAILED: the degradation test did not notice the silence"; exit 1; \
	else echo "OK: degradation caught it"; fi

compare:
	python3 tools/cmp_frames.py $(B)/out_m33.txt $(B)/out_dsp$(V).txt

# --- host, Cortex-M33 and HiFi4 on the same frames: the audio path must not
#     depend on the C library, so all three have to agree bit for bit.
crosscheck:
	sh tools/crosscheck.sh $${N:-32}

# --- per-frame cost on the ISS: total instructions with 8 frames minus the same
#     build with 0 frames, over 8; cycles per frame from the profile.
cost: $(B)/dsp_cost$(V).elf $(B)/dsp_cost0$(V).elf
	@$(XTRUN) --client_commands="summary" $(B)/dsp_cost0$(V).elf > $(B)/cost0$(V).txt 2>&1
	@$(XTRUN) --client_commands="summary" $(B)/dsp_cost$(V).elf > $(B)/cost8$(V).txt 2>&1
	@$(XTRUN) --profile=$(B)/dsp$(V).gmon $(B)/dsp_cost$(V).elf > /dev/null 2>&1
	@$(XTRUN) --profile=$(B)/dsp0$(V).gmon $(B)/dsp_cost0$(V).elf > /dev/null 2>&1
	@$(XTGPROF) $(B)/dsp_cost$(V).elf $(B)/dsp$(V).gmon > $(B)/gprof8$(V).txt 2>&1
	@$(XTGPROF) $(B)/dsp_cost0$(V).elf $(B)/dsp0$(V).gmon > $(B)/gprof0$(V).txt 2>&1
	@python3 tools/cost.py $(B)/cost0$(V).txt $(B)/cost8$(V).txt 8 $(B)/gprof0$(V).txt $(B)/gprof8$(V).txt \
	   --max-insn $(COST_MAX_INSN) --max-cycles $(COST_MAX_CYCLES)

# --- the same measurement as a pass/fail gate.  `make cost` already enforces it;
# this target exists so a CI or a reviewer can run nothing else.
cost-check: $(B)/dsp_cost$(V).elf $(B)/dsp_cost0$(V).elf
	@$(XTRUN) --client_commands="summary" $(B)/dsp_cost0$(V).elf > $(B)/cost0$(V).txt 2>&1
	@$(XTRUN) --client_commands="summary" $(B)/dsp_cost$(V).elf > $(B)/cost8$(V).txt 2>&1
	@python3 tools/cost.py $(B)/cost0$(V).txt $(B)/cost8$(V).txt 8 \
	   --max-insn $(COST_MAX_INSN) --max-cycles $(COST_MAX_CYCLES)

# --- the init-time tables both builds generate from their own libm
tables-dsp: $(DSP_SRC) $(XTLIBS) | $(B)
	$(XTCLANG) $(XTF) $(XTDEFS) $(NSINC) $(DSP_SRC) bench/tables_main.c \
	   $(XTLINK) -o $(B)/tables_dsp.elf
	$(XTRUN) $(B)/tables_dsp.elf > $(B)/tables_dsp.txt 2>&1

tables-m33: $(M33OBJ) $(B)/ns_log10f_arm.o $(B)/ns_logf_arm.o | $(B)
	$(ARMCC) $(ARMFLAGS) -c common/startup.c -o $(B)/startup.o
	$(ARMCC) $(ARMFLAGS) -DNS_RDIMON=1 $(DEFS) $(NSINC) \
	   -c bench/tables_main.c -o $(B)/tables_m33.o
	$(ARMCC) $(ARMFLAGS) -T common/mps2.ld -specs=rdimon.specs -o $(B)/tables_m33.elf \
	   $(B)/startup.o $(B)/tables_m33.o $(M33OBJ) $(B)/ns_log10f_arm.o \
	   $(B)/ns_logf_arm.o -lrdimon -lm -lc
	$(QEMU_ARM) -kernel $(B)/tables_m33.elf > $(B)/tables_m33.txt 2>&1 || true

tables: tables-dsp tables-m33
	@python3 tools/cmp_tables.py $(B)/tables_m33.txt $(B)/tables_dsp.txt

# ns_tables.h is generated once from the M33 dump and committed: the DSP build
# must not depend on an M33 run, only on the values that run produced.
ns_tables.h: tables-m33
	python3 tools/gen_tables.py $(B)/tables_m33.txt $@

# --- stage-by-stage trace of one frame, to find where two builds diverge
trace: $(B)/ns_log10f_arm.o $(B)/ns_logf_arm.o | $(B) $(XTLIBS) $(DSP_SRC)
	$(XTCLANG) $(XTF) -DNS_TRACE=1 -DNS_ITERS=1 $(XTDEFS) $(NSINC) \
	   $(B)/src_f32/denoise.c $(DSP_SRC) bench/bench_main.c $(XTLINK) \
	   -o $(B)/trace_dsp$(V).elf
	$(XTRUN) $(B)/trace_dsp$(V).elf > $(B)/trace_dsp$(V).txt 2>&1
	$(ARMCC) $(ARMFLAGS) -c common/startup.c -o $(B)/startup.o
	$(ARMCC) $(ARMFLAGS) -DNS_DUMP=1 -DNS_ITERS=1 -DNS_RDIMON=1 -DNS_OWN_SBRK=1 \
	   -DNS_TRACE=1 $(DEFS) $(NSINC) -c bench/bench_main.c -o $(B)/bench_m33_trace.o
	for f in $(RNSRC); do \
	  $(ARMCC) $(ARMFLAGS) -DNS_TRACE=1 $(DEFS) $(NSINC) -c $$f \
	     -o $(B)/t_$$(basename $$f .c).o || exit 1; \
	done
	$(ARMCC) $(ARMFLAGS) -T common/mps2.ld -specs=rdimon.specs -o $(B)/trace_m33.elf \
	   $(B)/startup.o $(B)/bench_m33_trace.o $(B)/t_denoise.o $(B)/t_rnn.o \
	   $(B)/t_rnn_data.o $(B)/t_pitch.o $(B)/t_celt_lpc.o $(B)/t_kiss_fft.o \
	   $(B)/ns_log10f_arm.o $(B)/ns_logf_arm.o -lrdimon -lm -lc
	$(QEMU_ARM) -kernel $(B)/trace_m33.elf > $(B)/trace_m33.txt 2>&1 || true
	@diff $(B)/trace_m33.txt $(B)/trace_dsp$(V).txt > $(B)/trace.diff && echo "traces identical" \
	  || (echo "first differences:"; head -6 $(B)/trace.diff)

# --- x86, only to check the harness itself.  PATH is cleared on purpose: with
# the Xtensa tools exported, `cc` picks up the Xtensa assembler.
host: | $(B)
	env -i PATH=/usr/bin:/bin HOME=$(HOME) TMPDIR=$(TMPDIR) \
	  cc -O2 $(NUM) $(LIBFLAGS) -DNS_DUMP=1 -DNS_OWN_SBRK=1 $(DEFS) $(NSINC) \
	   -I$(FDLIBM) $(RNSRC) $(FDLIBM)/ef_log10.c $(FDLIBM)/ef_log.c \
	   -D__ieee754_logf=ns_logf -D__ieee754_log10f=ns_log10f \
	   bench/bench_main.c -o $(B)/host_dump -lm
	./$(B)/host_dump > $(B)/out_host.txt
	@grep -c '^F' $(B)/out_host.txt

clean:
	rm -rf $(B)
