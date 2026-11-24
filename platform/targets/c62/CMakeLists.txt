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
# C62_DMR_VOCODER: AMBE+2 voice decoder for the DMR receive mode.
# Builds lib/dmr_vocoder (the decoder half of OpenDMR, C only, static state, no
# heap) and links it into the application. OFF by default: it costs 41 KiB of
# flash and 52 KiB of RAM (measured, see docs/DMR_VOCODER.md), and nothing calls
# it until CONFIG_C62_DMR_RX is set.
#
if(CONFIG_C62_DMR_VOCODER)
  add_subdirectory(${OPENRTX_ROOT}/lib/dmr_vocoder ${CMAKE_BINARY_DIR}/dmr_vocoder)
  # Build it with the Zephyr compiler flags (CPU, FPU, ABI, include paths).
  target_compile_options(dmr_vocoder PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_OPTIONS>)
  target_compile_definitions(dmr_vocoder PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>)
  target_include_directories(dmr_vocoder PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
  target_include_directories(dmr_vocoder SYSTEM PRIVATE
    $<TARGET_PROPERTY:zephyr_interface,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
  # Nothing calls the vocoder until the DMR receive mode is merged, and the
  # linker drops an unreferenced static library.  This forces the symbol in
  # anyway so its flash/RAM cost can be measured on the real firmware image;
  # it is a measurement aid, off by default.
  option(C62_DMR_VOCODER_FORCE_LINK
         "Link the vocoder without a caller (size measurement only)" OFF)
  if(C62_DMR_VOCODER_FORCE_LINK)
    # Must precede the library on the link line: -u is position sensitive.
    zephyr_link_libraries(-Wl,-u,dmr_vocoder_decode)
  endif()

  target_link_libraries(app PRIVATE dmr_vocoder)
  target_compile_definitions(app PRIVATE CONFIG_DMR_VOCODER)

  message(STATUS "C62: DMR AMBE+2 vocoder (decode only) enabled"
                 " (force link: ${C62_DMR_VOCODER_FORCE_LINK})")
endif()

#
# C62_DMR_RX: DMR mode. The PHY, the FEC and the burst handling are the C port
# of the host prototype in work/dmr-rx (see docs/DMR_RX.md); voice goes through
# lib/dmr_vocoder, so this option selects C62_DMR_VOCODER.
#
# With CONFIG_DMR_TX the same option also builds the transmit chain (DmrTx.c,
# the encode halves of DmrFec.c and the DmrVocoder.c hook), see docs/DMR_TX.md.
#
# C62_APRS: APRS on the analog FM path (messaging, beacon, station list).
# The decoder is off at run time until it is enabled in the menu: this option
# only decides whether the code is linked in. See docs/APRS.md.
#
#
# CONFIG_MESSAGING: the shared text message store (core/messaging.c): outbox,
# inbox, states and the flash record, used by APRS, DMR SMS and TETRA SDS.  The
# messaging screen of the UI talks to this and nothing else.
#
option(CONFIG_MESSAGING "Shared text message store (APRS, DMR SMS, TETRA SDS)" ON)

if(CONFIG_MESSAGING)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/core/messaging.c
    ${OPENRTX_ROOT}/openrtx/src/core/messaging_store_zephyr.c)
  target_compile_definitions(app PRIVATE CONFIG_MESSAGING)
  message(STATUS "C62: shared message store enabled")
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

  if(CONFIG_MESSAGING)
    # The APRS leg of the message store: queued messages go into its mailbox.
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/openrtx/src/core/messaging_aprs.c)
  endif()
endif()

#
# CONFIG_DMR_SMS: short messages in the DMR mode (protocols/DMR/DmrSms.c).
# A data call carries the text, one burst per TDMA frame; the message store
# (CONFIG_MESSAGING) holds the queue the UI writes to.  Needs CONFIG_DMR_TX to
# transmit and CONFIG_C62_DMR_RX to receive, but compiles with either alone.
#
option(CONFIG_DMR_SMS "DMR short messages (data calls)" ON)

if(CONFIG_C62_DMR_RX)
  target_sources(app PRIVATE
    ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrDefs.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrFec.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrPhy.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrSpeech.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrStatus.c
    ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrVocoder.c
    ${OPENRTX_ROOT}/openrtx/src/rtx/OpMode_DMR.cpp)
  target_compile_definitions(app PRIVATE CONFIG_DMR)
  message(STATUS "C62: DMR mode enabled")

  if(CONFIG_DMR_SMS)
    if(NOT CONFIG_MESSAGING)
      message(FATAL_ERROR "CONFIG_DMR_SMS needs CONFIG_MESSAGING")
    endif()
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrSms.c
      ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrSmsApp.c)
    target_compile_definitions(app PRIVATE CONFIG_DMR_SMS)
    message(STATUS "C62: DMR short messages enabled")
  endif()

  if(CONFIG_DMR_TX)
    target_sources(app PRIVATE
      ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrTx.c
      ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrTxTables.c)
    target_compile_definitions(app PRIVATE CONFIG_DMR_TX)
    message(STATUS "C62: DMR transmit chain enabled")

    #
    # DMR_TX_ENC: the AMBE+2 encoder (lib/dmr_encoder, pure C: the fixed-point
    # analysis of the IMBE encoder plus the OP25 quantiser and Golay).  It
    # overrides the weak dmrVocoder_encode() hook, so with this on the PTT of
    # the DMR mode really transmits.  1.43 M instructions per 20 ms frame,
    # 63 KiB of flash, 5.5 KiB of .bss; see docs/DMR_TX.md.
    #
    if(CONFIG_DMR_TX_ENC)
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
      target_sources(app PRIVATE
        ${OPENRTX_ROOT}/openrtx/src/protocols/DMR/DmrVocoderEncoder.c)
      target_compile_definitions(app PRIVATE CONFIG_DMR_TX_ENC)
      message(STATUS "C62: DMR AMBE+2 encoder enabled (transmit)")
    endif()
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
