/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>
#include <zmk/keymap.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/dynamic_macros.h>

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
#include <zephyr/settings/settings.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* -------------------------------------------------------------------------- */
/*  Constants and types                                                       */
/* -------------------------------------------------------------------------- */

#define MAX_SLOTS  CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_SLOTS
#define MAX_EVENTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS
#define TAP_DELAY  CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY

enum dm_state {
    DM_STATE_IDLE = 0,
    DM_STATE_RECORDING,
    DM_STATE_PENDING_ASSIGN,
    DM_STATE_DELETE_PENDING,
    DM_STATE_PLAYING,
    DM_STATE_TYPING_FEEDBACK,
};

struct dm_event {
    uint16_t usage_page;
    uint32_t keycode;
    uint8_t implicit_mods;
    uint8_t explicit_mods;
    bool pressed;
};

struct dm_slot {
    uint32_t event_count;
    struct dm_event events[MAX_EVENTS];
};

struct dm_data {
    enum dm_state state;
    struct dm_slot slots[MAX_SLOTS];
    struct dm_slot recording_buffer;
    struct k_work_delayable assign_timeout_work;
    int playback_slot;
    uint32_t playback_event;
};

static struct dm_data dm;

static void save_slot(int slot_idx);

/* -------------------------------------------------------------------------- */
/*  Feedback: text output via simulated keystrokes                            */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK)

static bool suppress_recording = false;

#define FEEDBACK_BUF_LEN 512

struct hid_keycode {
    uint8_t keycode;
    bool shift;
};

struct fb_event {
    uint16_t keycode;
    uint8_t mods;
};

static struct fb_event feedback_buf[FEEDBACK_BUF_LEN];
static int feedback_len;
static int feedback_pos;
static bool feedback_press_phase;
static enum dm_state feedback_return_state;
static int feedback_post_save_slot;
static bool status_mode;
static int status_next_slot;
static struct k_timer feedback_timer;
static struct k_work feedback_work;

static struct hid_keycode ascii_to_hid(char c) {
    if (c >= 'a' && c <= 'z') {
        return (struct hid_keycode){.keycode = 0x04 + (c - 'a'), .shift = false};
    }
    if (c >= 'A' && c <= 'Z') {
        return (struct hid_keycode){.keycode = 0x04 + (c - 'A'), .shift = true};
    }
    if (c >= '1' && c <= '9') {
        return (struct hid_keycode){.keycode = 0x1E + (c - '1'), .shift = false};
    }
    if (c == '0') {
        return (struct hid_keycode){.keycode = 0x27, .shift = false};
    }
    switch (c) {
    case ' ':
        return (struct hid_keycode){.keycode = 0x2C, .shift = false};
    case '\n':
        return (struct hid_keycode){.keycode = 0x28, .shift = false};
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
    default:
        return (struct hid_keycode){.keycode = 0x38, .shift = true}; /* '?' for unknown */
    }
}

static void fb_reset(void) {
    feedback_len = 0;
    feedback_pos = 0;
    feedback_press_phase = true;
}

static void fb_append_hid(uint32_t keycode, uint8_t mods) {
    if (feedback_len >= FEEDBACK_BUF_LEN) {
        LOG_WRN("Dynamic macro feedback buffer full");
        return;
    }

    feedback_buf[feedback_len++] = (struct fb_event){
        .keycode = (uint16_t)keycode,
        .mods = mods,
    };
}

static void fb_append_char(char c) {
    struct hid_keycode hk = ascii_to_hid(c);
    uint8_t mods = hk.shift ? 0x02 : 0x00; /* LSHIFT */
    fb_append_hid(hk.keycode, mods);
}

static void fb_append_str(const char *str) {
    for (const char *p = str; *p; p++) {
        fb_append_char(*p);
    }
}

static void fb_append_number(int n) {
    char buf[8];
    int len = 0;
    if (n == 0) {
        fb_append_char('0');
        return;
    }
    while (n > 0 && len < (int)sizeof(buf)) {
        buf[len++] = '0' + (n % 10);
        n /= 10;
    }
    for (int i = len - 1; i >= 0; i--) {
        fb_append_char(buf[i]);
    }
}

/* Macro preview rendering: literal text stays literal; actions become <TOKENS>. */

static bool is_modifier_keycode(uint32_t keycode) {
    return keycode >= 0xE0 && keycode <= 0xE7;
}

static bool printable_char_for_keycode(uint32_t keycode, bool shifted, char *out) {
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

static const char *action_name(uint16_t usage_page, uint32_t keycode) {
    if (usage_page == 0x07) {
        return keyboard_action_name(keycode);
    }
    if (usage_page == 0x09) {
        switch (keycode) {
        case 0x01: return "MOUSE_LEFT";
        case 0x02: return "MOUSE_RIGHT";
        case 0x03: return "MOUSE_MIDDLE";
        case 0x04: return "MOUSE_BACK";
        case 0x05: return "MOUSE_FORWARD";
        default:   return "MOUSE";
        }
    }
    if (usage_page == 0x0C) {
        return "MEDIA";
    }
    return "ACTION";
}

static bool render_modifiers(uint8_t mods) {
    static const uint8_t mod_bits[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    static const char *mod_names[] = {"LCTL", "LSFT", "LALT", "LGUI",
                                      "RCTL", "RSFT", "RALT", "RGUI"};
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (mods & mod_bits[i]) {
            if (!first) {
                fb_append_char('+');
            }
            fb_append_str(mod_names[i]);
            first = false;
        }
    }
    return !first;
}

static void render_action_token(uint8_t mods, uint16_t usage_page, uint32_t keycode) {
    fb_append_char('<');
    if (render_modifiers(mods)) {
        fb_append_char('+');
    }
    fb_append_str(action_name(usage_page, keycode));
    fb_append_char('>');
}

static void render_slot_contents(const struct dm_slot *slot) {
    const uint8_t shift_mods = 0x02 | 0x20;
    const uint8_t non_shift_mods = 0xFF & ~shift_mods;
    uint8_t active_mods = 0;

    for (uint32_t i = 0; i < slot->event_count; i++) {
        const struct dm_event *ev = &slot->events[i];

        if (ev->usage_page == 0x07 && is_modifier_keycode(ev->keycode)) {
            uint8_t mod_bit = 1 << (ev->keycode - 0xE0);
            if (ev->pressed) {
                active_mods |= mod_bit;
            } else {
                active_mods &= ~mod_bit;
            }
            continue;
        }

        if (!ev->pressed) {
            continue;
        }

        uint8_t mods = active_mods | ev->implicit_mods | ev->explicit_mods;
        bool shifted = (mods & shift_mods) != 0;
        char output;

        if (ev->usage_page == 0x07 && (mods & non_shift_mods) == 0 &&
            printable_char_for_keycode(ev->keycode, shifted, &output)) {
            fb_append_char(output);
        } else {
            render_action_token(mods, ev->usage_page, ev->keycode);
        }
    }
}

static int filled_slot_count(void) {
    int filled = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (dm.slots[i].event_count > 0) {
            filled++;
        }
    }

    return filled;
}

static void render_status_slot(int slot_idx) {
    fb_append_char('S');
    fb_append_number(slot_idx);
    fb_append_str(": ");
    if (dm.slots[slot_idx].event_count == 0) {
        fb_append_char('-');
    } else {
        fb_append_char('\'');
        render_slot_contents(&dm.slots[slot_idx]);
        fb_append_str("' (");
        fb_append_number(dm.slots[slot_idx].event_count);
        fb_append_char(')');
    }
    fb_append_char('\n');
}

static void feedback_complete(void) {
    if (status_mode && status_next_slot < MAX_SLOTS) {
        fb_reset();
        render_status_slot(status_next_slot);
        status_next_slot++;
        k_timer_start(&feedback_timer, K_NO_WAIT, K_NO_WAIT);
        return;
    }

    status_mode = false;
    suppress_recording = false;

    int post_save_slot = feedback_post_save_slot;
    enum dm_state return_state = feedback_return_state;

    feedback_post_save_slot = -1;
    feedback_return_state = DM_STATE_IDLE;
    dm.state = return_state;

    if (return_state == DM_STATE_PENDING_ASSIGN || return_state == DM_STATE_DELETE_PENDING) {
        k_work_reschedule(&dm.assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
    }

    if (post_save_slot >= 0) {
        save_slot(post_save_slot);
    }
}

static void feedback_work_handler(struct k_work *work) {
    if (dm.state != DM_STATE_TYPING_FEEDBACK) {
        return;
    }

    if (feedback_pos >= feedback_len) {
        feedback_complete();
        return;
    }

    const struct fb_event *ev = &feedback_buf[feedback_pos];
    struct zmk_keycode_state_changed kc = {
        .usage_page = 0x07,
        .keycode = ev->keycode,
        .implicit_modifiers = ev->mods,
        .explicit_modifiers = 0,
        .state = feedback_press_phase,
        .timestamp = k_uptime_get(),
    };

    raise_zmk_keycode_state_changed(kc);

    if (feedback_press_phase) {
        feedback_press_phase = false;
    } else {
        feedback_press_phase = true;
        feedback_pos++;
    }

    k_timer_start(&feedback_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
}

static void feedback_timer_handler(struct k_timer *timer) {
    k_work_submit(&feedback_work);
}

static void start_feedback(enum dm_state return_state, int post_save_slot) {
    feedback_return_state = return_state;
    feedback_post_save_slot = post_save_slot;
    feedback_pos = 0;
    feedback_press_phase = true;
    suppress_recording = true;
    dm.state = DM_STATE_TYPING_FEEDBACK;

    if (feedback_len == 0) {
        feedback_complete();
        return;
    }

    k_timer_start(&feedback_timer, K_NO_WAIT, K_NO_WAIT);
}

static void feedback_rec(void) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM REC]");
    start_feedback(DM_STATE_RECORDING, -1);
}

static void feedback_stop(void) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM STOP]");
    start_feedback(DM_STATE_PENDING_ASSIGN, -1);
}

static void feedback_saved(int slot_idx, const struct dm_slot *slot) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM SAVED ");
    fb_append_number(slot_idx);
    fb_append_str(": '");
    render_slot_contents(slot);
    fb_append_str("']");
    start_feedback(DM_STATE_IDLE, slot_idx);
}

static void feedback_slot_full(int slot_idx) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM SLOT ");
    fb_append_number(slot_idx);
    fb_append_str(" FULL]");
    start_feedback(DM_STATE_PENDING_ASSIGN, -1);
}

static void feedback_deleted(int slot_idx) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM DEL ");
    fb_append_number(slot_idx);
    fb_append_str("]");
    start_feedback(DM_STATE_IDLE, -1);
}

static void feedback_slot_empty(int slot_idx) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM SLOT ");
    fb_append_number(slot_idx);
    fb_append_str(" EMPTY]");
    start_feedback(DM_STATE_IDLE, -1);
}

static void feedback_overflow(void) {
    status_mode = false;
    fb_reset();
    fb_append_str("[DM FULL]");
    start_feedback(DM_STATE_PENDING_ASSIGN, -1);
}

static void feedback_status(void) {
    status_mode = true;
    status_next_slot = 1;
    fb_reset();
    fb_append_str("[DM ");
    fb_append_number(filled_slot_count());
    fb_append_char('/');
    fb_append_number(MAX_SLOTS);
    fb_append_str("]\n");
    render_status_slot(0);
    start_feedback(DM_STATE_IDLE, -1);
}

#else /* !CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK */

static bool suppress_recording = false;
static void feedback_rec(void) { dm.state = DM_STATE_RECORDING; }
static void feedback_stop(void) {
    dm.state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&dm.assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}
static void feedback_saved(int slot_idx, const struct dm_slot *slot) {
    (void)slot;
    dm.state = DM_STATE_IDLE;
    save_slot(slot_idx);
}
static void feedback_slot_full(int slot_idx) {
    (void)slot_idx;
    dm.state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&dm.assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}
static void feedback_deleted(int slot_idx) {
    (void)slot_idx;
    dm.state = DM_STATE_IDLE;
}
static void feedback_slot_empty(int slot_idx) {
    (void)slot_idx;
    dm.state = DM_STATE_IDLE;
}
static void feedback_overflow(void) {
    dm.state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&dm.assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}
static void feedback_status(void) { dm.state = DM_STATE_IDLE; }

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK */

/* -------------------------------------------------------------------------- */
/*  NVS Persistence                                                           */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)

static int dm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    int slot_idx = -1;

    if (sscanf(name, "slot/%d", &slot_idx) != 1 || slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        return -ENOENT;
    }

    if (len < sizeof(uint32_t)) {
        LOG_WRN("Slot %d: stored length %zu too small", slot_idx, len);
        return -EINVAL;
    }

    memset(&dm.slots[slot_idx], 0, sizeof(struct dm_slot));

    int rc = read_cb(cb_arg, &dm.slots[slot_idx], sizeof(struct dm_slot));
    if (rc < 0) {
        LOG_WRN("Slot %d: read failed: %d", slot_idx, rc);
        memset(&dm.slots[slot_idx], 0, sizeof(struct dm_slot));
        return rc;
    }

    if (rc < (int)sizeof(uint32_t)) {
        LOG_WRN("Slot %d: read returned %d bytes", slot_idx, rc);
        memset(&dm.slots[slot_idx], 0, sizeof(struct dm_slot));
        return -EINVAL;
    }

    if (dm.slots[slot_idx].event_count > MAX_EVENTS) {
        LOG_WRN("Slot %d: event_count %u exceeds MAX_EVENTS", slot_idx,
                (unsigned int)dm.slots[slot_idx].event_count);
        memset(&dm.slots[slot_idx], 0, sizeof(struct dm_slot));
        return -EINVAL;
    }

    size_t expected = sizeof(uint32_t) +
                      dm.slots[slot_idx].event_count * sizeof(struct dm_event);
    if (len < expected || rc < (int)expected) {
        LOG_WRN("Slot %d: expected %zu bytes for %u events, got len=%zu rc=%d",
                slot_idx, expected, (unsigned int)dm.slots[slot_idx].event_count, len, rc);
        memset(&dm.slots[slot_idx], 0, sizeof(struct dm_slot));
        return -EINVAL;
    }

    LOG_DBG("Loaded dynamic macro slot %d with %u events", slot_idx,
            (unsigned int)dm.slots[slot_idx].event_count);
    return 0;
}

static int dm_settings_commit(void) {
    return 0;
}

static int dm_settings_export(int (*storage_func)(const char *name, const void *value,
                                                   size_t val_len)) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (dm.slots[i].event_count > 0) {
            char key[16];
            snprintf(key, sizeof(key), "dm/slot/%d", i);
            size_t data_size = sizeof(uint32_t) +
                               dm.slots[i].event_count * sizeof(struct dm_event);
            int rc = storage_func(key, &dm.slots[i], data_size);
            if (rc) {
                return rc;
            }
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dm, "dm", NULL, dm_settings_set, dm_settings_commit,
                               dm_settings_export);

static void save_slot(int slot_idx) {
    char key[16];
    snprintf(key, sizeof(key), "dm/slot/%d", slot_idx);
    size_t data_size = sizeof(uint32_t) +
                       dm.slots[slot_idx].event_count * sizeof(struct dm_event);
    int rc = settings_save_one(key, &dm.slots[slot_idx], data_size);
    if (rc) {
        LOG_ERR("Failed to save dynamic macro slot %d: %d", slot_idx, rc);
        return;
    }

    LOG_DBG("Saved dynamic macro slot %d (%u events)", slot_idx,
            (unsigned int)dm.slots[slot_idx].event_count);
}

static void delete_slot_from_storage(int slot_idx) {
    char key[16];
    snprintf(key, sizeof(key), "dm/slot/%d", slot_idx);
    int rc = settings_delete(key);
    if (rc) {
        LOG_ERR("Failed to delete dynamic macro slot %d from storage: %d", slot_idx, rc);
        return;
    }

    LOG_DBG("Deleted dynamic macro slot %d from storage", slot_idx);
}

#else /* !CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */

static void save_slot(int slot_idx) {}
static void delete_slot_from_storage(int slot_idx) {}

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */

/* -------------------------------------------------------------------------- */
/*  Playback timer                                                            */
/* -------------------------------------------------------------------------- */

static struct k_timer playback_timer;
static struct k_work playback_work;

static void playback_work_handler(struct k_work *work) {
    if (dm.state != DM_STATE_PLAYING || dm.playback_slot < 0) {
        return;
    }

    struct dm_slot *slot = &dm.slots[dm.playback_slot];
    if (dm.playback_event >= slot->event_count) {
        dm.state = DM_STATE_IDLE;
        dm.playback_slot = -1;
        k_timer_stop(&playback_timer);
        return;
    }

    const struct dm_event *ev = &slot->events[dm.playback_event++];

    struct zmk_keycode_state_changed kc = {
        .usage_page = ev->usage_page,
        .keycode = ev->keycode,
        .implicit_modifiers = ev->implicit_mods,
        .explicit_modifiers = ev->explicit_mods,
        .state = ev->pressed,
        .timestamp = k_uptime_get(),
    };

    suppress_recording = true;
    raise_zmk_keycode_state_changed(kc);
    suppress_recording = false;

    if (dm.playback_event >= slot->event_count) {
        dm.state = DM_STATE_IDLE;
        dm.playback_slot = -1;
        k_timer_stop(&playback_timer);
    }
}

static void playback_timer_handler(struct k_timer *timer) {
    k_work_submit(&playback_work);
}

/* -------------------------------------------------------------------------- */
/*  Assign/delete timeout                                                     */
/* -------------------------------------------------------------------------- */

static void assign_timeout_handler(struct k_work *work) {
    if (dm.state == DM_STATE_PENDING_ASSIGN || dm.state == DM_STATE_DELETE_PENDING) {
        LOG_DBG("Dynamic macro assign/delete timed out");
        dm.state = DM_STATE_IDLE;
    }
}

/* -------------------------------------------------------------------------- */
/*  Command handlers                                                          */
/* -------------------------------------------------------------------------- */

static void cmd_record(void) {
    if (dm.state == DM_STATE_PLAYING || dm.state == DM_STATE_TYPING_FEEDBACK) {
        return;
    }

    dm.recording_buffer.event_count = 0;
    k_work_cancel_delayable(&dm.assign_timeout_work);
    LOG_DBG("Started recording dynamic macro");
    feedback_rec();
}

static void cmd_stop(void) {
    if (dm.state != DM_STATE_RECORDING) {
        return;
    }

    LOG_DBG("Stopped recording (%d events), awaiting slot assignment",
            dm.recording_buffer.event_count);
    feedback_stop();
}

static void cmd_delete_mode(void) {
    if (dm.state != DM_STATE_IDLE) {
        return;
    }

    dm.state = DM_STATE_DELETE_PENDING;
    k_work_reschedule(&dm.assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
    LOG_DBG("Entered delete mode");
}

static void cmd_status(void) {
    if (dm.state != DM_STATE_IDLE) {
        return;
    }

    feedback_status();
}

static void cmd_slot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        LOG_ERR("Invalid slot index: %d", slot_idx);
        return;
    }

    switch (dm.state) {
    case DM_STATE_PENDING_ASSIGN:
        k_work_cancel_delayable(&dm.assign_timeout_work);
        if (dm.slots[slot_idx].event_count > 0) {
            feedback_slot_full(slot_idx);
            return;
        }
        memcpy(&dm.slots[slot_idx], &dm.recording_buffer, sizeof(struct dm_slot));
        feedback_saved(slot_idx, &dm.slots[slot_idx]);
        LOG_DBG("Assigned recording to slot %d (%d events)", slot_idx,
                dm.slots[slot_idx].event_count);
        break;

    case DM_STATE_DELETE_PENDING:
        k_work_cancel_delayable(&dm.assign_timeout_work);
        if (dm.slots[slot_idx].event_count == 0) {
            feedback_slot_empty(slot_idx);
        } else {
            dm.slots[slot_idx].event_count = 0;
            feedback_deleted(slot_idx);
            delete_slot_from_storage(slot_idx);
        }
        LOG_DBG("Slot %d cleared", slot_idx);
        break;

    case DM_STATE_IDLE:
        if (dm.slots[slot_idx].event_count == 0) {
            return;
        }
        dm.state = DM_STATE_PLAYING;
        dm.playback_slot = slot_idx;
        dm.playback_event = 0;
        LOG_DBG("Playing slot %d (%d events)", slot_idx, dm.slots[slot_idx].event_count);
        k_timer_start(&playback_timer, K_NO_WAIT, K_MSEC(TAP_DELAY));
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/*  Behavior driver API                                                       */
/* -------------------------------------------------------------------------- */

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case DM_REC:
        cmd_record();
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STP:
        cmd_stop();
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_DEL:
        cmd_delete_mode();
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STATE:
        cmd_status();
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_SLOT:
        cmd_slot(binding->param2);
        return ZMK_BEHAVIOR_OPAQUE;
    default:
        LOG_ERR("Unknown dynamic macro command: %d", binding->param1);
        return -ENOTSUP;
    }
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_dynamic_macro_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

/* -------------------------------------------------------------------------- */
/*  Event listener: capture keycode events during recording                   */
/* -------------------------------------------------------------------------- */

static int dm_event_listener(const zmk_event_t *eh) {
    if (suppress_recording) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (dm.state != DM_STATE_RECORDING) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (dm.recording_buffer.event_count >= MAX_EVENTS) {
        LOG_WRN("Dynamic macro recording buffer full (%d events)", MAX_EVENTS);
        feedback_overflow();
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct dm_event *rec = &dm.recording_buffer.events[dm.recording_buffer.event_count];
    rec->usage_page = ev->usage_page;
    rec->keycode = ev->keycode;
    rec->implicit_mods = ev->implicit_modifiers;
    rec->explicit_mods = ev->explicit_modifiers;
    rec->pressed = ev->state;
    dm.recording_buffer.event_count++;

    LOG_DBG("Recorded event %d/%d: page=0x%02x key=0x%02x %s",
            dm.recording_buffer.event_count, MAX_EVENTS,
            ev->usage_page, ev->keycode, ev->state ? "press" : "release");

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dynamic_macro, dm_event_listener);
ZMK_SUBSCRIPTION(dynamic_macro, zmk_keycode_state_changed);

/* -------------------------------------------------------------------------- */
/*  Initialization                                                            */
/* -------------------------------------------------------------------------- */

static int behavior_dynamic_macro_init(const struct device *dev) {
    memset(&dm, 0, sizeof(dm));
    dm.state = DM_STATE_IDLE;
    dm.playback_slot = -1;

    k_work_init_delayable(&dm.assign_timeout_work, assign_timeout_handler);
    k_work_init(&playback_work, playback_work_handler);
    k_timer_init(&playback_timer, playback_timer_handler, NULL);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK)
    k_work_init(&feedback_work, feedback_work_handler);
    k_timer_init(&feedback_timer, feedback_timer_handler, NULL);
    feedback_post_save_slot = -1;
#endif

    LOG_DBG("Dynamic macro behavior initialized (%d slots, %d max events)",
            MAX_SLOTS, MAX_EVENTS);
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, behavior_dynamic_macro_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_dynamic_macro_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
