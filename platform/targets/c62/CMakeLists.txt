# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

#
# CMake configuration file for the Retevis C62 OpenRTX target
#
cmake_minimum_required(VERSION 3.20.0)

zephyr_compile_definitions(PSRAM_CHIP_SIZE=4) # 4MBytes PSRAM!

target_sources(app
  PRIVATE
    ${OPENRTX_ROOT}/platform/mcu/CSK6011B/drivers/delays.c

    ${OPENRTX_ROOT}/platform/drivers/GPIO/gpio_zephyr.c
    ${OPENRTX_ROOT}/platform/drivers/GPIO/gpio_bk4819.c
    ${OPENRTX_ROOT}/platform/drivers/SPI/spi_zephyr.c
    ${OPENRTX_ROOT}/platform/drivers/SPI/spi_zephyr_bus.c
    ${OPENRTX_ROOT}/platform/drivers/I2C/i2c_bitbang.c

    ${OPENRTX_ROOT}/platform/drivers/baseband/BK4819.c
    ${OPENRTX_ROOT}/platform/drivers/baseband/BK1080.c

    ${OPENRTX_ROOT}/platform/drivers/NVM/nvmem_C62.c
    ${OPENRTX_ROOT}/platform/drivers/NVM/channel_store_c62.c
    ${OPENRTX_ROOT}/platform/drivers/NVM/user_store_c62.c
    ${OPENRTX_ROOT}/platform/targets/c62/calib_C62.c
    ${OPENRTX_ROOT}/platform/drivers/stubs/cps_io_stub.c

    ${OPENRTX_ROOT}/platform/drivers/display/display_c62.c
    ${OPENRTX_ROOT}/platform/drivers/baseband/radio_C62.cpp
    ${OPENRTX_ROOT}/platform/drivers/keyboard/keyboard_c62.c
    ${OPENRTX_ROOT}/platform/drivers/audio/audio_c62.c
    ${OPENRTX_ROOT}/platform/drivers/audio/csk6_codec.c

    ${OPENRTX_ROOT}/platform/targets/c62/platform.c
    ${OPENRTX_ROOT}/platform/targets/c62/hwconfig.c
    ${OPENRTX_ROOT}/platform/targets/c62/dsp.c
    ${OPENRTX_ROOT}/platform/targets/c62/lsf.c
)

#
# Experimental RX options, set with e.g.
#   west build -b c62 -d build_c62 . -- -DC62_BASEBAND_DUMP_BAUD=2000000
# CMake caches them in the build directory: pass them again with a new value
# or use a fresh build directory to change them.
#
# C62_WIDE_RX: widest BK4819 RX channel filter in M17 mode (REG_43 0x7e28).
#   TETRA RX always uses that setting: its equalizer taps were measured on it.
#   OFF gives upstream's reviewed M17 setting, 12.5 kHz mode (0x7808).
# C62_BASEBAND_DUMP: stream the raw 48 kHz RX audio over the console UART,
#   muting the log. OFF by default: the dump holds 1 MiB of PSRAM (77,8 % of it
#   with the dump, 20,7 % without) and takes the console, and it is only armed
#   by holding A/B at power on anyway. Turn it on for a capture session:
#     west build -b c62 -d build_dump . -- -DC62_BASEBAND_DUMP=ON
#   See platform/drivers/audio/baseband_dump_c62.h, scripts/c62_bbdump.py and
#   docs/DMR_RX.md.
# C62_REG43_SWEEP: in M17 RX, cycle REG_43 through test values, each for
#   C62_REG43_SWEEP_MS. Use it with the dump to measure each setting.
#
option(C62_WIDE_RX "Widest BK4819 RX filter in M17 mode" ON)
option(C62_BASEBAND_DUMP "Stream raw RX baseband over UART" OFF)
# C62_REMOTE: remote control over the programming cable, for automated tests
#   (scripts/c62_remote.py). The cable's TX line is the PTT pad, which
#   becomes UART RX: PTT does not work in these builds.
option(C62_REMOTE "Remote control over the programming cable (no PTT)" OFF)
# The C62 programming cables top out at 115200 baud (11.5 kB/s), far below
# the 96 kB/s of a 48 kHz s16 stream: use burst mode there. Streaming needs
# at least 1500000 baud for s16 or 921600 for 12-bit samples.
set(C62_BASEBAND_DUMP_BAUD 115200 CACHE STRING "Baseband dump baud rate")
set(C62_BASEBAND_DUMP_FORMAT 1 CACHE STRING
    "Baseband dump sample format: 1 = s16, 2 = 12-bit packed")
option(C62_BASEBAND_DUMP_BURST
       "Fill 1 MiB of PSRAM, send it, repeat (gapless at any baud rate)" ON)
option(C62_REG43_SWEEP "Cycle REG_43 test values in M17 RX" OFF)
# C62_MODE_STATS: per-mode receive and transmit statistics (core/mode_stats.h),
#   read with the "rx" command of the remote control. OFF by default: with it
#   off the hooks compile to nothing and the image is the one the build would
#   produce without the layer. On, it costs a few integer adds per block of
#   baseband plus the cycle counter reads, and about 100 bytes of RAM per mode.
#     west build -b c62 -d build_stats . -- -DC62_MODE_STATS=ON -DC62_REMOTE=ON \
#         -DC62_BASEBAND_DUMP=ON
#   See docs/STATS.md.
option(C62_MODE_STATS "Per-mode RX/TX counters, quality and demodulation cost"
       OFF)
# C62_DMR_12K5: run the DMR mode in the BK4819's 12.5 kHz channel mode with the
#   4.5 kHz IF filters, which is what the stock firmware uses for its own 4FSK
#   digital mode (REG_43 = 0x7e48), instead of the 25 kHz mode used for TETRA
#   and M17-wide (0x7e28, where the IF filters are doubled to 9 kHz). See
#   reports/dmr_rx_hw.md.
option(C62_DMR_12K5 "DMR in the BK4819 12.5 kHz channel mode" OFF)
# C62_TETRA: TETRA TMO downlink receive mode ("T" under key 5), decoded with
#   libtetra, which west fetches to modules/lib/libtetra.
option(C62_TETRA "TETRA receive mode" ON)
# C62_TETRA_IF_DC_FILTER: keep the BK4819 RX IF DC filter on in T mode (the
#   flat audio mode bypasses it). OFF to compare.
option(C62_TETRA_IF_DC_FILTER "BK4819 RX IF DC filter in TETRA mode" ON)
# C62_TETRA_SPEECH: play clear TETRA speech with OpenACELP, which west
#   fetches to modules/lib/openacelp.
option(C62_TETRA_SPEECH "TETRA speech with OpenACELP" ON)
set(C62_REG43_SWEEP_MS 1400 CACHE STRING "REG_43 sweep dwell time, ms")

if(C62_WIDE_RX)
  target_compile_definitions(app PRIVATE C62_WIDE_RX)
endif()
if(C62_DMR_12K5)
  target_compile_definitions(app PRIVATE C62_DMR_12K5)
endif()
if(C62_REG43_SWEEP)
  target_compile_definitions(app PRIVATE C62_REG43_SWEEP
                             C62_REG43_SWEEP_MS=${C62_REG43_SWEEP_MS})
endif()
if(C62_MODE_STATS)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/core/mode_stats.c
    ${OPENRTX_ROOT}/platform/mcu/CSK6011B/drivers/cycles.c)
  target_compile_definitions(app PRIVATE CONFIG_C62_MODE_STATS)
  message(STATUS "C62: per-mode RX/TX statistics enabled")
endif()
if(C62_REMOTE AND NOT C62_BASEBAND_DUMP)
  message(FATAL_ERROR "C62_REMOTE needs C62_BASEBAND_DUMP: its replies are "
                      "dump control packets")
endif()
if(C62_REMOTE)
  target_sources(app PRIVATE ${OPENRTX_ROOT}/platform/drivers/audio/c62_remote.c)
  target_compile_definitions(app PRIVATE C62_REMOTE)
endif()
if(C62_BASEBAND_DUMP)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/platform/drivers/audio/baseband_dump_c62.c)
  target_compile_definitions(app PRIVATE C62_BASEBAND_DUMP
    C62_BASEBAND_DUMP_BAUD=${C62_BASEBAND_DUMP_BAUD}
    C62_BASEBAND_DUMP_FORMAT=${C62_BASEBAND_DUMP_FORMAT})
  if(C62_BASEBAND_DUMP_BURST)
    target_compile_definitions(app PRIVATE C62_BASEBAND_DUMP_BURST=1)
  else()
    target_compile_definitions(app PRIVATE C62_BASEBAND_DUMP_BURST=0)
  endif()
endif()
#
# DMR: the protocol package of protocols/DMR/ (dmr_demod.c .. trunk/, the tree
# of c62/tetra as packaged, see docs/DMR_PKG.md for the map and licences).
#
# Options:
#   CONFIG_C62_DMR_RX  the mode                     (Kconfig, OFF)
#   CONFIG_DMR_TX      the transmit chain           (Kconfig, OFF)
#   CONFIG_DMR_TRUNK   Tier III                     (Kconfig, ON)
#   CONFIG_DMR_SMS     messages bridged to core/messaging (Kconfig, ON)
#   C62_DMR_VOICE      the AMBE+2 decoder: ours (OFF, default) or mbelib (ON)
#   DMR_ENC            the AMBE+2 encoder: ours (OFF, default) or OpenDMR (ON)
#
# mbelib and OpenDMR are the package's choice, fetched by
# scripts/c62/fetch_mbelib.sh and scripts/c62/fetch_opendmr.sh into
# <west>/modules/lib/.  Both are off by default now: neither vocoder fits the
# M33 in real time (mbelib 9.36 M instructions per 20 ms frame, OpenDMR
# 4.10 M), while lib/dmr_vocoder (113 k) and lib/dmr_encoder (1.43 M) do.
# They stay selectable so the decision can be revisited with measurements;
# mbelib is also the only one that decodes D-STAR and P25 IMBE.  See
# docs/DMR_PKG.md, "The vocoders".
#
set(C62_DMR_VOICE OFF CACHE STRING
    "DMR voice: AMBE+2 decoder, ON = mbelib, OFF = lib/dmr_vocoder (ours)")
set(DMR_ENC OFF CACHE STRING
    "DMR transmitter: AMBE+2 encoder, ON = OpenDMR, OFF = lib/dmr_encoder (ours)")

set(C62_DMR_SRC ${OPENRTX_ROOT}/openrtx/src/protocols/DMR)
set(C62_MBELIB ${WEST_TOPDIR}/modules/lib/mbelib)
set(C62_OPENDMR ${WEST_TOPDIR}/modules/lib/opendmr)

#
# lib/dmr_vocoder: the AMBE+2 3600x2450 decoder, one static library shared by
# every mode that carries that frame -- DMR (the package's voice path), NXDN,
# YSF and dPMR -- and, through dmr_golay.c, the D-STAR front end.  It is built
# here, once, whichever of those modes asked for it: a second
# add_subdirectory() on the same binary directory is a CMake error.
#
if(CONFIG_C62_DMR_RX OR CONFIG_C62_DMR_VOCODER)
  add_subdirectory(${OPENRTX_ROOT}/lib/dmr_vocoder
                   ${CMAKE_BINARY_DIR}/dmr_vocoder)
  # Build it with the Zephyr compiler flags (CPU, FPU, ABI, include paths).
  target_compile_options(dmr_vocoder PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_OPTIONS>)
  target_compile_definitions(dmr_vocoder PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>)
  target_include_directories(dmr_vocoder PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
  target_include_directories(dmr_vocoder SYSTEM PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
  target_link_libraries(app PRIVATE dmr_vocoder)
  target_include_directories(app PRIVATE ${OPENRTX_ROOT}/lib/dmr_vocoder)

  if(CONFIG_C62_DMR_VOCODER)
    target_compile_definitions(app PRIVATE CONFIG_DMR_VOCODER)
  endif()

  # The library's single instance (8 540 B, one decode per 20 ms frame) goes
  # to PSRAM: see the comment on dmr_instance in dmr_vocoder.c.  The .bss flag
  # that guards its first initialisation stays in SRAM.
  target_compile_definitions(dmr_vocoder PRIVATE
    "DMR_VOCODER_STATE_ATTR=__attribute__((section(\".psram_section\")))")

  # A measurement aid, off by default: an unreferenced static library is
  # dropped by the linker, and this forces the symbol in so its cost can be
  # read off the real image.
  option(C62_DMR_VOCODER_FORCE_LINK
         "Link the vocoder without a caller (size measurement only)" OFF)
  if(C62_DMR_VOCODER_FORCE_LINK)
    # Must precede the library on the link line: -u is position sensitive.
    zephyr_link_libraries(-Wl,-u,dmr_vocoder_decode)
  endif()

  message(STATUS "C62: AMBE+2 decoder enabled"
                 " (force link: ${C62_DMR_VOCODER_FORCE_LINK})")
endif()

#
# C62_APRS: APRS on the analog FM path (messaging, beacon, station list).
# The decoder is off at run time until it is enabled in the menu: this option
# only decides whether the code is linked in. See docs/APRS.md.
#
#
# C62_MESSAGING: the shared text message store (core/messaging.c): outbox,
# inbox, states and the flash record, used by APRS, DMR SMS and TETRA SDS.  The
# messaging screen of the UI talks to this and nothing else.
#
# The option is NOT called CONFIG_MESSAGING on purpose, and so is C62_SCAN
# below: Zephyr takes every CMake cache variable named CONFIG_* as a Kconfig
# assignment (cmake/modules/kconfig.cmake), so an option named that way
# survives in CMakeCache.txt and the next configure aborts with "attempt to
# assign the value 'ON' to the undefined symbol".  The CONFIG_* spelling is the
# C macro the sources see, and that one is emitted by target_compile_definitions
# below -- or, for DMR_SMS and DMR_TRUNK, it is the Kconfig symbol itself, read
# here as a CMake variable (CONFIG_DMR_SMS), which is not a cache entry.
#
option(C62_MESSAGING "Shared text message store (APRS, DMR SMS, TETRA SDS)" ON)

if(C62_MESSAGING)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/core/messaging.c
    ${OPENRTX_ROOT}/openrtx/src/core/messaging_store_zephyr.c)
  target_compile_definitions(app PRIVATE CONFIG_MESSAGING)
  message(STATUS "C62: shared message store enabled")
endif()

#
# C62_SCAN: memory and VFO scanning with a priority channel, per-channel
# SKIP and a configurable resume rule (core/scanner.c, core/scan_service.c).
# The state machine is pure and unit tested on the host; the service is what
# moves the radio, from state_task(). See docs/SCAN.md.
#
option(C62_SCAN "Channel and VFO scan with priority and configurable resume" ON)

if(C62_SCAN)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/core/scanner.c
    ${OPENRTX_ROOT}/openrtx/src/core/scan_service.c)
  target_compile_definitions(app PRIVATE CONFIG_SCAN)
  message(STATUS "C62: scan and vigilance enabled")
endif()

#
# C62_VFO_SEARCH: carrier search on the VFO with a long press of UP or DOWN
# (core/vfo_search.c, core/vfo_search_service.c, rtx/ModeReport.c).  The
# machine is pure and unit tested on the host; the service moves the radio from
# state_task() and reads the per-mode sync counters through ModeReport.c.
# See docs/VFO_SEARCH.md.
#
option(C62_VFO_SEARCH "VFO carrier search on a long press of UP/DOWN" ON)

if(C62_VFO_SEARCH)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/core/vfo_search.c
    ${OPENRTX_ROOT}/openrtx/src/core/vfo_search_service.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/ModeReport.c)
  target_compile_definitions(app PRIVATE CONFIG_VFO_SEARCH)
  message(STATUS "C62: VFO carrier search enabled")
endif()

if(CONFIG_C62_APRS)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/APRS/ax25.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/APRS/hdlc.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/APRS/afsk.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/APRS/aprs.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/APRS/aprs_msg.c
    ${OPENRTX_ROOT}/openrtx/src/core/aprs_service.c
    ${OPENRTX_ROOT}/openrtx/src/ui/default/ui_aprs.c)
  target_compile_definitions(app PRIVATE CONFIG_APRS)
  message(STATUS "C62: APRS on the FM path enabled")

  if(C62_MESSAGING)
    # The APRS leg of the message store: queued messages go into its mailbox.
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/openrtx/src/core/messaging_aprs.c)
  endif()
endif()

#
# C62_KISS: the packet modem (KISS TNC) and its configuration page.
# docs/KISS.md is the reference. The service runs in its own thread and only
# touches the audio path while the operator has it enabled; the protocol layer
# (protocols/KISS/) is freestanding and is the same code the host bench runs.
#
# C62_KISS_UART: bind the KISS stream to the programming cable (UART2 RXD on
# the PTT pad, log muted). It needs the pad, so it is off by default and
# mutually exclusive with C62_REMOTE; when the PC protocol layer lands it binds
# its own transport with packet_service_bind() and this option stays off.
#
# Both are Kconfig symbols (Kconfig.board), read here as CMake variables like
# CONFIG_C62_APRS and CONFIG_C62_DMR_RX: calling them option(C62_KISS) would
# leave the -DCONFIG_C62_KISS=y of the build scripts with nothing to enable.
#
if(CONFIG_C62_KISS)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/KISS/kiss.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/KISS/afsk9600.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/KISS/packet_modem.c
    ${OPENRTX_ROOT}/openrtx/src/core/packet_service.c
    ${OPENRTX_ROOT}/openrtx/src/ui/default/ui_packet.c)
  target_compile_definitions(app PRIVATE CONFIG_KISS)
  message(STATUS "C62: packet modem (KISS) enabled")

  if(CONFIG_C62_KISS_UART)
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/platform/drivers/audio/c62_kiss.c)
    target_compile_definitions(app PRIVATE C62_KISS_UART)
    if(C62_REMOTE)
      message(FATAL_ERROR "C62_KISS_UART and C62_REMOTE share the PTT pad")
    endif()
    if(NOT CONFIG_C62_APRS)
      message(WARNING "C62_KISS_UART: the packet modem needs the AX.25 layer, "
                      "enable CONFIG_C62_APRS for ax25.c and hdlc.c")
    endif()
  endif()
endif()

#
# C62_NXDN_RX: NXDN receive mode (see docs/RX_NXDN.md).  The PHY, the FEC and
# the frame handling are in protocols/NXDN; voice goes through lib/dmr_vocoder,
# which is the same AMBE+2 decoder DMR uses because a NXDN voice channel is one
# AMBE+2 3600x2450 frame.  Receive only: there is no transmitter.
#
if(CONFIG_C62_NXDN_RX)
  if(NOT CONFIG_C62_DMR_VOCODER)
    message(FATAL_ERROR "CONFIG_C62_NXDN_RX needs CONFIG_C62_DMR_VOCODER")
  endif()
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/NXDN/NxdnDefs.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/NXDN/NxdnFec.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/NXDN/NxdnPhy.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/NXDN/NxdnSpeech.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/NXDN/NxdnStatus.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_NXDN.cpp)
  target_compile_definitions(app PRIVATE CONFIG_NXDN)
  message(STATUS "C62: NXDN receive mode enabled")
endif()

#
# CONFIG_C62_DPMR_RX: the dPMR mode (ETSI TS 102 490), receive only.
#
# 4FSK at 2400 symbols/s on a 6,25 kHz channel, 80 ms frames of 384 bits,
# AMBE+2 3600x2450 voice through lib/dmr_vocoder.  Its own demodulator, coding
# layer, short data, playout and mode: a 161 tap matched filter at 48 kHz
# decimated to 9,6 kHz, a 1024 sample ring, and no transmitter at all.  The
# option is a Kconfig symbol in Kconfig.board, and it selects C62_DMR_VOCODER
# when DMR RX is not built, because the block above is what builds the library.
# See docs/RX_DPMR.md for the measured numbers.
#
if(CONFIG_C62_DPMR_RX)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/DPMR/DpmrDefs.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DPMR/DpmrData.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DPMR/DpmrPhy.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DPMR/DpmrSpeech.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_DPMR.cpp)
  target_compile_definitions(app PRIVATE CONFIG_DPMR)

  # The slicer and the sync search decide on margins (EVM, correlation score),
  # so the receiver must not depend on whether the compiler fuses multiply-adds:
  # the same capture gives 111 frames with contraction and 121 without.  -Os,
  # which is what this image uses, and -O2 then agree, host and firmware.
  set_source_files_properties(
    ${OPENRTX_ROOT}/openrtx/src/protocols/DPMR/DpmrPhy.c
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

  message(STATUS "C62: dPMR receive mode enabled")
endif()

#
# C62_YSF: Yaesu System Fusion receive mode.  The frame layer reuses the Golay
# decoder and the AMBE+2 vocoder of lib/dmr_vocoder, so the option selects
# C62_DMR_VOCODER.  See docs/RX_FUSION.md.
#
if(CONFIG_C62_YSF)
  if(NOT CONFIG_C62_DMR_VOCODER)
    message(FATAL_ERROR "CONFIG_C62_YSF needs CONFIG_C62_DMR_VOCODER")
  endif()
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/YSF/YsfPhy.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/YSF/YsfFrame.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/YSF/YsfSpeech.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_YSF.cpp)
  target_compile_definitions(app PRIVATE CONFIG_YSF)
  message(STATUS "C62: System Fusion (C4FM) receive mode enabled")
endif()

if(CONFIG_C62_DSTAR)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarFec.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarPhy.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarSlowData.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarSpeech.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarStatus.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarVocoder.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_DSTAR.cpp)
  target_compile_definitions(app PRIVATE CONFIG_DSTAR)
  if(C62_MESSAGING)
    target_compile_definitions(app PRIVATE CONFIG_DSTAR_MESSAGING)
  endif()
  message(STATUS "C62: D-STAR DV mode enabled")

  # The voice front end (Golay + PN of the AMBE 2400 channel frame) reuses the
  # Golay decoder of the DMR vocoder library.
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/lib/dmr_vocoder/dmr_golay.c
    ${OPENRTX_ROOT}/lib/dmr_vocoder/dmr_golay_table.c)
  target_include_directories(app PRIVATE ${OPENRTX_ROOT}/lib/dmr_vocoder)

  #
  # C62_DSTAR_VOCODER: the AMBE 3600x2400 voice decoder itself
  # (lib/dstar_vocoder, a port of mbelib-neo v2.6.2's D-STAR path).
  #
  # It is compiled into the application rather than into an archive so that it
  # overrides the weak dstarVocoder_decode()/dstarVocoder_available() hooks of
  # the protocol layer, and its state is placed in PSRAM: 14 KiB of it, which
  # the radio's 256 KB of internal RAM cannot spare.  The library needs the
  # DMR vocoder's math and FFT (dmr_math.h is header-only, dmr_fft.c is not).
  #
  if(CONFIG_C62_DSTAR_VOCODER)
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/lib/dstar_vocoder/dstar_vocoder.c
      ${OPENRTX_ROOT}/lib/dstar_vocoder/dstar_decoder.c
      ${OPENRTX_ROOT}/lib/dmr_vocoder/dmr_fft.c)
    target_include_directories(app PRIVATE ${OPENRTX_ROOT}/lib/dstar_vocoder)
    target_compile_definitions(app PRIVATE CONFIG_DSTAR_VOCODER
      "DSTAR_VOCODER_STATE_ATTR=__attribute__((section(\".psram_section\")))")
    message(STATUS "C62: D-STAR AMBE 3600x2400 voice decoder enabled"
                   " (state in PSRAM)")
  endif()

  if(CONFIG_DSTAR_TX)
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/openrtx/src/protocols/DSTAR/DStarTx.c)
    target_compile_definitions(app PRIVATE CONFIG_DSTAR_TX)
    message(STATUS "C62: D-STAR transmitter enabled")
  endif()
endif()

if(CONFIG_C62_DMR_RX)
  target_include_directories(app PRIVATE ${C62_DMR_SRC}
    ${OPENRTX_ROOT}/openrtx/include/protocols/DMR
    ${OPENRTX_ROOT}/openrtx/include/protocols/DMR/trunk)

  target_sources(app PRIVATE
    ${C62_DMR_SRC}/DmrDefs.c
    ${C62_DMR_SRC}/DmrStatus.c
    ${C62_DMR_SRC}/dmr_glue.c
    ${C62_DMR_SRC}/dmr_demod.c
    ${C62_DMR_SRC}/dmr_fec.c
    ${C62_DMR_SRC}/dmr_trellis.c
    ${C62_DMR_SRC}/dmr_proto.c
    ${C62_DMR_SRC}/dmr_data.c
    ${C62_DMR_SRC}/dmr_lrrp.c
    ${C62_DMR_SRC}/dmr_track.c
    ${C62_DMR_SRC}/dmr_voice.c
    ${C62_DMR_SRC}/DmrSpeech.c
    ${C62_DMR_SRC}/dmr_sms.c
    ${C62_DMR_SRC}/dmr_ep.c
    ${C62_DMR_SRC}/dmr_aes.c
    ${C62_DMR_SRC}/dmr_hytera.c
    ${C62_DMR_SRC}/aes/tinyaes.c
    ${C62_DMR_SRC}/aes/tinyaes128.c
    ${C62_DMR_SRC}/aes/des.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_DMR.cpp)
  target_compile_definitions(app PRIVATE CONFIG_DMR)
  message(STATUS "C62: DMR mode enabled (protocol package)")

  # ---- voice: ours by default, mbelib selectable (off) --------------------
  if(C62_DMR_VOICE STREQUAL "AUTO")
    set(C62_DMR_VOICE OFF) # old build lines: AUTO used to mean "mbelib if there"
  endif()
  if(C62_DMR_VOICE)
    if(NOT EXISTS ${C62_MBELIB}/mbelib.c)
      message(FATAL_ERROR "C62_DMR_VOICE needs mbelib in ${C62_MBELIB}: run "
                          "scripts/c62/fetch_mbelib.sh")
    endif()
    file(GLOB C62_MBELIB_SRC ${C62_MBELIB}/*.c)
    # Its own static library: mbelib is C89-ish and warns on Zephyr's flag set.
    # A COMPILE_OPTIONS property on the sources does not reach the command
    # line here, so the -w is a target option of the library (as libtetra).
    add_library(c62_mbelib STATIC ${C62_MBELIB_SRC})
    target_compile_options(c62_mbelib PRIVATE
      $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_OPTIONS> -w)
    target_compile_definitions(c62_mbelib PRIVATE
      $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>
      _GNU_SOURCE M_PI=3.14159265358979323846f)
    target_include_directories(c62_mbelib PRIVATE
      $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>
      ${C62_MBELIB})
    target_include_directories(c62_mbelib SYSTEM PRIVATE
      $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
    target_link_libraries(app PRIVATE c62_mbelib)
    # Only dmr_voice.c may see this mbelib.h: OpenDMR carries its own copy in
    # decoder/, and a target-wide -I would shadow it (two different
    # declarations of the same symbols, one without extern "C").
    set_source_files_properties(${C62_DMR_SRC}/dmr_voice.c
      DIRECTORY ${CMAKE_SOURCE_DIR}
      PROPERTIES INCLUDE_DIRECTORIES ${C62_MBELIB})
    target_compile_definitions(app PRIVATE DMR_VOICE_ENABLED)
    message(STATUS "C62: DMR voice: mbelib (reference, 9.36 M instr/frame)")
  else()
    # Ours: lib/dmr_vocoder, 113 k instructions per 20 ms frame.  Its state
    # (8 540 B) goes to DTCM from dmr_voice.c (DMR_FAST_BSS), so only the
    # per-instance header is used; dmr_vocoder.c stays in the library for
    # the host tests.  The library itself is built once by the shared block
    # near the top of this file (NXDN, YSF and dPMR link the same one).
    target_compile_definitions(app PRIVATE DMR_VOICE_OWN)
    message(STATUS "C62: DMR voice: lib/dmr_vocoder (ours, 113 k instr/frame)")
  endif()

  # ---- short messages ----------------------------------------------------
  if(CONFIG_DMR_SMS)
    if(NOT C62_MESSAGING)
      message(FATAL_ERROR "CONFIG_DMR_SMS needs C62_MESSAGING")
    endif()
    target_compile_definitions(app PRIVATE CONFIG_DMR_SMS)
    message(STATUS "C62: DMR short messages bridged to core/messaging")
  endif()

  # ---- Tier III ----------------------------------------------------------
  if(CONFIG_DMR_TRUNK)
    target_include_directories(app PRIVATE ${C62_DMR_SRC}/trunk)
    target_sources(app PRIVATE
      ${C62_DMR_SRC}/trunk/dmr_trunk.c
      ${C62_DMR_SRC}/trunk/t3_ms.c
      ${C62_DMR_SRC}/trunk/t3_pdu.c
      ${C62_DMR_SRC}/trunk/t3_plan.c
      ${C62_DMR_SRC}/trunk/t3_sys.c)
    target_compile_definitions(app PRIVATE CONFIG_DMR_TRUNK)
    message(STATUS "C62: DMR Tier III enabled")
  endif()

  # ---- transmitter -------------------------------------------------------
  if(CONFIG_DMR_TX)
    target_sources(app PRIVATE
      ${C62_DMR_SRC}/dmr_tx.c
      ${C62_DMR_SRC}/dmr_enc.c)
    target_compile_definitions(app PRIVATE CONFIG_DMR_TX)
    message(STATUS "C62: DMR transmit chain enabled")

    if(DMR_ENC STREQUAL "AUTO")
      set(DMR_ENC OFF) # old build lines: ours is the default now
    endif()
    if(DMR_ENC)
      if(NOT EXISTS ${C62_OPENDMR}/opendmr.h)
        message(FATAL_ERROR "DMR_ENC needs OpenDMR in ${C62_OPENDMR}: run "
                            "scripts/c62/fetch_opendmr.sh")
      endif()
      # OpenDMR is self-contained (its own mbelib in decoder/, the IMBE
      # analyser in encoder/): the same list as its Makefile.  It is its own
      # static library for two reasons: the -w (third-party C and C++), and
      # the defines below, which must reach every one of its translation
      # units (a source-file property does not, see c62_mbelib above).
      set(C62_OPENDMR_SRC
        ${C62_OPENDMR}/opendmr.cpp
        ${C62_OPENDMR}/decoder/mbelib.c
        ${C62_OPENDMR}/decoder/mbe_adaptive.c
        ${C62_OPENDMR}/decoder/mbe_unvoiced_fft.c
        ${C62_OPENDMR}/decoder/ambe3600x2450.c
        ${C62_OPENDMR}/decoder/ambe_common.c
        ${C62_OPENDMR}/decoder/ecc.c
        ${C62_OPENDMR}/decoder/ecc_const.c
        ${C62_OPENDMR}/decoder/pffft.c
        ${C62_OPENDMR}/decoder/fftpack.c
        ${C62_OPENDMR}/encoder/cgolay24128.cpp
        ${C62_OPENDMR}/encoder/mbeenc.cpp
        ${C62_OPENDMR}/encoder/aux_sub.cc
        ${C62_OPENDMR}/encoder/basicop2.cc
        ${C62_OPENDMR}/encoder/ch_encode.cc
        ${C62_OPENDMR}/encoder/dc_rmv.cc
        ${C62_OPENDMR}/encoder/dsp_sub.cc
        ${C62_OPENDMR}/encoder/encode.cc
        ${C62_OPENDMR}/encoder/imbe_vocoder.cc
        ${C62_OPENDMR}/encoder/imbe_vocoder_impl.cc
        ${C62_OPENDMR}/encoder/math_sub.cc
        ${C62_OPENDMR}/encoder/pe_lpf.cc
        ${C62_OPENDMR}/encoder/pitch_est.cc
        ${C62_OPENDMR}/encoder/pitch_ref.cc
        ${C62_OPENDMR}/encoder/qnt_sub.cc
        ${C62_OPENDMR}/encoder/rand_gen.cc
        ${C62_OPENDMR}/encoder/sa_encode.cc
        ${C62_OPENDMR}/encoder/tbls.cc
        ${C62_OPENDMR}/encoder/v_uv_det.cc)
      # OpenDMR carries its own mbelib (decoder/), and the receive voice path
      # links mbelib proper for dmr_voice.c: both export the same 28 globals
      # (golayGenerator ... mbe_useLastMbeParms).  Every OpenDMR translation
      # unit sees them renamed, so the two libraries stay whole and apart.
      # Mechanical: the names are mangled the same way inside OpenDMR, and
      # nobody outside refers to its copies.
      set(C62_OPENDMR_PREFIX -DgolayGenerator=odmr_golayGenerator;-DgolayMatrix=odmr_golayMatrix;-DhammingGenerator=odmr_hammingGenerator;-Dimbe7100x4400hammingGenerator=odmr_imbe7100x4400hammingGenerator;-Dmbe_7100x4400hamming1511=odmr_mbe_7100x4400hamming1511;-Dmbe_checkGolayBlock=odmr_mbe_checkGolayBlock;-Dmbe_decodeAmbe2450Parms=odmr_mbe_decodeAmbe2450Parms;-Dmbe_demodulateAmbe3600x2450Data=odmr_mbe_demodulateAmbe3600x2450Data;-Dmbe_dumpAmbe2450Data=odmr_mbe_dumpAmbe2450Data;-Dmbe_dumpAmbe3600x2450Frame=odmr_mbe_dumpAmbe3600x2450Frame;-Dmbe_eccAmbe3600x2450C0=odmr_mbe_eccAmbe3600x2450C0;-Dmbe_eccAmbe3600x2450Data=odmr_mbe_eccAmbe3600x2450Data;-Dmbe_floattoshort=odmr_mbe_floattoshort;-Dmbe_golay2312=odmr_mbe_golay2312;-Dmbe_hamming1511=odmr_mbe_hamming1511;-Dmbe_initMbeParms=odmr_mbe_initMbeParms;-Dmbe_moveMbeParms=odmr_mbe_moveMbeParms;-Dmbe_printVersion=odmr_mbe_printVersion;-Dmbe_processAmbe2450Data=odmr_mbe_processAmbe2450Data;-Dmbe_processAmbe2450Dataf=odmr_mbe_processAmbe2450Dataf;-Dmbe_processAmbe3600x2450Frame=odmr_mbe_processAmbe3600x2450Frame;-Dmbe_processAmbe3600x2450Framef=odmr_mbe_processAmbe3600x2450Framef;-Dmbe_spectralAmpEnhance=odmr_mbe_spectralAmpEnhance;-Dmbe_synthesizeSilence=odmr_mbe_synthesizeSilence;-Dmbe_synthesizeSilencef=odmr_mbe_synthesizeSilencef;-Dmbe_synthesizeSpeech=odmr_mbe_synthesizeSpeech;-Dmbe_synthesizeSpeechf=odmr_mbe_synthesizeSpeechf;-Dmbe_useLastMbeParms=odmr_mbe_useLastMbeParms)
      add_library(c62_opendmr STATIC ${C62_OPENDMR_SRC})
      target_compile_options(c62_opendmr PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_OPTIONS> -w)
      # MBE_THREAD_LOCAL carries the DMR PSRAM section: the C62 has no TLS
      # (the header is patched by scripts/c62/fetch_opendmr.sh to be
      # overridable), so the four statics that hook marks are plain statics in
      # .psram_dmr, which platform_init zeroes at boot.  They are the decoder's
      # DCT cosine cache (23.7 KB, built on the first decode and only read
      # afterwards) and three small decoder seeds/pointers: the transmitter
      # never touches them, and the receiver's voice goes through mbelib, not
      # through OpenDMR's decoder, whose code the linker drops.
      target_compile_definitions(c62_opendmr PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>
        _GNU_SOURCE M_PI=3.14159265358979323846f
        "MBE_THREAD_LOCAL=__attribute__((section(\".psram_dmr\")))"
        ${C62_OPENDMR_PREFIX})
      target_include_directories(c62_opendmr PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>
        ${C62_OPENDMR} ${C62_OPENDMR}/decoder ${C62_OPENDMR}/encoder)
      target_include_directories(c62_opendmr SYSTEM PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
      target_link_libraries(app PRIVATE c62_opendmr)
      # dmr_enc.c includes opendmr.h. SYSTEM, so a -I of our own headers
      # (dmr_voice.c's mbelib.h) still wins for the file that needs it.
      target_include_directories(app SYSTEM PRIVATE ${C62_OPENDMR}
        ${C62_OPENDMR}/decoder ${C62_OPENDMR}/encoder)
      target_compile_definitions(app PRIVATE CONFIG_DMR_ENC_OPENDMR)
      message(STATUS "C62: DMR encoder: OpenDMR (reference, 4.10 M instr/frame)")
    else()
      # Ours: lib/dmr_encoder, 1.43 M instructions per 20 ms frame (28-38 %
      # of the M33).  dmr_enc.c interleaves its canonical frames to the burst
      # order as it does OpenDMR's (see dmr_enc.c).
      add_subdirectory(${OPENRTX_ROOT}/lib/dmr_encoder
                       ${CMAKE_BINARY_DIR}/dmr_encoder)
      target_compile_options(dmr_encoder PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_OPTIONS>)
      target_compile_definitions(dmr_encoder PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>)
      target_include_directories(dmr_encoder PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
      target_include_directories(dmr_encoder SYSTEM PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
      target_link_libraries(app PRIVATE dmr_encoder)
      target_include_directories(app PRIVATE ${OPENRTX_ROOT}/lib/dmr_encoder)
      message(STATUS "C62: DMR encoder: lib/dmr_encoder (ours, 1.43 M instr/frame)")
    endif()
  endif()
endif()

#
# CONFIG_C62_P25: P25 Phase 1 receive mode (protocols/P25). The PHY, the FEC and
# the frame decoding are the C port of the host prototype in work/p25-rx, which
# is validated against dsd-neo and op25 on real captures (docs/RX_P25.md).
# The ring and the sliced-frame buffers live in PSRAM; the vocoder is a hook
# (protocols/P25/P25Vocoder.c) with no backend in this tree, so voice needs
# CONFIG_P25_VOCODER and an IMBE decoder linked in.
#
if(CONFIG_C62_P25)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Defs.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Fec.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Phy.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Speech.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Status.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Tables.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/P25/P25Vocoder.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_P25.cpp)
  target_compile_definitions(app PRIVATE CONFIG_P25)
  message(STATUS "C62: P25 Phase 1 receive enabled")

  if(CONFIG_C62_P25_VOCODER)
    target_compile_definitions(app PRIVATE CONFIG_P25_VOCODER)
    message(STATUS "C62: P25 IMBE vocoder enabled")
  endif()
endif()

if(C62_TETRA)
  set(C62_LIBTETRA ${WEST_TOPDIR}/modules/lib/libtetra)
  if(NOT EXISTS ${C62_LIBTETRA}/CMakeLists.txt)
    message(FATAL_ERROR "libtetra not found in ${C62_LIBTETRA}: run "
                        "'west update --group-filter +csk6', or build with "
                        "-DC62_TETRA=OFF")
  endif()
  add_subdirectory(${C62_LIBTETRA} ${CMAKE_BINARY_DIR}/libtetra)
  # Build it with the Zephyr compiler flags (CPU, FPU, ABI), without a link
  # dependency on zephyr_interface: libtetra may export its target
  target_compile_options(tetra PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_OPTIONS>)
  target_compile_definitions(tetra PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>)
  target_include_directories(tetra PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
  target_include_directories(tetra SYSTEM PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
  target_link_libraries(app PRIVATE tetra)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/TetraDemod.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/TetraSds.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/TetraSdsEnc.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/SdsMsg.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/SdsHist.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/TetraStatus.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_TETRA.cpp)
  target_compile_definitions(app PRIVATE CONFIG_TETRA)
  if(C62_TETRA_IF_DC_FILTER)
    target_compile_definitions(app PRIVATE C62_TETRA_IF_DC_FILTER)
  endif()

  if(C62_TETRA_SPEECH)
    set(C62_OPENACELP ${WEST_TOPDIR}/modules/lib/openacelp)
    if(NOT EXISTS ${C62_OPENACELP}/src/oa_tables_tetra.c)
      message(FATAL_ERROR "OpenACELP not found in ${C62_OPENACELP}: run "
                          "'west update --group-filter +csk6', or build with "
                          "-DC62_TETRA_SPEECH=OFF")
    endif()
    target_sources(app PRIVATE
      ${C62_OPENACELP}/src/oa_dsp.c
      ${C62_OPENACELP}/src/oa_bits.c
      ${C62_OPENACELP}/src/oa_dec.c
      ${C62_OPENACELP}/src/oa_tables_tetra.c)
    target_include_directories(app PRIVATE
      ${C62_OPENACELP}/include ${C62_OPENACELP}/src)
    target_compile_definitions(app PRIVATE CONFIG_TETRA_SPEECH)
  endif()
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/TETRA/TetraSpeech.c)
endif()

message(STATUS "C62: wide RX ${C62_WIDE_RX}, TETRA ${C62_TETRA} "
               "(speech ${C62_TETRA_SPEECH}), REG_43 sweep ${C62_REG43_SWEEP}, "
               "DMR 12.5 kHz ${C62_DMR_12K5}, "
               "baseband dump ${C62_BASEBAND_DUMP} "
               "(${C62_BASEBAND_DUMP_BAUD} baud, format "
               "${C62_BASEBAND_DUMP_FORMAT}, burst ${C62_BASEBAND_DUMP_BURST})")

add_subdirectory(services)
zephyr_library_link_libraries(lsf_client)
zephyr_linker_sources(DATA_SECTIONS lsf_service.ld)

# .psram_dmr: the DMR package's state in PSRAM, an output section of its own so
# that platform_init_csk6() can zero it (the PSRAM region also holds the CSK6
# system heap, so .psram_section cannot be zeroed wholesale).
zephyr_linker_sources(SECTIONS ${CMAKE_CURRENT_LIST_DIR}/psram_dmr.ld)

target_include_directories(app
  PRIVATE
    ${OPENRTX_ROOT}/platform/targets/c62
)

target_compile_definitions(app PRIVATE PLATFORM_C62)

# Activate SWD (Serial Wire Debug) for the C62 platform: frees the SWD pins from
# the speaker GPIO and the display reset line. See C62_ENABLE_SWD users.
#target_compile_definitions(app PRIVATE C62_ENABLE_SWD)

#
# DSP firmware, flashed separately at 0x100000 (dsp_firmware partition). The
# firmware does not boot without it, and it must match the audio code, so the
# build pins one image by hash: by default it is downloaded from OpenRTX's
# lsf-zephyr-sdk, or C62_DSP_FIRMWARE (CMake or environment variable) gives a
# local copy for offline builds. Either way it ends up next to zephyr.bin.
#
set(C62_DSP_FIRMWARE_URL
    "https://raw.githubusercontent.com/OpenRTX/lsf-zephyr-sdk/2ac605527dcde3e90c814bb4175546cc7fbd36da/resources/dsp_firmware.bin")
set(C62_DSP_FIRMWARE_SHA256
    "9579b617f048c646465118f6d0b9e1b765b4d7d811ff9c8649c5754fe45a8903")
set(C62_DSP_FIRMWARE_OUT ${CMAKE_BINARY_DIR}/zephyr/dsp_firmware.bin)

if(NOT DEFINED C62_DSP_FIRMWARE AND DEFINED ENV{C62_DSP_FIRMWARE})
  set(C62_DSP_FIRMWARE $ENV{C62_DSP_FIRMWARE})
endif()

# A relative path is from the OpenRTX root. Check the hash here, as a
# mismatch in file(DOWNLOAD) stops with CMake's own error.
if(C62_DSP_FIRMWARE)
  get_filename_component(c62_dsp_local "${C62_DSP_FIRMWARE}" ABSOLUTE
                         BASE_DIR "${OPENRTX_ROOT}")
  if(EXISTS "${c62_dsp_local}")
    file(SHA256 "${c62_dsp_local}" c62_dsp_local_sha256)
  endif()
  if(NOT "${c62_dsp_local_sha256}" STREQUAL "${C62_DSP_FIRMWARE_SHA256}")
    message(FATAL_ERROR "C62_DSP_FIRMWARE ${c62_dsp_local} is missing or is "
                        "not the pinned DSP firmware (sha256 "
                        "${C62_DSP_FIRMWARE_SHA256}).")
  endif()
  configure_file("${c62_dsp_local}" "${C62_DSP_FIRMWARE_OUT}" COPYONLY)
else()
  # Skipped when the file is already there with the expected hash
  file(DOWNLOAD "${C62_DSP_FIRMWARE_URL}" "${C62_DSP_FIRMWARE_OUT}"
       EXPECTED_HASH SHA256=${C62_DSP_FIRMWARE_SHA256}
       TLS_VERIFY ON
       STATUS c62_dsp_status)
  list(GET c62_dsp_status 0 c62_dsp_error)
  if(c62_dsp_error)
    list(GET c62_dsp_status 1 c62_dsp_message)
    message(FATAL_ERROR "Could not get the C62 DSP firmware from "
                        "${C62_DSP_FIRMWARE_URL} (${c62_dsp_message}). Set "
                        "C62_DSP_FIRMWARE to a local copy with sha256 "
                        "${C62_DSP_FIRMWARE_SHA256}.")
  endif()
endif()

message(STATUS "C62 DSP firmware: ${C62_DSP_FIRMWARE_OUT}, flash at 0x100000")
