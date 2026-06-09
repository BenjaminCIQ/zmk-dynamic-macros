# Ztest build of the pure-core unit tests (redesign §4.1).
#
# Runs under `west test` / Twister as native_sim, alongside the existing
# snapshot suite, so CI sees one unified harness. The SAME test_*.c files are
# also compiled standalone via tests/unit/Makefile for the fast local loop.
#
# host_runner.c is EXCLUDED here: Ztest supplies its own main() and test
# registration; host_runner.c only backs the standalone build.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dm_unit_tests)

# Pure-core headers (dm_event.h, future dm_render.h, …) live in the module's
# include/ tree.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Every test_*.c is a Ztest translation unit. As pure-core modules are
# extracted (steps 1/3/4) add their Zephyr-free .c to this list next to the
# test that drives them.
file(GLOB DM_TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
target_sources(app PRIVATE ${DM_TEST_SRCS})
