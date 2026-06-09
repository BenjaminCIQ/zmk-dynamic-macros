/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Standalone host runner (redesign §4.1).
 *
 * Provides the storage for the test registry declared in ztest_shim.h and a
 * main() that runs every ZTEST-registered test, printing a summary. Compiled
 * ONLY in the standalone host build (ZTEST_SHIM_HOST); under Zephyr/Ztest the
 * real harness supplies main(), so this file is excluded there.
 */

#include <stdbool.h>

#include "ztest_shim.h"

struct dm_host_test dm_host_tests[DM_MAX_HOST_TESTS];
int dm_host_test_count = 0;
int dm_host_failures = 0;

int main(void) {
    int failed_tests = 0;

    printf("dm unit tests (host build) — %d test(s)\n", dm_host_test_count);

    for (int i = 0; i < dm_host_test_count; i++) {
        int before = dm_host_failures;
        dm_host_tests[i].fn();
        bool ok = (dm_host_failures == before);
        printf("  [%s] %s.%s\n", ok ? "PASS" : "FAIL", dm_host_tests[i].suite,
               dm_host_tests[i].name);
        if (!ok) {
            failed_tests++;
        }
    }

    printf("%s — %d passed, %d failed\n", failed_tests == 0 ? "OK" : "FAILED",
           dm_host_test_count - failed_tests, failed_tests);

    return failed_tests == 0 ? 0 : 1;
}
