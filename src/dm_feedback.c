/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk-behavior-dynamic-macros/dm_internal.h>
#include <zmk-behavior-dynamic-macros/dm_feedback.h>
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
#include <zmk-behavior-dynamic-macros/events/dynamic_macro_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define TAP_DELAY CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY

/* -------------------------------------------------------------------------- */
/*  Feedback: text output via simulated keystrokes                            */
/* -------------------------------------------------------------------------- */

#if DM_TYPING_ENABLED

struct hid_keycode {
    uint8_t keycode;
    bool shift;
};

static bool feedback_enabled_for(struct behavior_dynamic_macro_data *data, int level) {
    return data->current_feedback_level >= level;
}

static bool status_enabled(int level) {
    return DM_STATUS_DETAIL >= level;
}

static struct hid_keycode letter_to_hid(char c, bool upper) {
    uint8_t keycode = 0x04 + (c - 'a');

#if DM_LOCALE == DM_LOCALE_DE
    if (c == 'y') {
        keycode = 0x1D; /* Z position */
    } else if (c == 'z') {
        keycode = 0x1C; /* Y position */
    }
#elif DM_LOCALE == DM_LOCALE_FR
    if (c == 'a') {
        keycode = 0x14; /* Q position */
    } else if (c == 'q') {
        keycode = 0x04; /* A position */
    } else if (c == 'w') {
        keycode = 0x1D; /* Z position */
    } else if (c == 'z') {
        keycode = 0x1A; /* W position */
    } else if (c == 'm') {
        keycode = 0x33; /* semicolon position */
    }
#endif

    return (struct hid_keycode){.keycode = keycode, .shift = upper};
}

static struct hid_keycode digit_to_hid(char c) {
    uint8_t keycode = (c == '0') ? 0x27 : (0x1E + (c - '1'));

#if DM_LOCALE == DM_LOCALE_FR
    return (struct hid_keycode){.keycode = keycode, .shift = true};
#else
    return (struct hid_keycode){.keycode = keycode, .shift = false};
#endif
}

static struct hid_keycode ascii_to_hid(char c) {
    if (c >= 'a' && c <= 'z') {
        return letter_to_hid(c, false);
    }
    if (c >= 'A' && c <= 'Z') {
        return letter_to_hid(c - 'A' + 'a', true);
    }
    if (c >= '0' && c <= '9') {
        return digit_to_hid(c);
    }

    switch (c) {
    case ' ':
        return (struct hid_keycode){.keycode = 0x2C, .shift = false};
    case '\n':
        return (struct hid_keycode){.keycode = 0x28, .shift = false};
#if !DM_LOCALE_PLAIN
    case '[':
        return (struct hid_keycode){.keycode = 0x2F, .shift = false};
    case ']':
        return (struct hid_keycode){.keycode = 0x30, .shift = false};
    case '\'':
        return (struct hid_keycode){.keycode = 0x34, .shift = false};
    case '"':
        return (struct hid_keycode){.keycode = 0x34, .shift = true};
    case ':':
        return (struct hid_keycode){.keycode = 0x33, .shift = true};
    case ';':
        return (struct hid_keycode){.keycode = 0x33, .shift = false};
    case '+':
        return (struct hid_keycode){.keycode = 0x2E, .shift = true};
    case '=':
        return (struct hid_keycode){.keycode = 0x2E, .shift = false};
    case '-':
        return (struct hid_keycode){.keycode = 0x2D, .shift = false};
    case '_':
        return (struct hid_keycode){.keycode = 0x2D, .shift = true};
    case '.':
        return (struct hid_keycode){.keycode = 0x37, .shift = false};
    case ',':
        return (struct hid_keycode){.keycode = 0x36, .shift = false};
    case '<':
        return (struct hid_keycode){.keycode = 0x36, .shift = true};
    case '>':
        return (struct hid_keycode){.keycode = 0x37, .shift = true};
    case '/':
        return (struct hid_keycode){.keycode = 0x38, .shift = false};
    case '?':
        return (struct hid_keycode){.keycode = 0x38, .shift = true};
    case '!':
        return (struct hid_keycode){.keycode = 0x1E, .shift = true};
    case '@':
        return (struct hid_keycode){.keycode = 0x1F, .shift = true};
    case '#':
        return (struct hid_keycode){.keycode = 0x20, .shift = true};
    case '$':
        return (struct hid_keycode){.keycode = 0x21, .shift = true};
    case '%':
        return (struct hid_keycode){.keycode = 0x22, .shift = true};
    case '^':
        return (struct hid_keycode){.keycode = 0x23, .shift = true};
    case '&':
        return (struct hid_keycode){.keycode = 0x24, .shift = true};
    case '*':
        return (struct hid_keycode){.keycode = 0x25, .shift = true};
    case '(':
        return (struct hid_keycode){.keycode = 0x26, .shift = true};
    case ')':
        return (struct hid_keycode){.keycode = 0x27, .shift = true};
    case '{':
        return (struct hid_keycode){.keycode = 0x2F, .shift = true};
    case '}':
        return (struct hid_keycode){.keycode = 0x30, .shift = true};
    case '\\':
        return (struct hid_keycode){.keycode = 0x31, .shift = false};
    case '|':
        return (struct hid_keycode){.keycode = 0x31, .shift = true};
    case '`':
        return (struct hid_keycode){.keycode = 0x35, .shift = false};
    case '~':
        return (struct hid_keycode){.keycode = 0x35, .shift = true};
#endif
    default:
        return (struct hid_keycode){.keycode = 0x2C, .shift = false}; /* space for unknown */
    }
}

static void fb_reset(struct behavior_dynamic_macro_data *data) {
    data->ring_head = 0;
    data->ring_tail = 0;
    data->feedback_press_phase = true;
    data->preview_slot = NULL;
    data->preview_idx = 0;
    data->preview_mods = 0;
    data->preview_done = true;
    data->needs_suffix = false;
    data->status_current_slot = -1;
}

static inline uint8_t ring_count_internal(struct behavior_dynamic_macro_data *data) {
    return (data->ring_head - data->ring_tail) & (FB_RING_SIZE - 1);
}

static inline uint8_t ring_space(struct behavior_dynamic_macro_data *data) {
    return FB_RING_SIZE - 1 - ring_count_internal(data);
}

bool ring_empty(struct behavior_dynamic_macro_data *data) {
    return data->ring_head == data->ring_tail;
}

static void fb_push(struct behavior_dynamic_macro_data *data, uint16_t keycode, uint8_t mods) {
    data->ring[data->ring_head] = (struct fb_event){.keycode = keycode, .mods = mods};
    data->ring_head = (data->ring_head + 1) & (FB_RING_SIZE - 1);
}

static void fb_append_hid(struct behavior_dynamic_macro_data *data, uint32_t keycode, uint8_t mods) {
    if (ring_space(data) < 1) {
        LOG_WRN("Dynamic macro feedback ring full");
        return;
    }
    fb_push(data, (uint16_t)keycode, mods);
}

static void fb_append_char(struct behavior_dynamic_macro_data *data, char c) {
    struct hid_keycode hk = ascii_to_hid(c);
    uint8_t mods = hk.shift ? 0x02 : 0x00; /* LSHIFT */
    fb_append_hid(data, hk.keycode, mods);
}

void fb_append_str(struct behavior_dynamic_macro_data *data, const char *str) {
    for (const char *p = str; *p; p++) {
        fb_append_char(data, *p);
    }
}

#if DM_LOCALE_PLAIN
#define DM_MSG_REC       "DM REC"
#define DM_MSG_STOP      "DM STOP"
#define DM_MSG_SAVED     "DM SAVED "
#define DM_MSG_SLOT      "DM SLOT "
#define DM_MSG_DEL       "DM DEL "
#define DM_MSG_DEL_FAIL  "DM DEL FAILED"
#define DM_MSG_SAVE_FAIL "DM SAVE FAILED "
#define DM_MSG_SAVE_FULL "DM SAVE QUEUE FULL "
#define DM_MSG_DEL_FULL  "DM DEL QUEUE FULL "
#define DM_MSG_EMPTY     " EMPTY"
#define DM_MSG_FULL      "DM FULL"
#define DM_MSG_MOV       "DM MOV"
#define DM_MSG_MOV_SRC   "DM MOV SRC "
#define DM_MSG_MOV_DEST  "DM MOV "
#define DM_MSG_MOV_SEP   " TO "
#define DM_MSG_MOV_CANCEL "DM MOV CANCEL"
#define DM_MSG_CHAIN     "DM PLUS"
#define DM_MSG_PREVIEW_START ""
#define DM_MSG_PREVIEW_END   ""
#define DM_MSG_SLOT_START ""
#define DM_MSG_SLOT_END   ""
#define DM_MSG_EVENTS    " EVENTS"
#define DM_MSG_FB_PREFIX "DM FB "
#define DM_MSG_CLOSE     ""
#else
#define DM_MSG_REC       "[DM REC]"
#define DM_MSG_STOP      "[DM STOP]"
#define DM_MSG_SAVED     "[DM SAVED "
#define DM_MSG_SLOT      "[DM SLOT "
#define DM_MSG_DEL       "[DM DEL "
#define DM_MSG_DEL_FAIL  " FAILED]"
#define DM_MSG_SAVE_FAIL "[DM SAVE FAILED "
#define DM_MSG_SAVE_FULL "[DM SAVE QUEUE FULL "
#define DM_MSG_DEL_FULL  "[DM DEL QUEUE FULL "
#define DM_MSG_EMPTY     ": -]"
#define DM_MSG_FULL      "[DM FULL]"
#define DM_MSG_MOV       "[DM MOV]"
#define DM_MSG_MOV_SRC   "[DM MOV SRC "
#define DM_MSG_MOV_DEST  "[DM MOV "
#define DM_MSG_MOV_SEP   "->"
#define DM_MSG_MOV_CANCEL "[DM MOV CANCEL]"
#define DM_MSG_CHAIN     "[DM +"
#define DM_MSG_PREVIEW_START ": '"
#define DM_MSG_PREVIEW_END   "'"
#define DM_MSG_SLOT_START "["
#define DM_MSG_SLOT_END   "]"
#define DM_MSG_EVENTS    ""
#define DM_MSG_FB_PREFIX "[DM FB:"
#define DM_MSG_CLOSE     "]"
#endif

static void fb_append_number(struct behavior_dynamic_macro_data *data, int n) {
    char buf[8];
    int len = 0;
    if (n == 0) {
        fb_append_char(data, '0');
        return;
    }
    while (n > 0 && len < (int)sizeof(buf)) {
        buf[len++] = '0' + (n % 10);
        n /= 10;
    }
    for (int i = len - 1; i >= 0; i--) {
        fb_append_char(data, buf[i]);
    }
}

/* Macro preview rendering: literal text stays literal; actions become <TOKENS>. */

bool printable_char_for_keycode(uint32_t keycode, bool shifted, char *out) {
    if (keycode >= 0x04 && keycode <= 0x1D) {
        *out = (shifted ? 'A' : 'a') + (keycode - 0x04);
        return true;
    }
    if (keycode >= 0x1E && keycode <= 0x27) {
        static const char normal[] = "1234567890";
        static const char shifted_chars[] = "!@#$%^&*()";
        *out = shifted ? shifted_chars[keycode - 0x1E] : normal[keycode - 0x1E];
        return true;
    }
    switch (keycode) {
    case 0x2C: *out = ' '; return true;
    case 0x2D: *out = shifted ? '_' : '-'; return true;
    case 0x2E: *out = shifted ? '+' : '='; return true;
    case 0x2F: *out = shifted ? '{' : '['; return true;
    case 0x30: *out = shifted ? '}' : ']'; return true;
    case 0x31: *out = shifted ? '|' : '\\'; return true;
    case 0x33: *out = shifted ? ':' : ';'; return true;
    case 0x34: *out = shifted ? '"' : '\''; return true;
    case 0x35: *out = shifted ? '~' : '`'; return true;
    case 0x36: *out = shifted ? '<' : ','; return true;
    case 0x37: *out = shifted ? '>' : '.'; return true;
    case 0x38: *out = shifted ? '?' : '/'; return true;
    default: return false;
    }
}

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
    case 0x3A: return "F1";
    case 0x3B: return "F2";
    case 0x3C: return "F3";
    case 0x3D: return "F4";
    case 0x3E: return "F5";
    case 0x3F: return "F6";
    case 0x40: return "F7";
    case 0x41: return "F8";
    case 0x42: return "F9";
    case 0x43: return "F10";
    case 0x44: return "F11";
    case 0x45: return "F12";
    case 0x46: return "PSCRN";
    case 0x47: return "SLCK";
    case 0x48: return "PAUSE";
    case 0x49: return "INS";
    case 0x4A: return "HOME";
    case 0x4B: return "PGUP";
    case 0x4C: return "DEL";
    case 0x4D: return "END";
    case 0x4E: return "PGDN";
    case 0x4F: return "RIGHT";
    case 0x50: return "LEFT";
    case 0x51: return "DOWN";
    case 0x52: return "UP";
    default:   return "KEY";
    }
}

const char *action_name(uint16_t usage_page, uint32_t keycode) {
    if (usage_page == HID_USAGE_KEY) {
        return keyboard_action_name(keycode);
    }
    if (usage_page == HID_USAGE_BUTTON) {
        switch (keycode) {
        case 0x01: return "MOUSE_LEFT";
        case 0x02: return "MOUSE_RIGHT";
        case 0x03: return "MOUSE_MIDDLE";
        case 0x04: return "MOUSE_BACK";
        case 0x05: return "MOUSE_FORWARD";
        default:   return "MOUSE";
        }
    }
    if (usage_page == HID_USAGE_CONSUMER) {
        return "MEDIA";
    }
    return "ACTION";
}

bool is_modifier_key(uint16_t usage_page, uint32_t keycode) {
    return usage_page == HID_USAGE_KEY && keycode >= 0xE0 && keycode <= 0xE7;
}

static bool is_replayable_event(const struct dm_event *ev, uint8_t active_mods) {
    if (ev->usage_page != HID_USAGE_KEY) {
        return false;
    }
    uint8_t mods = active_mods | ev->implicit_mods | ev->explicit_mods;
    if (mods & MOD_NON_SHIFT_MASK) {
        return false;
    }
    char dummy;
    return printable_char_for_keycode(ev->keycode, (mods & MOD_SHIFT_MASK) != 0, &dummy);
}

static const uint8_t mod_bits[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
const char *mod_names[] = {"LCTL", "LSFT", "LALT", "LGUI",
                           "RCTL", "RSFT", "RALT", "RGUI"};

static uint8_t token_size(uint8_t mods, uint16_t usage_page, uint32_t keycode) {
    uint8_t size = 0;
#if !DM_LOCALE_PLAIN
    size += 2; /* < and > */
#endif
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (mods & mod_bits[i]) {
            if (!first) {
                size += 1; /* separator */
            }
            size += strlen(mod_names[i]);
            first = false;
        }
    }
    if (!first) {
        size += 1; /* separator before action name */
    }
    size += strlen(action_name(usage_page, keycode));
    return size;
}

static void render_modifiers(struct behavior_dynamic_macro_data *data, uint8_t mods,
                             bool *first) {
    for (int i = 0; i < 8; i++) {
        if (mods & mod_bits[i]) {
            if (!*first) {
#if DM_LOCALE_PLAIN
                fb_append_char(data, ' ');
#else
                fb_append_char(data, '+');
#endif
            }
            fb_append_str(data, mod_names[i]);
            *first = false;
        }
    }
}

static void render_action_token(struct behavior_dynamic_macro_data *data, uint8_t mods,
                                uint16_t usage_page, uint32_t keycode) {
#if !DM_LOCALE_PLAIN
    fb_append_char(data, '<');
#endif
    bool first = true;
    render_modifiers(data, mods, &first);
    if (!first) {
#if DM_LOCALE_PLAIN
        fb_append_char(data, ' ');
#else
        fb_append_char(data, '+');
#endif
    }
    fb_append_str(data, action_name(usage_page, keycode));
#if !DM_LOCALE_PLAIN
    fb_append_char(data, '>');
#endif
}

bool render_slot_contents_stream(struct behavior_dynamic_macro_data *data) {
    const struct dm_slot *slot = data->preview_slot;
    if (!slot) {
        return false;
    }

    while (data->preview_idx < slot->event_count) {
        const struct dm_event *ev = &slot->events[data->preview_idx];

        if (is_modifier_key(ev->usage_page, ev->keycode)) {
            uint8_t mod_bit = 1 << (ev->keycode - 0xE0);
            if (ev->pressed) {
                data->preview_mods |= mod_bit;
            } else {
                data->preview_mods &= ~mod_bit;
            }
            data->preview_idx++;
            continue;
        }

        if (!ev->pressed) {
            data->preview_idx++;
            continue;
        }

        uint8_t mods = data->preview_mods | ev->implicit_mods | ev->explicit_mods;

        if (is_replayable_event(ev, data->preview_mods)) {
            if (ring_space(data) < 1) {
                return true; /* need drain */
            }
            uint8_t emit_mods = (mods & MOD_SHIFT_MASK) ? 0x02 : 0x00;
            fb_append_hid(data, ev->keycode, emit_mods);
        } else {
            uint8_t size = token_size(mods, ev->usage_page, ev->keycode);
            if (ring_space(data) < size) {
                return true; /* need drain */
            }
            render_action_token(data, mods, ev->usage_page, ev->keycode);
        }
        data->preview_idx++;
    }

    data->preview_slot = NULL;
    return false;
}

int filled_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)

int filled_nvs_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = 0; i < NVS_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

int filled_ram_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = NVS_SLOTS; i < MAX_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS */

static bool render_status_slot(struct behavior_dynamic_macro_data *data, int slot_idx,
                               bool show_preview) {
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if DM_LOCALE_PLAIN
    fb_append_char(data, ' ');
#else
    fb_append_str(data, ": ");
#endif
    if (slot_is_empty(data, slot_idx)) {
#if DM_LOCALE_PLAIN
        fb_append_str(data, "EMPTY");
#else
        fb_append_char(data, '-');
#endif
        fb_append_char(data, '\n');
        return false;
    }

    if (show_preview) {
#if !DM_LOCALE_PLAIN
        fb_append_char(data, '\'');
#endif
        data->preview_slot = &data->slots[slot_idx];
        data->preview_idx = 0;
        data->preview_mods = 0;
        data->preview_done = false;
        render_slot_contents_stream(data);
        return true;
    }

#if DM_LOCALE_PLAIN
    fb_append_number(data, data->slots[slot_idx].event_count);
    fb_append_str(data, " EVENTS");
#else
    fb_append_number(data, data->slots[slot_idx].event_count);
#endif
    fb_append_char(data, '\n');
    return false;
}

void dm_feedback_preview_suffix(struct behavior_dynamic_macro_data *data) {
    fb_append_str(data, DM_MSG_PREVIEW_END);
    fb_append_str(data, DM_MSG_CLOSE);
}

void status_slot_suffix(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if DM_LOCALE_PLAIN
    fb_append_char(data, ' ');
    fb_append_number(data, data->slots[slot_idx].event_count);
    fb_append_str(data, " EVENTS");
#else
    fb_append_str(data, "' (");
    fb_append_number(data, data->slots[slot_idx].event_count);
    fb_append_char(data, ')');
#endif
    fb_append_char(data, '\n');
}

void feedback_complete(struct behavior_dynamic_macro_data *data) {
    if (data->status_mode && data->status_next_slot < MAX_SLOTS) {
        bool show_preview = status_enabled(DM_STATUS_USED_PREVIEW);
        bool show_all = status_enabled(DM_STATUS_FULL);

        while (data->status_next_slot < MAX_SLOTS) {
            if (show_all || !slot_is_empty(data, data->status_next_slot)) {
                break;
            }
            data->status_next_slot++;
        }

        if (data->status_next_slot < MAX_SLOTS) {
            fb_reset(data);
            data->status_current_slot = data->status_next_slot;
            bool streaming = render_status_slot(data, data->status_next_slot, show_preview);
            data->status_next_slot++;
            if (streaming) {
                /* Will continue via emit handler streaming */
            }
            k_timer_start(&data->emit_timer, K_NO_WAIT, K_NO_WAIT);
            return;
        }
    }

    data->status_mode = false;
    data->suppress_recording = false;

    int post_save_slot = data->feedback_post_save_slot;
    enum dm_state return_state = data->feedback_return_state;

    data->feedback_post_save_slot = -1;
    data->feedback_return_state = DM_STATE_IDLE;
    data->state = return_state;

    if (return_state == DM_STATE_PENDING_ASSIGN || return_state == DM_STATE_DELETE_PENDING ||
        return_state == DM_STATE_MOVE_PENDING) {
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
    }

    if (post_save_slot >= 0) {
        dm_save_slot(data, post_save_slot);
    }
}

static void start_feedback(struct behavior_dynamic_macro_data *data, enum dm_state return_state,
                           int post_save_slot) {
    data->feedback_return_state = return_state;
    data->feedback_post_save_slot = post_save_slot;
    data->feedback_press_phase = true;
    data->suppress_recording = true;
    data->state = DM_STATE_TYPING_FEEDBACK;

    if (ring_empty(data) && data->preview_done) {
        feedback_complete(data);
        return;
    }

    k_timer_start(&data->emit_timer, K_NO_WAIT, K_NO_WAIT);
}

void feedback_rec(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STARTED, -1);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_REC);
    start_feedback(data, DM_STATE_RECORDING, -1);
}

void feedback_stop(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STOPPED, -1);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_PENDING_ASSIGN;
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_STOP);
    start_feedback(data, DM_STATE_PENDING_ASSIGN, -1);
}

void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                    const struct dm_slot *slot) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_SAVED, slot_idx);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_IDLE;
        dm_save_slot(data, slot_idx);
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SAVED);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    if (feedback_enabled_for(data, DM_FEEDBACK_VERBOSE)) {
        fb_append_str(data, DM_MSG_PREVIEW_START);
        data->preview_slot = slot;
        data->preview_idx = 0;
        data->preview_mods = 0;
        data->preview_done = false;
        data->needs_suffix = true;
        render_slot_contents_stream(data);
    } else {
        fb_append_str(data, DM_MSG_CLOSE);
    }
    start_feedback(data, DM_STATE_IDLE, slot_idx);
}

void feedback_slot_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    enum dm_state return_state =
        data->state == DM_STATE_MOVE_PENDING ? DM_STATE_MOVE_PENDING : DM_STATE_PENDING_ASSIGN;

    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = return_state;
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SLOT);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if DM_LOCALE_PLAIN
    fb_append_str(data, " FULL");
#else
    fb_append_str(data, " FULL]");
#endif
    start_feedback(data, return_state, -1);
}

void dm_feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_DELETED, slot_idx);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_DEL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED, slot_idx);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_DEL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_DEL_FAIL);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED, slot_idx);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_ERROR) || data->state != DM_STATE_IDLE) {
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SAVE_FAIL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_ERROR) || data->state != DM_STATE_IDLE) {
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SAVE_FULL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_DEL_FULL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY, slot_idx);
#endif
    enum dm_state return_state =
        data->state == DM_STATE_MOVE_PENDING ? DM_STATE_MOVE_PENDING : DM_STATE_IDLE;

    if (!feedback_enabled_for(data, DM_FEEDBACK_BASIC)) {
        data->state = return_state;
        if (return_state == DM_STATE_MOVE_PENDING) {
            k_work_reschedule(&data->assign_timeout_work,
                              K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        }
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SLOT);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_EMPTY);
    start_feedback(data, return_state, -1);
}

void feedback_overflow(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW, -1);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_PENDING_ASSIGN;
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_FULL);
    start_feedback(data, DM_STATE_PENDING_ASSIGN, -1);
}

void feedback_status(struct behavior_dynamic_macro_data *data) {
    if (!status_enabled(DM_STATUS_COUNT)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = status_enabled(DM_STATUS_USED) && MAX_SLOTS > 0;
    data->status_next_slot = 0;
    fb_reset(data);
#if DM_LOCALE_PLAIN
    fb_append_str(data, "DM ");
#else
    fb_append_str(data, "[DM ");
#endif
    fb_append_number(data, filled_slot_count(data));
#if DM_LOCALE_PLAIN
    fb_append_str(data, " OF ");
#else
    fb_append_char(data, '/');
#endif
    fb_append_number(data, MAX_SLOTS);
    if (MAX_SLOTS == 0) {
        fb_append_str(data, " NO SLOTS");
    } else if (NVS_SLOTS > 0 && NVS_SLOTS < MAX_SLOTS) {
#if DM_LOCALE_PLAIN
        fb_append_str(data, " NVS 0 TO ");
#else
        fb_append_str(data, " NVS:0-");
#endif
        fb_append_number(data, NVS_SLOTS - 1);
#if DM_LOCALE_PLAIN
        fb_append_str(data, " RAM ");
#else
        fb_append_str(data, " RAM:");
#endif
        fb_append_number(data, NVS_SLOTS);
#if DM_LOCALE_PLAIN
        fb_append_str(data, " TO ");
#else
        fb_append_char(data, '-');
#endif
        fb_append_number(data, MAX_SLOTS - 1);
    } else if (NVS_SLOTS == 0) {
        fb_append_str(data, " RAM");
    } else {
        fb_append_str(data, " NVS");
    }
#if !DM_LOCALE_PLAIN
    fb_append_str(data, "]\n");
#else
    fb_append_char(data, '\n');
#endif
    start_feedback(data, DM_STATE_IDLE, -1);
}

void feedback_move_prompt(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_MOVE_PENDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV);
    start_feedback(data, DM_STATE_MOVE_PENDING, -1);
}

void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_MOVE_PENDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV_SRC);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_MOVE_PENDING, -1);
}

void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_MOVED, dst);
#endif
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV_DEST);
    fb_append_char(data, slot_storage_prefix(src));
    fb_append_number(data, src);
    fb_append_str(data, DM_MSG_MOV_SEP);
    fb_append_char(data, slot_storage_prefix(dst));
    fb_append_number(data, dst);
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void feedback_move_cancelled(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV_CANCEL);
    start_feedback(data, DM_STATE_IDLE, -1);
}

void feedback_chain_insert(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot) {
    (void)slot_idx;

    if (!feedback_enabled_for(data, DM_FEEDBACK_VERBOSE)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    data->preview_slot = slot;
    data->preview_idx = 0;
    data->preview_mods = 0;
    data->preview_done = false;
    data->needs_suffix = false;
    render_slot_contents_stream(data);
    start_feedback(data, DM_STATE_RECORDING, -1);
}

void feedback_chain_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled_for(data, DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SLOT);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_EMPTY);
    start_feedback(data, DM_STATE_RECORDING, -1);
}

void feedback_chain_no_room(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled_for(data, DM_FEEDBACK_COMMAND)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_CHAIN);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if DM_LOCALE_PLAIN
    fb_append_str(data, " FULL");
#else
    fb_append_str(data, " FULL]");
#endif
    start_feedback(data, DM_STATE_RECORDING, -1);
}

static const char *feedback_level_name(uint8_t level) {
    switch (level) {
    case DM_FEEDBACK_ERROR:   return "ERROR";
    case DM_FEEDBACK_COMMAND: return "COMMAND";
    case DM_FEEDBACK_BASIC:   return "BASIC";
    case DM_FEEDBACK_VERBOSE: return "VERBOSE";
    default:                  return "?";
    }
}

void cmd_feedback_adjust(struct behavior_dynamic_macro_data *data, int direction) {
    if (data->state != DM_STATE_IDLE) {
        return;
    }

    int new_level = (int)data->current_feedback_level + direction;
    if (new_level < DM_FEEDBACK_ERROR) {
        new_level = DM_FEEDBACK_ERROR;
    }
    if (new_level > DM_FEEDBACK_VERBOSE) {
        new_level = DM_FEEDBACK_VERBOSE;
    }

    data->current_feedback_level = (uint8_t)new_level;
    dm_storage_save_feedback_level(data);

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_FB_PREFIX);
    fb_append_str(data, feedback_level_name(data->current_feedback_level));
    fb_append_str(data, DM_MSG_CLOSE);
    start_feedback(data, DM_STATE_IDLE, -1);
}

#else /* !DM_TYPING_ENABLED */

/*
 * Feedback OFF stubs - state transitions without typed output
 *
 * Function                      | Next State           | Side Effects
 * ------------------------------|----------------------|---------------------------
 * feedback_rec                  | RECORDING            |
 * feedback_stop                 | PENDING_ASSIGN       | reschedule timeout
 * feedback_saved                | IDLE                 | dm_save_slot()
 * feedback_slot_full            | keep/PENDING_ASSIGN  | reschedule timeout
 * feedback_deleted              | IDLE                 |
 * feedback_delete_failed        | IDLE                 |
 * feedback_save_failed          | (no change)          |
 * feedback_save_queue_full      | (no change)          |
 * feedback_delete_queue_full    | IDLE                 |
 * feedback_slot_empty           | IDLE or keep MOVE    | reschedule if MOVE_PENDING
 * feedback_overflow             | PENDING_ASSIGN       | reschedule timeout
 * feedback_status               | IDLE                 |
 * feedback_move_prompt          | MOVE_PENDING         |
 * feedback_move_source_selected | MOVE_PENDING         |
 * feedback_moved                | IDLE                 |
 * feedback_move_cancelled       | IDLE                 |
 * feedback_chain_insert         | RECORDING            |
 * feedback_chain_empty          | RECORDING            |
 * feedback_chain_no_room        | RECORDING            |
 */

#define ASSIGN_TIMEOUT K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT)

void feedback_rec(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STARTED, -1);
#endif
    data->state = DM_STATE_RECORDING;
}
void feedback_stop(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STOPPED, -1);
#endif
    data->state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
}
void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                    const struct dm_slot *slot) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_SAVED, slot_idx);
#endif
    (void)slot;
    data->state = DM_STATE_IDLE;
    dm_save_slot(data, slot_idx);
}
void feedback_slot_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    if (data->state != DM_STATE_MOVE_PENDING) {
        data->state = DM_STATE_PENDING_ASSIGN;
    }
    k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
}
void dm_feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_DELETED, slot_idx);
#endif
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
void dm_feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED, slot_idx);
#endif
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
void dm_feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED, slot_idx);
#endif
    (void)data;
    (void)slot_idx;
}
void dm_feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    (void)data;
    (void)slot_idx;
}
void dm_feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY, slot_idx);
#endif
    (void)slot_idx;
    if (data->state == DM_STATE_MOVE_PENDING) {
        k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
        return;
    }
    data->state = DM_STATE_IDLE;
}
void feedback_overflow(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW, -1);
#endif
    data->state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
}
void feedback_status(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_IDLE;
}
void feedback_move_prompt(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_MOVE_PENDING;
}
void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_MOVE_PENDING;
}
void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_MOVED, dst);
#endif
    (void)src;
    (void)dst;
    data->state = DM_STATE_IDLE;
}
void feedback_move_cancelled(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_IDLE;
}
void feedback_chain_insert(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot) {
    (void)slot_idx;
    (void)slot;
    data->state = DM_STATE_RECORDING;
}
void feedback_chain_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_RECORDING;
}
void feedback_chain_no_room(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_RECORDING;
}

void feedback_complete(struct behavior_dynamic_macro_data *data) {
    (void)data;
}

#undef ASSIGN_TIMEOUT

#endif /* DM_TYPING_ENABLED */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
