# Ztest build of the pure-core unit tests (redesign §4.1), run by the dedicated
# `west twister` CI job (.github/workflows/unit-tests.yml) on native_sim. The
# repo's snapshot job (ZMK run-test.sh) cannot discover these. The SAME test_*.c
# files are also compiled standalone via tests/unit/Makefile / run-host.ps1 for
# the fast local loop.
#
# host_runner.c is EXCLUDED here: Ztest supplies its own main() and test
# registration; host_runner.c only backs the standalone build.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dm_unit_tests)

# Module public headers (dm_event.h, dm_render.h, …), the test dir itself, and
# the parity corpus + golden tables.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../parity)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../parity/render)

# Every test_*.c is a Ztest translation unit.
file(GLOB DM_TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
target_sources(app PRIVATE ${DM_TEST_SRCS})

# Pure-core module sources under test, compiled DIRECTLY (not via the module's
# Kconfig-gated build) — they are Zephyr-free and the unit tests link them
# regardless of whether the behavior is enabled in this build. Add module .c
# here as steps 1/3/4 extract them (mirrors the host Makefile's source list).
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/dm_render.c)
