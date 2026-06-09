/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Shared render parity corpus (redesign §5.2).
 *
 * A fixed set of dm_event sequences fed identically into:
 *   - the OLD walk (dm_get_preview_string) by the native_sim capture test, to
 *     RECORD the golden strings from the live old code; and
 *   - the NEW dm_render (buffer sink) by the host parity test, to ASSERT it
 *     matches the recorded golden.
 *
 * PURE: only stdint + dm_event. No Zephyr — so both the host loop and the
 * native_sim test consume identical inputs. Add cases here as render edge
 * behavior is discovered; re-run the capture test to refresh the golden.
 */

#ifndef DM_RENDER_CORPUS_H
#define DM_RENDER_CORPUS_H

#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_event.h>

#define DM_PARITY_HID_USAGE_KEY      0x07
#define DM_PARITY_HID_USAGE_BUTTON   0x09
#define DM_PARITY_HID_USAGE_CONSUMER 0x0C

/* Modifier mask bits (match dm_event implicit/explicit mod masks). */
#define DM_PARITY_MOD_LCTL 0x01
#define DM_PARITY_MOD_LSFT 0x02
#define DM_PARITY_MOD_LALT 0x04
#define DM_PARITY_MOD_LGUI 0x08

/* HID keycodes used by the corpus. */
#define DM_PARITY_KC_A    0x04
#define DM_PARITY_KC_C    0x06
#define DM_PARITY_KC_Z    0x1D
#define DM_PARITY_KC_1    0x1E
#define DM_PARITY_KC_3    0x20
#define DM_PARITY_KC_SPC  0x2C
#define DM_PARITY_KC_RET  0x28
#define DM_PARITY_KC_LCTL 0xE0
#define DM_PARITY_KC_LSFT 0xE1

#define DM_PARITY_EV(pg, kc, imp, exp, pr)                                                         \
    {.usage_page = (pg), .keycode = (kc), .implicit_mods = (imp), .explicit_mods = (exp),          \
     .pressed = (pr), ._reserved = 0}

#define KEY(kc, exp, pr) DM_PARITY_EV(DM_PARITY_HID_USAGE_KEY, (kc), 0, (exp), (pr))

struct dm_parity_case {
    const char *name;
    const struct dm_event *events;
    uint32_t count;
};

/* ---- cases ---------------------------------------------------------------- */

/* "ac" — plain literal letters. */
static const struct dm_event dm_corpus_literal[] = {
    KEY(DM_PARITY_KC_A, 0, 1), KEY(DM_PARITY_KC_A, 0, 0),
    KEY(DM_PARITY_KC_C, 0, 1), KEY(DM_PARITY_KC_C, 0, 0),
};

/* Ctrl+C — non-shift modifier on a printable -> token. */
static const struct dm_event dm_corpus_ctrl_c[] = {
    KEY(DM_PARITY_KC_LCTL, 0, 1),
    KEY(DM_PARITY_KC_C, DM_PARITY_MOD_LCTL, 1),
    KEY(DM_PARITY_KC_C, DM_PARITY_MOD_LCTL, 0),
    KEY(DM_PARITY_KC_LCTL, 0, 0),
};

/* Shift+3 — locale-divergent: '#' on US, GBP (token) on UK. */
static const struct dm_event dm_corpus_shift_3[] = {
    KEY(DM_PARITY_KC_LSFT, 0, 1),
    KEY(DM_PARITY_KC_3, DM_PARITY_MOD_LSFT, 1),
    KEY(DM_PARITY_KC_3, DM_PARITY_MOD_LSFT, 0),
    KEY(DM_PARITY_KC_LSFT, 0, 0),
};

/* Space then Return — RET is non-printable -> <RET> token; space is literal. */
static const struct dm_event dm_corpus_space_ret[] = {
    KEY(DM_PARITY_KC_SPC, 0, 1), KEY(DM_PARITY_KC_SPC, 0, 0),
    KEY(DM_PARITY_KC_RET, 0, 1), KEY(DM_PARITY_KC_RET, 0, 0),
};

/* Ctrl+Alt+Z — multi-modifier token. */
static const struct dm_event dm_corpus_ctrl_alt_z[] = {
    KEY(DM_PARITY_KC_LCTL, 0, 1),
    KEY(DM_PARITY_KC_Z, DM_PARITY_MOD_LCTL | DM_PARITY_MOD_LALT, 1),
    KEY(DM_PARITY_KC_Z, DM_PARITY_MOD_LCTL | DM_PARITY_MOD_LALT, 0),
    KEY(DM_PARITY_KC_LCTL, 0, 0),
};

/* Mouse-left click — non-keyboard usage page -> token. */
static const struct dm_event dm_corpus_mouse_left[] = {
    DM_PARITY_EV(DM_PARITY_HID_USAGE_BUTTON, 0x01, 0, 0, 1),
    DM_PARITY_EV(DM_PARITY_HID_USAGE_BUTTON, 0x01, 0, 0, 0),
};

#define DM_PARITY_CASE(ident, label) {.name = (label), .events = (ident), .count = (uint32_t)(sizeof(ident) / sizeof((ident)[0]))}

static const struct dm_parity_case dm_render_corpus[] = {
    DM_PARITY_CASE(dm_corpus_literal,    "literal_ac"),
    DM_PARITY_CASE(dm_corpus_ctrl_c,     "ctrl_c"),
    DM_PARITY_CASE(dm_corpus_shift_3,    "shift_3"),
    DM_PARITY_CASE(dm_corpus_space_ret,  "space_ret"),
    DM_PARITY_CASE(dm_corpus_ctrl_alt_z, "ctrl_alt_z"),
    DM_PARITY_CASE(dm_corpus_mouse_left, "mouse_left"),
};

#define DM_RENDER_CORPUS_LEN ((int)(sizeof(dm_render_corpus) / sizeof(dm_render_corpus[0])))

#endif /* DM_RENDER_CORPUS_H */
