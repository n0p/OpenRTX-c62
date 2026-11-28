# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The interrupt-context harness (tests/qemu/m17_isr.c), built as a Zephyr
# application for the Cortex-M33 the radio uses.  The sources under test are
# the firmware's own: the audio path table, unmodified.
#
# Built by scripts/m17_isr.sh, which passes TEST_NAME and the variant.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(m17_isr)

# The harness itself, plus the board stub it needs.
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../m17_isr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../m17_isr_audio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../m17_isr_board.cpp
)

# The code under test: the firmware's own audio path table.
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../openrtx/src/core/audio_path.cpp
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../openrtx/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Take over k_mutex_lock(): the wrapper in tests/qemu/m17_isr.c reports the
# caller when the lock is asked for from an interrupt, and then calls through.
# k_mutex_lock() itself is a static inline around z_impl_k_mutex_lock().
# "app" is an interface library in Zephyr: link options put on it do not reach
# the link.  zephyr_interface is what every Zephyr target links with.
target_link_options(zephyr_interface INTERFACE -Wl,--wrap=z_impl_k_mutex_lock)

# Which behaviour is under test, and how the interrupt is raised.  Plain
# compile definitions, so the app needs no Kconfig of its own.
if(NOT DEFINED TEST_NAME)
  set(TEST_NAME mutation)
endif()
if(NOT DEFINED TEST_TRIGGER)
  set(TEST_TRIGGER irq)
endif()

string(TOUPPER "${TEST_NAME}" TEST_UPPER)
target_compile_definitions(app PRIVATE
    TEST_${TEST_UPPER}
)
if(TEST_TRIGGER STREQUAL "timer")
  target_compile_definitions(app PRIVATE TEST_TRIGGER_TIMER)
endif()
message(STATUS "m17_isr: test ${TEST_NAME}, trigger ${TEST_TRIGGER}")
