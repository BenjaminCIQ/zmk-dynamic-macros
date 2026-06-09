/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Step-0 sanity test (redesign §5, step 0): one trivial test that passes BOTH
 * ways — as Ztest under `west test` and as a standalone host binary. Its only
 * job is to prove the dual-mode loop is wired before any pure-core module
 * (dm_render / slot_store / dm_machine) is extracted.
 *
 * It also serves as the first DECOUPLING PROOF: it includes the pure-core
 * header dm_event.h and asserts the dm_event ABI (8-byte packed), with NO
 * Zephyr include. If a future change makes that header pull in Zephyr, the
 * standalone host compile breaks here and the decoupling regression is caught
 * locally in under a second.
 */

#include <stdint.h>
#include "ztest_shim.h"

#include <zmk-behavior-dynamic-macros/dm_event.h>

ZTEST_SUITE(dm_sanity, NULL, NULL, NULL, NULL, NULL);

/* The harness itself runs. */
ZTEST(dm_sanity, harness_runs) {
    zassert_true(true, "the dual-mode harness executes a test body");
}

/* The pure-core dm_event ABI holds without Zephyr (decoupling proof seed). */
ZTEST(dm_sanity, dm_event_is_8_bytes_packed) {
    zassert_equal(sizeof(struct dm_event), 8, "dm_event must be 8 bytes packed");

    /* Field round-trips through the packed layout the storage format depends on. */
    struct dm_event ev = {
        .usage_page = 0x07,
        .keycode = 0x04,
        .implicit_mods = 0x02,
        .explicit_mods = 0x00,
        .pressed = 1,
        ._reserved = 0,
    };
    zassert_equal(ev.usage_page, 0x07, "usage_page round-trips");
    zassert_equal(ev.keycode, 0x04, "keycode round-trips");
    zassert_equal(ev.pressed, 1, "pressed round-trips");
}
