/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Dual-mode test shim (redesign §4.1).
 *
 * The pure-core unit tests (dm_render, slot_store, dm_machine) are written ONCE
 * and compiled TWO ways:
 *
 *   1. As Ztest under `west test` (one CI harness alongside the snapshot suite).
 *   2. As a standalone host binary built with plain `cc` and no Zephyr at all,
 *      for a sub-second local red-green loop.
 *
 * When ZTEST_SHIM_HOST is defined (the standalone build sets it), this header
 * maps the zassert_* macros the unit tests use onto plain assert()/printf, and
 * provides a tiny test-registration + main() so the same .c file links into a
 * runnable binary. Otherwise it includes the real <zephyr/ztest.h> and is a
 * no-op shim.
 *
 * Keep the mapped surface MINIMAL: add a zassert_* only when a unit test needs
 * it (§6 — "ztest_shim surface grows as the unit tests are written"). Step 0
 * maps just enough for the sanity test.
 */

#ifndef DM_ZTEST_SHIM_H
#define DM_ZTEST_SHIM_H

#ifndef ZTEST_SHIM_HOST
/* ---- Zephyr / Ztest build: defer to the real thing ---------------------- */

#include <zephyr/ztest.h>

#else
/* ---- Standalone host build: no Zephyr ----------------------------------- */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Test registration without Zephyr's linker-section magic.
 *
 * ZTEST(suite, name) { ... } defines a void(void) function and registers it via
 * a constructor so main() can run every test. This mirrors Ztest's
 * suite/test-name shape closely enough that the SAME source compiles both ways.
 */

#define DM_MAX_HOST_TESTS 256

typedef void (*dm_host_test_fn)(void);

struct dm_host_test {
    const char *suite;
    const char *name;
    dm_host_test_fn fn;
};

extern struct dm_host_test dm_host_tests[DM_MAX_HOST_TESTS];
extern int dm_host_test_count;
extern int dm_host_failures;
extern int dm_host_skipped; /* set by ztest_test_skip() for the current test */

static inline void dm_host_register(const char *suite, const char *name, dm_host_test_fn fn) {
    if (dm_host_test_count < DM_MAX_HOST_TESTS) {
        dm_host_tests[dm_host_test_count].suite = suite;
        dm_host_tests[dm_host_test_count].name = name;
        dm_host_tests[dm_host_test_count].fn = fn;
        dm_host_test_count++;
    }
}

/*
 * Auto-registration so the SAME test body links into a runnable binary under
 * both GCC/Clang and MSVC, with no per-test boilerplate:
 *   - GCC/Clang: __attribute__((constructor)) runs the registrar at startup.
 *   - MSVC: place a pointer to the registrar in the .CRT$XCU section, which the
 *     CRT walks before main() — the portable MSVC equivalent of a constructor.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define DM_HOST_CTOR(fn)                                                                            \
    static void fn(void);                                                                           \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr)(void) = fn;                                   \
    static void fn(void)
#else
#define DM_HOST_CTOR(fn) __attribute__((constructor)) static void fn(void)
#endif

#define ZTEST(suite, name)                                                                         \
    static void suite##_##name##_impl(void);                                                       \
    DM_HOST_CTOR(suite##_##name##_reg) {                                                            \
        dm_host_register(#suite, #name, suite##_##name##_impl);                                     \
    }                                                                                              \
    static void suite##_##name##_impl(void)

/* ZTEST_SUITE in real Ztest declares a suite; on host it is a harmless no-op. */
#define ZTEST_SUITE(name, ...) struct dm_host_unused_##name

/* Skip the current test (e.g. a golden not yet captured). On host this prints a
 * note and returns from the test body without counting a failure. */
#define ztest_test_skip()                                                                          \
    do {                                                                                           \
        dm_host_skipped = 1;                                                                        \
        return;                                                                                    \
    } while (0)

/* ---- Assertion surface (minimal; grow as tests need it) ----------------- */

#define DM_HOST_FAIL(fmt, ...)                                                                     \
    do {                                                                                           \
        fprintf(stderr, "  ASSERT FAILED %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);    \
        dm_host_failures++;                                                                        \
        return; /* abort this test, keep running the rest */                                       \
    } while (0)

#define zassert_true(cond, ...)                                                                    \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            DM_HOST_FAIL("expected true: %s", #cond);                                              \
        }                                                                                          \
    } while (0)

#define zassert_false(cond, ...)                                                                   \
    do {                                                                                           \
        if (cond) {                                                                                \
            DM_HOST_FAIL("expected false: %s", #cond);                                             \
        }                                                                                          \
    } while (0)

#define zassert_equal(a, b, ...)                                                                   \
    do {                                                                                           \
        long _va = (long)(a);                                                                      \
        long _vb = (long)(b);                                                                      \
        if (_va != _vb) {                                                                          \
            DM_HOST_FAIL("expected %s == %s (%ld != %ld)", #a, #b, _va, _vb);                       \
        }                                                                                          \
    } while (0)

#define zassert_str_equal(a, b, ...)                                                               \
    do {                                                                                           \
        if (strcmp((a), (b)) != 0) {                                                               \
            DM_HOST_FAIL("expected \"%s\" == \"%s\"", (a), (b));                                    \
        }                                                                                          \
    } while (0)

#endif /* ZTEST_SHIM_HOST */

#endif /* DM_ZTEST_SHIM_H */
