/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * US-locale render golden.
 *
 * The token cases here are captured from dm_get_preview_string: the
 * keymap-snapshot test (tests/parity/render/native_sim.keymap) records Ctrl+C
 * then Shift+3 and saves at VERBOSE/US, so the preview walk types the preview;
 * the captured keycode_events.snapshot decodes to:
 *
 *     [DM REC]c#[DM STOP][DM SAVED R0: '<LCTL+C>#']
 *                                       ^^^^^^^^^^  the captured preview
 *
 * So Ctrl+C renders as "<LCTL+C>" and US Shift+3 as "#". These are the parity
 * anchors; dm_render must reproduce them. The decode is cross-checked by the
 * independent first-principles assertions in tests/unit/test_render.c, so the
 * golden stands on the captured snapshot, not on the renderer under test.
 *
 * To extend coverage, add cases to the capture keymap, re-run the snapshot test
 * in CI, and decode the resulting preview here.
 */

#ifndef DM_GOLDEN_US_H
#define DM_GOLDEN_US_H

#define DM_GOLDEN_US_CAPTURED 1

struct dm_golden_entry {
    const char *name;     /* matches a dm_render_corpus[] case name */
    const char *expected; /* old-walk preview string (US locale) */
};

/* Old-walk-verified anchors (captured via the keymap snapshot above). */
static const struct dm_golden_entry dm_golden_us[] = {
    {"ctrl_c",  "<LCTL+C>"}, /* old walk: Ctrl+printable -> token */
    {"shift_3", "#"},        /* old walk: US Shift+3 -> '#' (printable) */
};

#define DM_GOLDEN_US_LEN ((int)(sizeof(dm_golden_us) / sizeof(dm_golden_us[0])))

#endif /* DM_GOLDEN_US_H */
