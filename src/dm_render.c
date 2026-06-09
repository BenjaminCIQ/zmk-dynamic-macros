/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_render — pure preview renderer (redesign §2.3, rewrite step 1).
 *
 * One event-walk, emitting to an abstract char sink. PURE: no Zephyr, no I/O,
 * no global state. This is the single home for the replayable-vs-token decision
 * and all token formatting, so the live-typing preview and the
 * dm_get_preview_string query API cannot disagree.
 *
 * STEP 1 SCOPE: this ports the existing dm_feedback.c walk
 * (is_replayable_event / printable_char_for_keycode / render_action_token) into
 * one pure routine behind the sink. The locale mapping is still expressed as the
 * existing branch logic here; STEP 2 replaces it with static const data tables
 * (§2.3) under the safety net of these tests. Behavior must stay identical.
 */

#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_render.h>

/* HID usage page for keyboard keys (mirrors dt-bindings hid_usage_pages.h, kept
 * local so the renderer stays Zephyr-free). */
#define DM_HID_USAGE_KEY 0x07

/* Shift bits within a HID modifier mask (LSHIFT | RSHIFT). */
#define DM_MOD_SHIFT_MASK     (0x02 | 0x20)
#define DM_MOD_NON_SHIFT_MASK (~DM_MOD_SHIFT_MASK & 0xFF)

static bool locale_is_plain(dm_locale locale) {
    return locale != DM_LOCALE_US && locale != DM_LOCALE_UK;
}

/*
 * Map a HID keycode (+ shift) back to the printable character it produces on the
 * given locale. Inverse of the feedback encoder. Returns false when the key
 * produces a non-ASCII glyph on that layout (e.g. UK Shift+3 = GBP) so the
 * caller renders a <TOKEN> instead of a wrong character.
 *
 * Ported from printable_char_for_keycode() in dm_feedback.c. Only US/UK (the
 * full-punctuation locales) are inverted; plain locales never reach here for
 * punctuation (see is_replayable in the walk).
 */
static bool printable_char_for_keycode(dm_locale locale, uint32_t keycode, bool shifted,
                                       char *out) {
    if (keycode >= 0x04 && keycode <= 0x1D) {
        *out = (shifted ? 'A' : 'a') + (keycode - 0x04);
        return true;
    }
    if (keycode >= 0x1E && keycode <= 0x27) {
        static const char normal[] = "1234567890";
        static const char shifted_chars[] = "!@#$%^&*()";
        if (locale == DM_LOCALE_UK) {
            if (shifted && keycode == 0x1F) { /* '2' -> " on UK */
                *out = '"';
                return true;
            }
            if (shifted && keycode == 0x20) { /* '3' -> GBP, non-ASCII -> token */
                return false;
            }
        }
        *out = shifted ? shifted_chars[keycode - 0x1E] : normal[keycode - 0x1E];
        return true;
    }
    switch (keycode) {
    case 0x2C: *out = ' '; return true;
    case 0x2D: *out = shifted ? '_' : '-'; return true;
    case 0x2E: *out = shifted ? '+' : '='; return true;
    case 0x2F: *out = shifted ? '{' : '['; return true;
    case 0x30: *out = shifted ? '}' : ']'; return true;
    case 0x33: *out = shifted ? ':' : ';'; return true;
    case 0x36: *out = shifted ? '<' : ','; return true;
    case 0x37: *out = shifted ? '>' : '.'; return true;
    case 0x38: *out = shifted ? '?' : '/'; return true;
    default: break;
    }
    if (locale == DM_LOCALE_UK) {
        switch (keycode) {
        case 0x32: *out = shifted ? '~' : '#'; return true;
        case 0x34: *out = shifted ? '@' : '\''; return true;
        case 0x35:
            if (shifted) { return false; } /* NOT sign -> token */
            *out = '`';
            return true;
        case 0x64: *out = shifted ? '|' : '\\'; return true;
        default: return false;
        }
    }
    switch (keycode) {
    case 0x31: *out = shifted ? '|' : '\\'; return true;
    case 0x34: *out = shifted ? '"' : '\''; return true;
    case 0x35: *out = shifted ? '~' : '`'; return true;
    default: return false;
    }
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
