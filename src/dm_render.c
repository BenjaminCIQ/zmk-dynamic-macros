/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_render — pure preview renderer (redesign §2.3, rewrite steps 1–2).
 *
 * One event-walk, emitting to an abstract char sink. PURE: no Zephyr, no I/O,
 * no global state. This is the single home for the replayable-vs-token decision
 * and all token formatting, so the live-typing preview and the
 * dm_get_preview_string query API cannot disagree.
 *
 * STEP 1 ported the existing dm_feedback.c walk (is_replayable_event /
 * printable_char_for_keycode / render_action_token) into one pure routine behind
 * the sink, proven byte-identical to the live old walk (parity test).
 *
 * STEP 2 (this file) replaced the per-locale #if/switch ladders in
 * printable_char_for_keycode with static const data tables (one dm_keymap per
 * locale; see below). Behavior is identical for US/UK; for the plain locales
 * (DE/FR) it is deliberately tightened to letters/digits/space only — the old
 * code fell through to the US punctuation branch, but the encoder never emits
 * punctuation on a plain locale, so this is unobservable through real recording
 * and brings the renderer in line with §2.3's "plain previews are letters,
 * digits, space". See tests de_plain_* for the pinned new behavior.
 */

#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_render.h>

/* HID usage page for keyboard keys (mirrors dt-bindings hid_usage_pages.h, kept
 * local so the renderer stays Zephyr-free). */
#define DM_HID_USAGE_KEY 0x07

/* Shift bits within a HID modifier mask (LSHIFT | RSHIFT). */
#define DM_MOD_SHIFT_MASK     (0x02 | 0x20)
#define DM_MOD_NON_SHIFT_MASK (~DM_MOD_SHIFT_MASK & 0xFF)

/*
 * Locale → character mapping as static const data (redesign §2.3, step 2),
 * replacing the old printable_char_for_keycode() #if/switch ladders. Each row
 * inverts one HID keycode to the {unshifted, shifted} characters it produces on
 * a layout. A '\0' in either slot means "this key produces no printable ASCII
 * glyph here (e.g. UK Shift+3 = GBP) — render a <TOKEN> instead", which is how
 * the old code's `return false` is encoded in the table.
 *
 * Letters (0x04–0x1D) stay algorithmic — a dense arithmetic range (`'a'+offset`)
 * with no per-locale variance, so a table would only obscure them. Everything
 * that USED to branch on DM_LOCALE — the number row (shared digit rows + shifted
 * symbols) and all punctuation — is data here.
 *
 * Tables are keyed by keycode via a small linear scan; the punctuation set is
 * tiny (≤16 rows) so a scan beats a 256-entry sparse array on flash with no
 * measurable cycle cost on the preview path.
 */
typedef struct {
    uint8_t keycode;
    char    normal;  /* '\0' => not printable (token) */
    char    shifted; /* '\0' => not printable (token) */
} dm_keymap_row;

/* Number row (0x1E–0x27): unshifted digit + US-style shifted symbol. Shared by
 * every locale's table — digits render on all layouts; only punctuation varies.
 * (UK overrides two of these rows; see keymap_uk.) */
#define DM_KEYMAP_DIGITS                                                                  \
    {0x1E, '1', '!'}, {0x1F, '2', '@'}, {0x20, '3', '#'}, {0x21, '4', '$'},               \
    {0x22, '5', '%'}, {0x23, '6', '^'}, {0x24, '7', '&'}, {0x25, '8', '*'},               \
    {0x26, '9', '('}, {0x27, '0', ')'}

/* US: digits + the full ASCII punctuation set. */
static const dm_keymap_row keymap_us[] = {
    DM_KEYMAP_DIGITS,
    {0x2C, ' ', ' '}, {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'},
    {0x30, ']', '}'}, {0x31, '\\', '|'}, {0x33, ';', ':'}, {0x34, '\'', '"'},
    {0x35, '`', '~'}, {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
};

/* UK diverges from US on the number row (Shift+2 = ", Shift+3 = GBP → token)
 * and on the ISO-layout punctuation keys (0x32 #/~, 0x34 '/@, 0x35 `/¬, 0x64
 * \/|). 0x31 is not emitted by the UK encoder, so it is absent → token. */
static const dm_keymap_row keymap_uk[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '"'}, {0x20, '3', 0 /* GBP */}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '^'}, {0x24, '7', '&'}, {0x25, '8', '*'},
    {0x26, '9', '('}, {0x27, '0', ')'},
    {0x2C, ' ', ' '}, {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'},
    {0x30, ']', '}'}, {0x32, '#', '~'}, {0x33, ';', ':'}, {0x34, '\'', '@'},
    {0x35, '`', 0 /* NOT sign */}, {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
    {0x64, '\\', '|'},
};

/* Plain locales (DE/FR): digits + space only, no punctuation inversion. A
 * recorded punctuation key therefore falls through to a <TOKEN> (§2.3 — plain
 * previews are letters/digits/space). */
static const dm_keymap_row keymap_plain[] = {
    DM_KEYMAP_DIGITS,
    {0x2C, ' ', ' '},
};

typedef struct {
    const dm_keymap_row *rows;
    uint8_t              len;
    bool                 plain; /* style/spacing variant (token delimiters) */
} dm_keymap;

#define DM_KEYMAP_LEN(t) ((uint8_t)(sizeof(t) / sizeof((t)[0])))

static const dm_keymap keymaps[] = {
    [DM_LOCALE_US] = {keymap_us, DM_KEYMAP_LEN(keymap_us), false},
    [DM_LOCALE_UK] = {keymap_uk, DM_KEYMAP_LEN(keymap_uk), false},
    [DM_LOCALE_DE] = {keymap_plain, DM_KEYMAP_LEN(keymap_plain), true},
    [DM_LOCALE_FR] = {keymap_plain, DM_KEYMAP_LEN(keymap_plain), true},
};

static bool locale_is_plain(dm_locale locale) {
    return keymaps[locale].plain;
}

/*
 * Map a HID keycode (+ shift) back to the printable character it produces on the
 * given locale, via the static const tables above. Returns false when the key
 * produces a non-ASCII glyph on that layout (e.g. UK Shift+3 = GBP) so the
 * caller renders a <TOKEN> instead of a wrong character.
 */
static bool printable_char_for_keycode(dm_locale locale, uint32_t keycode, bool shifted,
                                       char *out) {
    if (keycode >= 0x04 && keycode <= 0x1D) {
        *out = (shifted ? 'A' : 'a') + (keycode - 0x04);
        return true;
    }
    const dm_keymap *km = &keymaps[locale];
    for (uint8_t i = 0; i < km->len; i++) {
        if (km->rows[i].keycode == keycode) {
            char c = shifted ? km->rows[i].shifted : km->rows[i].normal;
            if (c == '\0') {
                return false; /* non-ASCII glyph on this layout -> token */
            }
            *out = c;
            return true;
        }
    }
    return false;
}

static bool is_modifier_key(uint16_t usage_page, uint32_t keycode) {
    return usage_page == DM_HID_USAGE_KEY && keycode >= 0xE0 && keycode <= 0xE7;
}

/*
 * True if the event is a plain (no non-shift modifier) printable key that can be
 * shown as a literal character rather than a <TOKEN>. Ported from
 * is_replayable_event(). active_mods are the modifiers held by prior events.
 */
static bool is_replayable(dm_locale locale, const struct dm_event *ev, uint8_t active_mods) {
    if (ev->usage_page != DM_HID_USAGE_KEY) {
        return false;
    }
    uint8_t mods = active_mods | ev->implicit_mods | ev->explicit_mods;
    if (mods & DM_MOD_NON_SHIFT_MASK) {
        return false;
    }
    char dummy;
    return printable_char_for_keycode(locale, ev->keycode, (mods & DM_MOD_SHIFT_MASK) != 0,
                                      &dummy);
}

/* Action names for the <TOKEN> form. Ported from action_name()/keyboard_action_name(). */
static const char *keyboard_action_name(uint32_t keycode) {
    if (keycode >= 0x04 && keycode <= 0x1D) {
        static const char *letters[] = {
            "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
            "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
        };
        return letters[keycode - 0x04];
    }
    if (keycode >= 0x1E && keycode <= 0x27) {
        static const char *numbers[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
        return numbers[keycode - 0x1E];
    }
    switch (keycode) {
    case 0x28: return "RET";
    case 0x29: return "ESC";
    case 0x2A: return "BSPC";
    case 0x2B: return "TAB";
    case 0x2C: return "SPC";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "CAPS";
    case 0x3A: return "F1";  case 0x3B: return "F2";  case 0x3C: return "F3";
    case 0x3D: return "F4";  case 0x3E: return "F5";  case 0x3F: return "F6";
    case 0x40: return "F7";  case 0x41: return "F8";  case 0x42: return "F9";
    case 0x43: return "F10"; case 0x44: return "F11"; case 0x45: return "F12";
    case 0x46: return "PSCRN"; case 0x47: return "SLCK"; case 0x48: return "PAUSE";
    case 0x49: return "INS"; case 0x4A: return "HOME"; case 0x4B: return "PGUP";
    case 0x4C: return "DEL"; case 0x4D: return "END";  case 0x4E: return "PGDN";
    case 0x4F: return "RIGHT"; case 0x50: return "LEFT"; case 0x51: return "DOWN";
    case 0x52: return "UP";
    default:   return "KEY";
    }
}

static const char *action_name(uint16_t usage_page, uint32_t keycode) {
    if (usage_page == DM_HID_USAGE_KEY) {
        return keyboard_action_name(keycode);
    }
    /* Mouse buttons / consumer pages: mirror dm_feedback.c action_name(). */
    if (usage_page == 0x09 /* HID_USAGE_BUTTON */) {
        switch (keycode) {
        case 0x01: return "MOUSE_LEFT";
        case 0x02: return "MOUSE_RIGHT";
        case 0x03: return "MOUSE_MIDDLE";
        case 0x04: return "MOUSE_BACK";
        case 0x05: return "MOUSE_FORWARD";
        default:   return "MOUSE";
        }
    }
    if (usage_page == 0x0C /* HID_USAGE_CONSUMER */) {
        return "MEDIA";
    }
    return "ACTION";
}

static const uint8_t mod_bits[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
static const char *const mod_names[] = {"LCTL", "LSFT", "LALT", "LGUI",
                                        "RCTL", "RSFT", "RALT", "RGUI"};

/* Char length a token will occupy, for the sink's space_for() backpressure.
 * Mirrors token_size() in dm_feedback.c. */
static uint8_t token_size(dm_locale locale, uint8_t mods, uint16_t usage_page, uint32_t keycode) {
    uint8_t size = 0;
    if (!locale_is_plain(locale)) {
        size += 2; /* '<' and '>' */
    }
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (mods & mod_bits[i]) {
            if (!first) {
                size += 1; /* separator */
            }
            size += (uint8_t)strlen(mod_names[i]);
            first = false;
        }
    }
    if (!first) {
        size += 1; /* separator before action name */
    }
    size += (uint8_t)strlen(action_name(usage_page, keycode));
    return size;
}

static void emit_str(dm_sink *sink, const char *s) {
    for (const char *p = s; *p; p++) {
        sink->emit_char(sink->ctx, *p);
    }
}

/* Render the <TOKEN> form to the sink as characters. Mirrors render_action_token(). */
static void emit_token(dm_sink *sink, dm_locale locale, uint8_t mods, uint16_t usage_page,
                       uint32_t keycode) {
    bool plain = locale_is_plain(locale);
    char sep = plain ? ' ' : '+';

    if (!plain) {
        sink->emit_char(sink->ctx, '<');
    }
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (mods & mod_bits[i]) {
            if (!first) {
                sink->emit_char(sink->ctx, sep);
            }
            emit_str(sink, mod_names[i]);
            first = false;
        }
    }
    if (!first) {
        sink->emit_char(sink->ctx, sep);
    }
    emit_str(sink, action_name(usage_page, keycode));
    if (!plain) {
        sink->emit_char(sink->ctx, '>');
    }
}

void dm_render_slot(const dm_render_slot_view *view, dm_locale locale, dm_sink *sink) {
    if (!view || !sink) {
        return;
    }

    uint8_t active_mods = 0;

    for (uint32_t i = 0; i < view->event_count; i++) {
        const struct dm_event *ev = &view->events[i];

        if (is_modifier_key(ev->usage_page, ev->keycode)) {
            uint8_t mod_bit = (uint8_t)(1u << (ev->keycode - 0xE0));
            if (ev->pressed) {
                active_mods |= mod_bit;
            } else {
                active_mods &= (uint8_t)~mod_bit;
            }
            continue;
        }

        if (!ev->pressed) {
            continue;
        }

        uint8_t mods = active_mods | ev->implicit_mods | ev->explicit_mods;
        char c;

        if (is_replayable(locale, ev, active_mods) &&
            printable_char_for_keycode(locale, ev->keycode, (mods & DM_MOD_SHIFT_MASK) != 0, &c)) {
            if (!sink->space_for(sink->ctx, 1)) {
                return; /* ring backpressure: resume after drain */
            }
            sink->emit_char(sink->ctx, c);
        } else {
            uint8_t size = token_size(locale, mods, ev->usage_page, ev->keycode);
            if (!sink->space_for(sink->ctx, size)) {
                return;
            }
            emit_token(sink, locale, mods, ev->usage_page, ev->keycode);
        }
    }
}
