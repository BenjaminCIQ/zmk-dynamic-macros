# Render parity CAPTURE test (redesign §5.2), native_sim Ztest.
#
# Links the module (via the Zephyr module mechanism — the repo's zephyr/module.yml
# adds src/) and runs the live old dm_get_preview_string over the shared corpus,
# printing the golden strings. The host parity test (tests/unit/test_render_parity.c)
# then asserts dm_render matches that recorded golden.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dm_render_parity_capture)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include   # module public headers
    ${CMAKE_CURRENT_SOURCE_DIR}/..)                # render_corpus.h

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/capture_test.c)
