/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Red tests for dm_render (redesign §4.2), written BEFORE the implementation.
 * They drive the interface: feed a dm_event[] + locale, assert the rendered
 * preview string. A buffer sink (the simplest adapter) collects emit_char into
 * a fixed buffer.
 *
 * Ported invariants under test:
 *   - Ctrl+printable -> <LCTL+C> token, not a bare char   (49c4f1a)
 *   - UK Shift+3 -> token, NOT the GBP char (non-ASCII)    (86993af)
 *   - DE/FR plain locales emit only letters/digits/space
 *   - literal printable text stays literal
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ztest_shim.h"

#include <zmk-behavior-dynamic-macros/dm_event.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>

/* HID usage page for the keyboard (matches dt-bindings hid_usage_pages). */
#define HID_USAGE_KEY 0x07

/* Modifier bit positions used in dm_event mod masks. */
#define MOD_LCTL 0x01
#define MOD_LSFT 0x02

/* ---- buffer sink ---------------------------------------------------------- */

struct buf_sink {
    char buf[256];
    size_t pos;
};

static void buf_emit_char(void *ctx, char c) {
    struct buf_sink *s = ctx;
    if (s->pos + 1 < sizeof(s->buf)) {
        s->buf[s->pos++] = c;
        s->buf[s->pos] = '\0';
    }
}

static bool buf_space_for(void *ctx, uint8_t n) {
    struct buf_sink *s = ctx;
    return s->pos + n + 1 < sizeof(s->buf);
}

static void render_into(struct buf_sink *s, const struct dm_event *events, uint32_t count,
                        dm_locale locale) {
    memset(s, 0, sizeof(*s));
    dm_sink sink = {.emit_char = buf_emit_char, .space_for = buf_space_for, .ctx = s};
    dm_render_slot_view view = {.event_count = count, .events = events};
    dm_render_slot(&view, locale, &sink);
}

/* ---- event builders ------------------------------------------------------- */

static struct dm_event key(uint16_t keycode, uint8_t implicit, uint8_t explicit_mods,
                           uint8_t pressed) {
    return (struct dm_event){
        .usage_page = HID_USAGE_KEY,
        .keycode = keycode,
        .implicit_mods = implicit,
        .explicit_mods = explicit_mods,
        .pressed = pressed,
        ._reserved = 0,
    };
}

/* Keycodes (HID): a=0x04, c=0x06; digit '3'=0x20; comma=0x36; modifiers LCTL=0xE0 LSFT=0xE1. */
#define KC_A 0x04
#define KC_C 0x06
#define KC_3 0x20
#define KC_COMMA 0x36
#define KC_LCTL 0xE0
#define KC_LSFT 0xE1

ZTEST_SUITE(dm_render, NULL, NULL, NULL, NULL, NULL);

/* Literal printable text stays literal: "ac" on US. */
ZTEST(dm_render, literal_text_us) {
    struct dm_event evs[] = {
        key(KC_A, 0, 0, 1), key(KC_A, 0, 0, 0),
        key(KC_C, 0, 0, 1), key(KC_C, 0, 0, 0),
    };
    struct buf_sink s;
    render_into(&s, evs, 4, DM_LOCALE_US);
    zassert_str_equal(s.buf, "ac", "plain letters render literally");
}

/* Ctrl+C becomes a token, not a bare 'c' (ports 49c4f1a). */
ZTEST(dm_render, ctrl_printable_is_token_us) {
    struct dm_event evs[] = {
        key(KC_LCTL, 0, 0, 1),           /* Ctrl down */
        key(KC_C, 0, MOD_LCTL, 1),       /* C with explicit Ctrl */
        key(KC_C, 0, MOD_LCTL, 0),
        key(KC_LCTL, 0, 0, 0),           /* Ctrl up */
    };
    struct buf_sink s;
    render_into(&s, evs, 4, DM_LOCALE_US);
    zassert_str_equal(s.buf, "<LCTL+C>", "Ctrl+printable renders as a token");
}

/* UK Shift+3 is GBP (non-ASCII) -> token, never a wrong char (ports 86993af). */
ZTEST(dm_render, uk_shift3_is_token_not_gbp) {
    struct dm_event evs[] = {
        key(KC_LSFT, 0, 0, 1),
        key(KC_3, 0, MOD_LSFT, 1),
        key(KC_3, 0, MOD_LSFT, 0),
        key(KC_LSFT, 0, 0, 0),
    };
    struct buf_sink s;
    render_into(&s, evs, 4, DM_LOCALE_UK);
    zassert_str_equal(s.buf, "<LSFT+3>", "UK Shift+3 (GBP) renders as a token, not a char");
}

/* US Shift+3 IS '#': a printable ASCII shifted char stays literal. */
ZTEST(dm_render, us_shift3_is_hash) {
    struct dm_event evs[] = {
        key(KC_LSFT, 0, 0, 1),
        key(KC_3, 0, MOD_LSFT, 1),
        key(KC_3, 0, MOD_LSFT, 0),
        key(KC_LSFT, 0, 0, 0),
    };
    struct buf_sink s;
    render_into(&s, evs, 4, DM_LOCALE_US);
    zassert_str_equal(s.buf, "#", "US Shift+3 is the printable '#'");
}

/* DE plain locale: letters render literally (no punctuation machinery). */
ZTEST(dm_render, de_plain_letters) {
    struct dm_event evs[] = {
        key(KC_A, 0, 0, 1), key(KC_A, 0, 0, 0),
    };
    struct buf_sink s;
    render_into(&s, evs, 2, DM_LOCALE_DE);
    zassert_str_equal(s.buf, "a", "DE plain locale emits letters");
}

/* DE plain locale: digits still render literally (digits are not punctuation). */
ZTEST(dm_render, de_plain_digit) {
    struct dm_event evs[] = {
        key(KC_3, 0, 0, 1), key(KC_3, 0, 0, 0),
    };
    struct buf_sink s;
    render_into(&s, evs, 2, DM_LOCALE_DE);
    zassert_str_equal(s.buf, "3", "DE plain locale emits digits literally");
}

/*
 * DE plain locale: a punctuation key renders via the <TOKEN> path, not as a
 * literal — plain previews are letters/digits/space only (§2.3, step-2 decision).
 * A SHIFTED comma proves the token path was taken: on US it would be the literal
 * '<', but on DE plain it goes through emit_token as "LSFT ," (plain separator =
 * space, no <…> delimiters), which no literal path could produce. */
ZTEST(dm_render, de_plain_shifted_punctuation_is_token) {
    struct dm_event evs[] = {
        key(KC_LSFT, 0, 0, 1),
        key(KC_COMMA, 0, MOD_LSFT, 1),
        key(KC_COMMA, 0, MOD_LSFT, 0),
        key(KC_LSFT, 0, 0, 0),
    };
    struct buf_sink s;
    render_into(&s, evs, 4, DM_LOCALE_DE);
    zassert_str_equal(s.buf, "LSFT ,", "DE plain shifted comma -> token path, not literal '<'");
}
