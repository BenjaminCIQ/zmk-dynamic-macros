/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests for dm_query — the pure widget-preview projection. Pins that the buffer
 * sink reproduces dm_render's output, truncates honestly (stop-at-first-non-fit,
 * no silently-missing middle token), and falls back to "(N events)" when typing
 * is compiled out.
 */

#include "ztest_shim.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_query.h>

static struct dm_event key(uint16_t kc, uint8_t imp, uint8_t exp, uint8_t pressed) {
    struct dm_event e = {0};
    e.usage_page = 0x07;
    e.keycode = kc;
    e.implicit_mods = imp;
    e.explicit_mods = exp;
    e.pressed = pressed;
    return e;
}

ZTEST(dm_query, literal_preview_us) {
    struct dm_event evs[] = {
        key(0x04, 0, 0, 1), key(0x04, 0, 0, 0), /* a */
        key(0x05, 0, 0, 1), key(0x05, 0, 0, 0), /* b */
    };
    dm_render_slot_view view = {.event_count = 4, .events = evs};
    char buf[64];
    int n = dm_query_preview_string(&view, DM_LOCALE_US, 2, true, buf, sizeof(buf));
    zassert_str_equal(buf, "ab", NULL);
    zassert_equal(n, 2, NULL);
}

ZTEST(dm_query, ctrl_token_preview_us) {
    struct dm_event evs[] = {
        key(0xE0, 0, 0, 1),
        key(0x06, 0, 0x01, 1),
        key(0x06, 0, 0x01, 0),
        key(0xE0, 0, 0, 0),
    };
    dm_render_slot_view view = {.event_count = 4, .events = evs};
    char buf[64];
    dm_query_preview_string(&view, DM_LOCALE_US, 1, true, buf, sizeof(buf));
    zassert_str_equal(buf, "<LCTL+C>", NULL);
}

/* Honest truncation: a buffer too small for the second token stops at the first,
 * never showing a sequence with a silently-missing middle. */
ZTEST(dm_query, truncates_at_first_non_fit) {
    struct dm_event evs[] = {
        key(0x04, 0, 0, 1), key(0x04, 0, 0, 0), /* a (literal, 1 char) */
        key(0xE0, 0, 0, 1),                     /* LCTL down */
        key(0x05, 0, 0x01, 1),                  /* Ctrl+B -> <LCTL+B> token (8 chars) */
        key(0x05, 0, 0x01, 0),
        key(0xE0, 0, 0, 0),
        key(0x06, 0, 0, 1), key(0x06, 0, 0, 0), /* c (literal) */
    };
    dm_render_slot_view view = {.event_count = 8, .events = evs};
    /* room for "a" + a few chars but not the whole token -> must stop after "a",
     * NOT skip the token and append "c". */
    char buf[5]; /* holds up to 4 chars + NUL */
    dm_query_preview_string(&view, DM_LOCALE_US, 3, true, buf, sizeof(buf));
    zassert_str_equal(buf, "a", NULL);
}

ZTEST(dm_query, events_fallback_when_typing_disabled) {
    char buf[32];
    int n = dm_query_preview_string(NULL, DM_LOCALE_US, 5, false, buf, sizeof(buf));
    zassert_str_equal(buf, "(5 events)", NULL);
    zassert_equal(n, 10, NULL);
}

ZTEST(dm_query, empty_view_is_empty_string) {
    dm_render_slot_view view = {.event_count = 0, .events = NULL};
    char buf[16];
    int n = dm_query_preview_string(&view, DM_LOCALE_US, 0, true, buf, sizeof(buf));
    zassert_str_equal(buf, "", NULL);
    zassert_equal(n, 0, NULL);
}

ZTEST(dm_query, zero_len_buffer_is_safe) {
    dm_render_slot_view view = {.event_count = 0, .events = NULL};
    char buf[1] = {'x'};
    int n = dm_query_preview_string(&view, DM_LOCALE_US, 0, true, buf, 0);
    zassert_equal(n, 0, NULL);
}
