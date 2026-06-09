/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Render parity CAPTURE test (redesign §5.2, native_sim).
 *
 * Runs the LIVE OLD walk (dm_get_preview_string) over the shared render corpus
 * and PRINTS the golden strings, so the golden table consumed by the fast host
 * parity test is recorded FROM THE OLD CODE, not hand-written. Run once per
 * locale build; transcribe its output into tests/parity/render/golden_<loc>.h.
 *
 * It also asserts internal consistency (every case renders without truncation),
 * but its real product is the printed golden, captured from CI logs.
 *
 * Locale is compile-time in the old code (DM_LOCALE Kconfig), so this test
 * captures exactly one locale per build; the prj.conf selects which.
 *
 * To populate a slot for the old walk we reach the behavior instance's data
 * directly — acceptable in a parity test whose whole job is to observe the old
 * internals. This mirrors how the old code itself owns slots[].
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_internal.h>
#include <zmk-behavior-dynamic-macros/events/dynamic_macro_state_changed.h>

#include "render_corpus.h"

/* The single behavior instance (single-instance per ADR-0002). */
extern const struct device *dm_devices[];
extern const size_t dm_devices_len;

static struct behavior_dynamic_macro_data *parity_data(void) {
    zassert_true(dm_devices_len > 0, "no dynamic-macro instance");
    return dm_devices[0]->data;
}

/* Load one corpus case into slot 0 so the old walk can render it. */
static void load_slot0(struct behavior_dynamic_macro_data *data, const struct dm_parity_case *c) {
    memset(&data->slots[0], 0, sizeof(data->slots[0]));
    zassert_true(c->count <= MAX_EVENTS, "case %s exceeds MAX_EVENTS", c->name);
    data->slots[0].event_count = c->count;
    memcpy(data->slots[0].events, c->events, c->count * sizeof(struct dm_event));
    atomic_clear_bit(data->pending_delete, 0);
}

ZTEST_SUITE(render_parity_capture, NULL, NULL, NULL, NULL, NULL);

ZTEST(render_parity_capture, emit_golden) {
    struct behavior_dynamic_macro_data *data = parity_data();
    char buf[256];

    printk("\n===DM_RENDER_GOLDEN_BEGIN locale=%d===\n", DM_LOCALE);
    for (int i = 0; i < DM_RENDER_CORPUS_LEN; i++) {
        const struct dm_parity_case *c = &dm_render_corpus[i];
        load_slot0(data, c);

        int n = dm_get_preview_string(0, buf, sizeof(buf));
        zassert_true(n >= 0 && n < (int)sizeof(buf), "case %s: bad length %d", c->name, n);

        /* Machine-parseable line: NAME<TAB>"string". The host golden is
         * transcribed from these. */
        printk("GOLDEN\t%s\t\"%s\"\n", c->name, buf);
    }
    printk("===DM_RENDER_GOLDEN_END===\n");
}
