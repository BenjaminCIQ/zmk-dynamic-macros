/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#include <stdio.h>
#include <string.h>

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

#define MAX_EVENTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS
#define TAP_DELAY  CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY
#define DM_FEEDBACK_OFF     0
#define DM_FEEDBACK_ERROR   1
#define DM_FEEDBACK_BASIC   2
#define DM_FEEDBACK_VERBOSE 3
#define DM_FEEDBACK_LEVEL   CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_LEVEL
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
#define NVS_SLOTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_NVS_SLOTS
#else
#define NVS_SLOTS 0
#endif
#define RAM_SLOTS     CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_RAM_SLOTS
#define MAX_SLOTS     (NVS_SLOTS + RAM_SLOTS)
#define SLOT_CAPACITY (MAX_SLOTS > 0 ? MAX_SLOTS : 1)

BUILD_ASSERT(NVS_SLOTS <= 16, "Dynamic macros support at most 16 NVS slots");
BUILD_ASSERT(RAM_SLOTS <= 48, "Dynamic macros support at most 48 RAM slots");
BUILD_ASSERT(MAX_SLOTS <= 64, "Dynamic macros support at most 64 total slots");

#if MAX_SLOTS == 0
#warning "Dynamic macro has zero slots; all dynamic macro slot bindings are invalid"
#endif

#define DM_IS_DM_BINDING(idx, layer)                                                              \
    DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(layer, bindings, idx),                                   \
                       zmk_behavior_dynamic_macro)

#define DM_VALIDATE_SLOT_CMD(idx, layer, command, limit, msg)                                     \
    COND_CODE_1(DM_IS_DM_BINDING(idx, layer),                                                     \
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) != command ||           \
                                  DT_PHA_BY_IDX(layer, bindings, idx, param2) < (limit),           \
                              msg);),                                                            \
                ())

#define DM_VALIDATE_CMD_RANGE(idx, layer)                                                         \
    COND_CODE_1(DM_IS_DM_BINDING(idx, layer),                                                     \
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) <= DM_SLOT_RAM,         \
                              "Dynamic macro param1 is not a valid command (expected 0-6)");),    \
                ())

#define DM_VALIDATE_CMD_NO_PARAM2(idx, layer, command)                                            \
    COND_CODE_1(DM_IS_DM_BINDING(idx, layer),                                                     \
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) != command ||           \
                                  DT_PHA_BY_IDX(layer, bindings, idx, param2) == 0,                \
                              #command " does not use param2 (must be 0)");),                     \
                ())

#define DM_VALIDATE_KEYMAP_BINDING(idx, layer)                                                    \
    DM_VALIDATE_CMD_RANGE(idx, layer)                                                             \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_REC)                                                 \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_STP)                                                 \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_DEL)                                                 \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_STATE)                                               \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_MOV)                                                 \
    DM_VALIDATE_SLOT_CMD(idx, layer, DM_SLOT_NVS, NVS_SLOTS,                                      \
                         "DM_SLOT_NVS index exceeds configured NVS dynamic macro slots")          \
    DM_VALIDATE_SLOT_CMD(idx, layer, DM_SLOT_RAM, RAM_SLOTS,                                      \
                         "DM_SLOT_RAM index exceeds configured RAM dynamic macro slots")

#define DM_VALIDATE_KEYMAP_LAYER(layer)                                                           \
    COND_CODE_1(DT_NODE_HAS_PROP(layer, bindings),                                                \
                (LISTIFY(DT_PROP_LEN(layer, bindings), DM_VALIDATE_KEYMAP_BINDING, (), layer)),   \
                ())

ZMK_KEYMAP_LAYERS_FOREACH(DM_VALIDATE_KEYMAP_LAYER)

enum dm_state {
    DM_STATE_IDLE = 0,
    DM_STATE_RECORDING,
    DM_STATE_PENDING_ASSIGN,
    DM_STATE_DELETE_PENDING,
    DM_STATE_MOVE_PENDING,
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

#if DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF
#define FEEDBACK_BUF_LEN 512

struct fb_event {
    uint16_t keycode;
    uint8_t mods;
};
#endif

struct behavior_dynamic_macro_config {
    const char *settings_name;
};

struct behavior_dynamic_macro_data {
    const struct device *dev;
    enum dm_state state;
    struct dm_slot slots[SLOT_CAPACITY];
    bool pending_delete[SLOT_CAPACITY];
    uint32_t slot_generation[SLOT_CAPACITY];
    struct dm_slot recording_buffer;
    struct k_work_delayable assign_timeout_work;
    int move_source_slot;
    int playback_slot;
    uint32_t playback_event;
    struct k_timer playback_timer;
    struct k_work playback_work;
    bool suppress_recording;
#if DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF
    struct fb_event feedback_buf[FEEDBACK_BUF_LEN];
    int feedback_len;
    int feedback_pos;
    bool feedback_press_phase;
    enum dm_state feedback_return_state;
    int feedback_post_save_slot;
    bool status_mode;
    int status_next_slot;
    struct k_timer feedback_timer;
    struct k_work feedback_work;
#endif
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

#define DM_COMMAND_VALUE(name, command)                                                           \
    {                                                                                              \
        .display_name = name,                                                                      \
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,                                               \
        .value = command,                                                                          \
    }

static const struct behavior_parameter_value_metadata dm_param_rec[] = {
    DM_COMMAND_VALUE("Record", DM_REC),
};
static const struct behavior_parameter_value_metadata dm_param_stop[] = {
    DM_COMMAND_VALUE("Stop", DM_STP),
};
static const struct behavior_parameter_value_metadata dm_param_delete[] = {
    DM_COMMAND_VALUE("Delete", DM_DEL),
};
static const struct behavior_parameter_value_metadata dm_param_state[] = {
    DM_COMMAND_VALUE("State", DM_STATE),
};
static const struct behavior_parameter_value_metadata dm_param_move[] = {
    DM_COMMAND_VALUE("Move", DM_MOV),
};
#if NVS_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_slot_nvs[] = {
    DM_COMMAND_VALUE("NVS Slot", DM_SLOT_NVS),
};
#endif
#if RAM_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_slot_ram[] = {
    DM_COMMAND_VALUE("RAM Slot", DM_SLOT_RAM),
};
#endif
static const struct behavior_parameter_value_metadata dm_param_unused[] = {
    {
        .display_name = "Unused",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_NIL,
    },
};

#if NVS_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_nvs_slot_index[] = {
    {
        .display_name = "NVS slot index",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = NVS_SLOTS - 1},
    },
};
#endif
#if RAM_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_ram_slot_index[] = {
    {
        .display_name = "RAM slot index",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = RAM_SLOTS - 1},
    },
};
#endif

static const struct behavior_parameter_metadata_set dm_parameter_metadata_sets[] = {
    {
        .param1_values_len = ARRAY_SIZE(dm_param_rec),
        .param1_values = dm_param_rec,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
    {
        .param1_values_len = ARRAY_SIZE(dm_param_stop),
        .param1_values = dm_param_stop,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
    {
        .param1_values_len = ARRAY_SIZE(dm_param_delete),
        .param1_values = dm_param_delete,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
    {
        .param1_values_len = ARRAY_SIZE(dm_param_state),
        .param1_values = dm_param_state,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
    {
        .param1_values_len = ARRAY_SIZE(dm_param_move),
        .param1_values = dm_param_move,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
#if NVS_SLOTS > 0
    {
        .param1_values_len = ARRAY_SIZE(dm_param_slot_nvs),
        .param1_values = dm_param_slot_nvs,
        .param2_values_len = ARRAY_SIZE(dm_param_nvs_slot_index),
        .param2_values = dm_param_nvs_slot_index,
    },
#endif
#if RAM_SLOTS > 0
    {
        .param1_values_len = ARRAY_SIZE(dm_param_slot_ram),
        .param1_values = dm_param_slot_ram,
        .param2_values_len = ARRAY_SIZE(dm_param_ram_slot_index),
        .param2_values = dm_param_ram_slot_index,
    },
#endif
};

static const struct behavior_parameter_metadata dm_parameter_metadata = {
    .sets_len = ARRAY_SIZE(dm_parameter_metadata_sets),
    .sets = dm_parameter_metadata_sets,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

static void save_slot(struct behavior_dynamic_macro_data *data, int slot_idx);
static int delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx);

static bool slot_is_nvs(int slot_idx) {
    return IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST) && slot_idx < NVS_SLOTS;
}

static char slot_storage_prefix(int slot_idx) {
    return slot_is_nvs(slot_idx) ? 'N' : 'R';
}

static bool slot_is_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    return data->slots[slot_idx].event_count == 0 || data->pending_delete[slot_idx];
}

/* -------------------------------------------------------------------------- */
/*  Feedback: text output via simulated keystrokes                            */
/* -------------------------------------------------------------------------- */

#if DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF

struct hid_keycode {
    uint8_t keycode;
    bool shift;
};

static bool feedback_enabled(int level) {
    return DM_FEEDBACK_LEVEL >= level;
}

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

static void fb_reset(struct behavior_dynamic_macro_data *data) {
    data->feedback_len = 0;
    data->feedback_pos = 0;
    data->feedback_press_phase = true;
}

static void fb_append_hid(struct behavior_dynamic_macro_data *data, uint32_t keycode, uint8_t mods) {
    if (data->feedback_len >= FEEDBACK_BUF_LEN) {
        LOG_WRN("Dynamic macro feedback buffer full");
        return;
    }

    data->feedback_buf[data->feedback_len++] = (struct fb_event){
        .keycode = (uint16_t)keycode,
        .mods = mods,
    };
}

static void fb_append_char(struct behavior_dynamic_macro_data *data, char c) {
    struct hid_keycode hk = ascii_to_hid(c);
    uint8_t mods = hk.shift ? 0x02 : 0x00; /* LSHIFT */
    fb_append_hid(data, hk.keycode, mods);
}

static void fb_append_str(struct behavior_dynamic_macro_data *data, const char *str) {
    for (const char *p = str; *p; p++) {
        fb_append_char(data, *p);
    }
}

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

static bool render_modifiers(struct behavior_dynamic_macro_data *data, uint8_t mods) {
    static const uint8_t mod_bits[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    static const char *mod_names[] = {"LCTL", "LSFT", "LALT", "LGUI",
                                      "RCTL", "RSFT", "RALT", "RGUI"};
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (mods & mod_bits[i]) {
            if (!first) {
                fb_append_char(data, '+');
            }
            fb_append_str(data, mod_names[i]);
            first = false;
        }
    }
    return !first;
}

static void render_action_token(struct behavior_dynamic_macro_data *data, uint8_t mods,
                                uint16_t usage_page, uint32_t keycode) {
    fb_append_char(data, '<');
    if (render_modifiers(data, mods)) {
        fb_append_char(data, '+');
    }
    fb_append_str(data, action_name(usage_page, keycode));
    fb_append_char(data, '>');
}

static void render_slot_contents(struct behavior_dynamic_macro_data *data,
                                 const struct dm_slot *slot) {
    const uint8_t shift_mods = 0x02 | 0x20;
    const uint8_t non_shift_mods = 0xFF & ~shift_mods;
    uint8_t active_mods = 0;

    for (uint32_t i = 0; i < slot->event_count; i++) {
        const struct dm_event *ev = &slot->events[i];

        if (is_mod(ev->usage_page, ev->keycode)) {
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

        if (ev->usage_page == HID_USAGE_KEY && (mods & non_shift_mods) == 0 &&
            printable_char_for_keycode(ev->keycode, shifted, &output)) {
            fb_append_char(data, output);
        } else {
            render_action_token(data, mods, ev->usage_page, ev->keycode);
        }
    }
}

static int filled_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

static void render_status_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, ": ");
    if (slot_is_empty(data, slot_idx)) {
        fb_append_char(data, '-');
    } else {
        fb_append_char(data, '\'');
        render_slot_contents(data, &data->slots[slot_idx]);
        fb_append_str(data, "' (");
        fb_append_number(data, data->slots[slot_idx].event_count);
        fb_append_char(data, ')');
    }
    fb_append_char(data, '\n');
}

static void feedback_complete(struct behavior_dynamic_macro_data *data) {
    if (data->status_mode && data->status_next_slot < MAX_SLOTS) {
        fb_reset(data);
        render_status_slot(data, data->status_next_slot);
        data->status_next_slot++;
        k_timer_start(&data->feedback_timer, K_NO_WAIT, K_NO_WAIT);
        return;
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
        save_slot(data, post_save_slot);
    }
}

static void feedback_work_handler(struct k_work *work) {
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(work, struct behavior_dynamic_macro_data, feedback_work);

    if (data->state != DM_STATE_TYPING_FEEDBACK) {
        return;
    }

    if (data->feedback_pos >= data->feedback_len) {
        feedback_complete(data);
        return;
    }

    const struct fb_event *ev = &data->feedback_buf[data->feedback_pos];
    struct zmk_keycode_state_changed kc = {
        .usage_page = HID_USAGE_KEY,
        .keycode = ev->keycode,
        .implicit_modifiers = ev->mods,
        .explicit_modifiers = 0,
        .state = data->feedback_press_phase,
        .timestamp = k_uptime_get(),
    };

    raise_zmk_keycode_state_changed(kc);

    if (data->feedback_press_phase) {
        data->feedback_press_phase = false;
    } else {
        data->feedback_press_phase = true;
        data->feedback_pos++;
    }

    k_timer_start(&data->feedback_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
}

static void feedback_timer_handler(struct k_timer *timer) {
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(timer, struct behavior_dynamic_macro_data, feedback_timer);

    k_work_submit(&data->feedback_work);
}

static void start_feedback(struct behavior_dynamic_macro_data *data, enum dm_state return_state,
                           int post_save_slot) {
    data->feedback_return_state = return_state;
    data->feedback_post_save_slot = post_save_slot;
    data->feedback_pos = 0;
    data->feedback_press_phase = true;
    data->suppress_recording = true;
    data->state = DM_STATE_TYPING_FEEDBACK;

    if (data->feedback_len == 0) {
        feedback_complete(data);
        return;
    }

    k_timer_start(&data->feedback_timer, K_NO_WAIT, K_NO_WAIT);
}

static void feedback_rec(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM REC]");
    start_feedback(data, DM_STATE_RECORDING, -1);
}

static void feedback_stop(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_PENDING_ASSIGN;
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM STOP]");
    start_feedback(data, DM_STATE_PENDING_ASSIGN, -1);
}

static void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        save_slot(data, slot_idx);
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM SAVED ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    if (feedback_enabled(DM_FEEDBACK_VERBOSE)) {
        fb_append_str(data, ": '");
        render_slot_contents(data, slot);
        fb_append_str(data, "'");
    }
    fb_append_char(data, ']');
    start_feedback(data, DM_STATE_IDLE, slot_idx);
}

static void feedback_slot_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    enum dm_state return_state =
        data->state == DM_STATE_MOVE_PENDING ? DM_STATE_MOVE_PENDING : DM_STATE_PENDING_ASSIGN;

    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = return_state;
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM SLOT ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, " FULL]");
    start_feedback(data, return_state, -1);
}

static void feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM DEL ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, "]");
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM DEL ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, " FAILED]");
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_ERROR) || data->state != DM_STATE_IDLE) {
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM SAVE FAILED ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_char(data, ']');
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_ERROR) || data->state != DM_STATE_IDLE) {
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM SAVE QUEUE FULL ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_char(data, ']');
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM DEL QUEUE FULL ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_char(data, ']');
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    enum dm_state return_state =
        data->state == DM_STATE_MOVE_PENDING ? DM_STATE_MOVE_PENDING : DM_STATE_IDLE;

    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = return_state;
        if (return_state == DM_STATE_MOVE_PENDING) {
            k_work_reschedule(&data->assign_timeout_work,
                              K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        }
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM SLOT ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, " EMPTY]");
    start_feedback(data, return_state, -1);
}

static void feedback_overflow(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_PENDING_ASSIGN;
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM FULL]");
    start_feedback(data, DM_STATE_PENDING_ASSIGN, -1);
}

static void feedback_status(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = feedback_enabled(DM_FEEDBACK_VERBOSE) && MAX_SLOTS > 0;
    data->status_next_slot = 1;
    fb_reset(data);
    fb_append_str(data, "[DM ");
    fb_append_number(data, filled_slot_count(data));
    fb_append_char(data, '/');
    fb_append_number(data, MAX_SLOTS);
    if (MAX_SLOTS == 0) {
        fb_append_str(data, " NO SLOTS");
    } else if (NVS_SLOTS > 0 && NVS_SLOTS < MAX_SLOTS) {
        fb_append_str(data, " NVS:0-");
        fb_append_number(data, NVS_SLOTS - 1);
        fb_append_str(data, " RAM:");
        fb_append_number(data, NVS_SLOTS);
        fb_append_char(data, '-');
        fb_append_number(data, MAX_SLOTS - 1);
    } else if (NVS_SLOTS == 0) {
        fb_append_str(data, " RAM");
    } else {
        fb_append_str(data, " NVS");
    }
    fb_append_str(data, "]\n");
    if (data->status_mode) {
        render_status_slot(data, 0);
    }
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_move_prompt(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_MOVE_PENDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM MOV]");
    start_feedback(data, DM_STATE_MOVE_PENDING, -1);
}

static void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_MOVE_PENDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM MOV SRC ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_char(data, ']');
    start_feedback(data, DM_STATE_MOVE_PENDING, -1);
}

static void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM MOV ");
    fb_append_char(data, slot_storage_prefix(src));
    fb_append_number(data, src);
    fb_append_str(data, "->");
    fb_append_char(data, slot_storage_prefix(dst));
    fb_append_number(data, dst);
    fb_append_char(data, ']');
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_move_cancelled(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM MOV CANCEL]");
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_chain_insert(struct behavior_dynamic_macro_data *data, int slot_idx,
                                  const struct dm_slot *slot) {
    (void)slot_idx;

    data->status_mode = false;
    fb_reset(data);
    render_slot_contents(data, slot);
    start_feedback(data, DM_STATE_RECORDING, -1);
}

static void feedback_chain_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM SLOT ");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, " EMPTY]");
    start_feedback(data, DM_STATE_RECORDING, -1);
}

static void feedback_chain_no_room(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, "[DM +");
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, " FULL]");
    start_feedback(data, DM_STATE_RECORDING, -1);
}

#else /* DM_FEEDBACK_LEVEL == DM_FEEDBACK_OFF */

static void feedback_rec(struct behavior_dynamic_macro_data *data) { data->state = DM_STATE_RECORDING; }
static void feedback_stop(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}
static void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot) {
    (void)slot;
    data->state = DM_STATE_IDLE;
    save_slot(data, slot_idx);
}
static void feedback_slot_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state =
        data->state == DM_STATE_MOVE_PENDING ? DM_STATE_MOVE_PENDING : DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}
static void feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
static void feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
static void feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)data;
    (void)slot_idx;
}
static void feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)data;
    (void)slot_idx;
}
static void feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
static void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    if (data->state == DM_STATE_MOVE_PENDING) {
        k_work_reschedule(&data->assign_timeout_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        return;
    }
    data->state = DM_STATE_IDLE;
}
static void feedback_overflow(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}
static void feedback_status(struct behavior_dynamic_macro_data *data) { data->state = DM_STATE_IDLE; }
static void feedback_move_prompt(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_MOVE_PENDING;
}
static void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_MOVE_PENDING;
}
static void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst) {
    (void)src;
    (void)dst;
    data->state = DM_STATE_IDLE;
}
static void feedback_move_cancelled(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_IDLE;
}
static void feedback_chain_insert(struct behavior_dynamic_macro_data *data, int slot_idx,
                                  const struct dm_slot *slot) {
    (void)slot_idx;
    (void)slot;
    data->state = DM_STATE_RECORDING;
}
static void feedback_chain_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_RECORDING;
}
static void feedback_chain_no_room(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_RECORDING;
}

#endif /* DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF */

/* -------------------------------------------------------------------------- */
/*  NVS Persistence                                                           */
/* -------------------------------------------------------------------------- */

#define DM_DEVICE(inst) DEVICE_DT_INST_GET(inst),
static const struct device *dm_devices[] = {DT_INST_FOREACH_STATUS_OKAY(DM_DEVICE)};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)

enum dm_storage_op_type {
    DM_STORAGE_OP_SAVE,
    DM_STORAGE_OP_DELETE,
};

struct dm_storage_op {
    enum dm_storage_op_type type;
    struct behavior_dynamic_macro_data *data;
    int slot_idx;
    uint32_t generation;
    struct dm_slot slot;
};

#define DM_STORAGE_STACK_SIZE  1024
#define DM_STORAGE_QUEUE_LEN   8
#define DM_STORAGE_PRIORITY    10

K_KERNEL_STACK_DEFINE(dm_storage_work_q_stack, DM_STORAGE_STACK_SIZE);
K_MSGQ_DEFINE(dm_storage_msgq, sizeof(struct dm_storage_op), DM_STORAGE_QUEUE_LEN, 4);

static struct k_work_q dm_storage_work_q;
static struct k_work dm_storage_work;
static bool dm_storage_work_q_started;

static void settings_slot_key(struct behavior_dynamic_macro_data *data, int slot_idx, char *key,
                              size_t key_len) {
    const struct behavior_dynamic_macro_config *config = data->dev->config;

    snprintf(key, key_len, "dm/%s/slot/%d", config->settings_name, slot_idx);
}

static void dm_storage_work_handler(struct k_work *work) {
    static struct dm_storage_op op;

    while (k_msgq_get(&dm_storage_msgq, &op, K_NO_WAIT) == 0) {
        char key[64];
        settings_slot_key(op.data, op.slot_idx, key, sizeof(key));

        if (op.type == DM_STORAGE_OP_SAVE) {
            size_t data_size = sizeof(uint32_t) + op.slot.event_count * sizeof(struct dm_event);
            int rc = settings_save_one(key, &op.slot, data_size);
            if (rc) {
                LOG_ERR("Failed to save dynamic macro slot %d: %d", op.slot_idx, rc);
                feedback_save_failed(op.data, op.slot_idx);
            } else {
                LOG_DBG("Saved dynamic macro slot %d (%u events)", op.slot_idx,
                        (unsigned int)op.slot.event_count);
            }
            continue;
        }

        int rc = settings_delete(key);
        if (rc) {
            LOG_ERR("Failed to delete dynamic macro slot %d from storage: %d", op.slot_idx, rc);
            if (op.data->pending_delete[op.slot_idx] &&
                op.data->slot_generation[op.slot_idx] == op.generation) {
                op.data->pending_delete[op.slot_idx] = false;
            }
            if (op.data->state == DM_STATE_IDLE) {
                feedback_delete_failed(op.data, op.slot_idx);
            }
            continue;
        }

        if (op.data->pending_delete[op.slot_idx] &&
            op.data->slot_generation[op.slot_idx] == op.generation) {
            memset(&op.data->slots[op.slot_idx], 0, sizeof(struct dm_slot));
            op.data->pending_delete[op.slot_idx] = false;
            if (op.data->state == DM_STATE_IDLE) {
                feedback_deleted(op.data, op.slot_idx);
            }
        }

        LOG_DBG("Deleted dynamic macro slot %d from storage", op.slot_idx);
    }
}

static void init_storage_work_queue(void) {
    if (dm_storage_work_q_started) {
        return;
    }

    k_work_init(&dm_storage_work, dm_storage_work_handler);
    k_work_queue_start(&dm_storage_work_q, dm_storage_work_q_stack,
                       K_KERNEL_STACK_SIZEOF(dm_storage_work_q_stack),
                       DM_STORAGE_PRIORITY, NULL);
    dm_storage_work_q_started = true;
}

static int enqueue_storage_op(struct behavior_dynamic_macro_data *data, enum dm_storage_op_type type,
                              int slot_idx, const struct dm_slot *slot) {
    struct dm_storage_op op = {
        .type = type,
        .data = data,
        .slot_idx = slot_idx,
        .generation = data->slot_generation[slot_idx],
    };

    if (slot != NULL) {
        memcpy(&op.slot, slot, sizeof(op.slot));
    }

    int rc = k_msgq_put(&dm_storage_msgq, &op, K_NO_WAIT);
    if (rc) {
        LOG_ERR("Dynamic macro storage queue full for slot %d: %d", slot_idx, rc);
        if (type == DM_STORAGE_OP_SAVE) {
            feedback_save_queue_full(data, slot_idx);
        } else {
            feedback_delete_queue_full(data, slot_idx);
        }
        return rc;
    }

    k_work_submit_to_queue(&dm_storage_work_q, &dm_storage_work);
    return 0;
}

static bool parse_settings_slot_name(const char *name, struct behavior_dynamic_macro_data **data,
                                     int *slot_idx) {
    const char *p = name;
    int parsed = 0;

    for (size_t i = 0; i < ARRAY_SIZE(dm_devices); i++) {
        const struct device *dev = dm_devices[i];
        const struct behavior_dynamic_macro_config *config = dev->config;
        size_t settings_name_len = strlen(config->settings_name);

        if (strncmp(name, config->settings_name, settings_name_len) != 0 ||
            name[settings_name_len] != '/') {
            continue;
        }

        *data = dev->data;
        p = name + settings_name_len + 1;
        break;
    }

    if (*data == NULL || strncmp(p, "slot/", strlen("slot/")) != 0) {
        return false;
    }
    p += strlen("slot/");

    if (*p == '\0') {
        return false;
    }
    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return false;
        }

        parsed = parsed * 10 + (*p - '0');
        if (parsed >= MAX_SLOTS) {
            return false;
        }

        p++;
    }

    *slot_idx = parsed;
    return true;
}

static int dm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    struct behavior_dynamic_macro_data *data = NULL;
    int slot_idx = -1;

    if (!parse_settings_slot_name(name, &data, &slot_idx)) {
        return -ENOENT;
    }

    if (!slot_is_nvs(slot_idx)) {
        LOG_DBG("Ignoring stored dynamic macro RAM slot %d", slot_idx);
        return 0;
    }

    if (len < sizeof(uint32_t)) {
        LOG_WRN("Slot %d: stored length %zu too small", slot_idx, len);
        return -EINVAL;
    }

    memset(&data->slots[slot_idx], 0, sizeof(struct dm_slot));

    int rc = read_cb(cb_arg, &data->slots[slot_idx], sizeof(struct dm_slot));
    if (rc < 0) {
        LOG_WRN("Slot %d: read failed: %d", slot_idx, rc);
        memset(&data->slots[slot_idx], 0, sizeof(struct dm_slot));
        return rc;
    }

    if (rc < (int)sizeof(uint32_t)) {
        LOG_WRN("Slot %d: read returned %d bytes", slot_idx, rc);
        memset(&data->slots[slot_idx], 0, sizeof(struct dm_slot));
        return -EINVAL;
    }

    if (data->slots[slot_idx].event_count > MAX_EVENTS) {
        LOG_WRN("Slot %d: event_count %u exceeds MAX_EVENTS", slot_idx,
                (unsigned int)data->slots[slot_idx].event_count);
        memset(&data->slots[slot_idx], 0, sizeof(struct dm_slot));
        return -EINVAL;
    }

    size_t expected = sizeof(uint32_t) +
                      data->slots[slot_idx].event_count * sizeof(struct dm_event);
    if (len < expected || rc < (int)expected) {
        LOG_WRN("Slot %d: expected %zu bytes for %u events, got len=%zu rc=%d",
                slot_idx, expected, (unsigned int)data->slots[slot_idx].event_count, len, rc);
        memset(&data->slots[slot_idx], 0, sizeof(struct dm_slot));
        return -EINVAL;
    }

    LOG_DBG("Loaded dynamic macro slot %d with %u events", slot_idx,
            (unsigned int)data->slots[slot_idx].event_count);
    return 0;
}

static int dm_settings_commit(void) {
    return 0;
}

static int dm_settings_export(int (*storage_func)(const char *name, const void *value,
                                                   size_t val_len)) {
    for (size_t dev_idx = 0; dev_idx < ARRAY_SIZE(dm_devices); dev_idx++) {
        const struct device *dev = dm_devices[dev_idx];
        struct behavior_dynamic_macro_data *data = dev->data;

        for (int i = 0; i < MAX_SLOTS; i++) {
            if (!slot_is_nvs(i) || slot_is_empty(data, i)) {
                continue;
            }

            char key[64];
            settings_slot_key(data, i, key, sizeof(key));
            size_t data_size = sizeof(uint32_t) +
                               data->slots[i].event_count * sizeof(struct dm_event);
            int rc = storage_func(key, &data->slots[i], data_size);
            if (rc) {
                return rc;
            }
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dm, "dm", NULL, dm_settings_set, dm_settings_commit,
                               dm_settings_export);

static void save_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!slot_is_nvs(slot_idx)) {
        return;
    }

    enqueue_storage_op(data, DM_STORAGE_OP_SAVE, slot_idx, &data->slots[slot_idx]);
}

static int delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!slot_is_nvs(slot_idx)) {
        return 0;
    }

    return enqueue_storage_op(data, DM_STORAGE_OP_DELETE, slot_idx, NULL);
}

#else /* !CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */

static void save_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)data;
    (void)slot_idx;
}
static int delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)data;
    (void)slot_idx;
    return 0;
}

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */

/* -------------------------------------------------------------------------- */
/*  Playback timer                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Dynamic macros record HID keycode events, not behavior bindings. Playback
 * raises the same raw events intentionally instead of using behavior_queue.
 */
static void playback_work_handler(struct k_work *work) {
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(work, struct behavior_dynamic_macro_data, playback_work);

    if (data->state != DM_STATE_PLAYING || data->playback_slot < 0) {
        return;
    }

    struct dm_slot *slot = &data->slots[data->playback_slot];
    if (data->playback_event >= slot->event_count) {
        data->state = DM_STATE_IDLE;
        data->playback_slot = -1;
        k_timer_stop(&data->playback_timer);
        return;
    }

    const struct dm_event *ev = &slot->events[data->playback_event++];

    struct zmk_keycode_state_changed kc = {
        .usage_page = ev->usage_page,
        .keycode = ev->keycode,
        .implicit_modifiers = ev->implicit_mods,
        .explicit_modifiers = ev->explicit_mods,
        .state = ev->pressed,
        .timestamp = k_uptime_get(),
    };

    data->suppress_recording = true;
    raise_zmk_keycode_state_changed(kc);
    data->suppress_recording = false;

    if (data->playback_event >= slot->event_count) {
        data->state = DM_STATE_IDLE;
        data->playback_slot = -1;
        k_timer_stop(&data->playback_timer);
    }
}

static void playback_timer_handler(struct k_timer *timer) {
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(timer, struct behavior_dynamic_macro_data, playback_timer);

    k_work_submit(&data->playback_work);
}

/* -------------------------------------------------------------------------- */
/*  Assign/delete timeout                                                     */
/* -------------------------------------------------------------------------- */

static void assign_timeout_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(delayable, struct behavior_dynamic_macro_data, assign_timeout_work);

    if (data->state == DM_STATE_PENDING_ASSIGN || data->state == DM_STATE_DELETE_PENDING ||
        data->state == DM_STATE_MOVE_PENDING) {
        LOG_DBG("Dynamic macro assign/delete timed out");
        data->move_source_slot = -1;
        data->state = DM_STATE_IDLE;
    }
}

/* -------------------------------------------------------------------------- */
/*  Command handlers                                                          */
/* -------------------------------------------------------------------------- */

static void cmd_record(struct behavior_dynamic_macro_data *data) {
    if (data->state == DM_STATE_PLAYING || data->state == DM_STATE_TYPING_FEEDBACK) {
        return;
    }

    data->recording_buffer.event_count = 0;
    k_work_cancel_delayable(&data->assign_timeout_work);
    LOG_DBG("Started recording dynamic macro");
    feedback_rec(data);
}

static void cmd_stop(struct behavior_dynamic_macro_data *data) {
    if (data->state != DM_STATE_RECORDING) {
        return;
    }

    LOG_DBG("Stopped recording (%d events), awaiting slot assignment",
            data->recording_buffer.event_count);
    feedback_stop(data);
}

static void cmd_delete_mode(struct behavior_dynamic_macro_data *data) {
    if (data->state != DM_STATE_IDLE) {
        return;
    }

    data->state = DM_STATE_DELETE_PENDING;
    k_work_reschedule(&data->assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
    LOG_DBG("Entered delete mode");
}

static void cmd_move_mode(struct behavior_dynamic_macro_data *data) {
    if (data->state != DM_STATE_IDLE) {
        return;
    }

    data->move_source_slot = -1;
    data->state = DM_STATE_MOVE_PENDING;
    k_work_reschedule(&data->assign_timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
    LOG_DBG("Entered move mode");
    feedback_move_prompt(data);
}

static void cmd_status(struct behavior_dynamic_macro_data *data) {
    if (data->state != DM_STATE_IDLE) {
        return;
    }

    feedback_status(data);
}

static void cmd_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        LOG_ERR("Invalid slot index: %d", slot_idx);
        return;
    }

    switch (data->state) {
    case DM_STATE_RECORDING: {
        if (slot_is_empty(data, slot_idx)) {
            feedback_chain_empty(data, slot_idx);
            return;
        }

        struct dm_slot *src = &data->slots[slot_idx];
        uint32_t remaining = MAX_EVENTS - data->recording_buffer.event_count;
        if (src->event_count > remaining) {
            feedback_chain_no_room(data, slot_idx);
            return;
        }

        memcpy(&data->recording_buffer.events[data->recording_buffer.event_count],
               src->events, src->event_count * sizeof(struct dm_event));
        data->recording_buffer.event_count += src->event_count;
        feedback_chain_insert(data, slot_idx, src);
        LOG_DBG("Chained slot %d into recording (%u events total)", slot_idx,
                (unsigned int)data->recording_buffer.event_count);
        break;
    }

    case DM_STATE_PENDING_ASSIGN:
        k_work_cancel_delayable(&data->assign_timeout_work);
        if (data->slots[slot_idx].event_count > 0 && !data->pending_delete[slot_idx]) {
            feedback_slot_full(data, slot_idx);
            return;
        }
        data->pending_delete[slot_idx] = false;
        data->slot_generation[slot_idx]++;
        memcpy(&data->slots[slot_idx], &data->recording_buffer, sizeof(struct dm_slot));
        feedback_saved(data, slot_idx, &data->slots[slot_idx]);
        LOG_DBG("Assigned recording to slot %d (%d events)", slot_idx,
                data->slots[slot_idx].event_count);
        break;

    case DM_STATE_DELETE_PENDING:
        k_work_cancel_delayable(&data->assign_timeout_work);
        if (slot_is_empty(data, slot_idx)) {
            feedback_slot_empty(data, slot_idx);
        } else {
            if (slot_is_nvs(slot_idx)) {
                data->pending_delete[slot_idx] = true;
                data->state = DM_STATE_IDLE;
                int rc = delete_slot_from_storage(data, slot_idx);
                if (rc) {
                    data->pending_delete[slot_idx] = false;
                    return;
                }

                LOG_DBG("Queued slot %d for deletion", slot_idx);
                break;
            }

            data->slots[slot_idx].event_count = 0;
            feedback_deleted(data, slot_idx);
        }
        LOG_DBG("Slot %d cleared", slot_idx);
        break;

    case DM_STATE_MOVE_PENDING: {
        if (data->move_source_slot < 0) {
            if (slot_is_empty(data, slot_idx)) {
                feedback_slot_empty(data, slot_idx);
                return;
            }

            data->move_source_slot = slot_idx;
            feedback_move_source_selected(data, slot_idx);
            LOG_DBG("Selected move source slot %d", slot_idx);
            return;
        }

        int src = data->move_source_slot;
        int dst = slot_idx;

        if (src == dst) {
            data->move_source_slot = -1;
            k_work_cancel_delayable(&data->assign_timeout_work);
            feedback_move_cancelled(data);
            LOG_DBG("Cancelled same-slot move %d", src);
            return;
        }

        if (!slot_is_empty(data, dst)) {
            feedback_slot_full(data, dst);
            return;
        }

        k_work_cancel_delayable(&data->assign_timeout_work);
        data->pending_delete[dst] = false;
        data->slot_generation[dst]++;
        memcpy(&data->slots[dst], &data->slots[src], sizeof(struct dm_slot));

        data->pending_delete[src] = false;
        data->slot_generation[src]++;
        memset(&data->slots[src], 0, sizeof(struct dm_slot));

        save_slot(data, dst);
        delete_slot_from_storage(data, src);

        data->move_source_slot = -1;
        LOG_DBG("Moved slot %d -> slot %d", src, dst);
        feedback_moved(data, src, dst);
        break;
    }

    case DM_STATE_IDLE:
        if (slot_is_empty(data, slot_idx)) {
            LOG_DBG("Slot %d is empty, nothing to play", slot_idx);
            feedback_slot_empty(data, slot_idx);
            return;
        }
        data->state = DM_STATE_PLAYING;
        data->playback_slot = slot_idx;
        data->playback_event = 0;
        LOG_DBG("Playing slot %d (%d events)", slot_idx, data->slots[slot_idx].event_count);
        k_timer_start(&data->playback_timer, K_NO_WAIT, K_MSEC(TAP_DELAY));
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
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_dynamic_macro_data *data = dev->data;

    switch (binding->param1) {
    case DM_REC:
        cmd_record(data);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STP:
        cmd_stop(data);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_DEL:
        cmd_delete_mode(data);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_MOV:
        cmd_move_mode(data);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STATE:
        cmd_status(data);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_SLOT_NVS:
        if (binding->param2 < 0 || binding->param2 >= NVS_SLOTS) {
            LOG_ERR("NVS slot index %d out of range (max %d)", binding->param2, NVS_SLOTS - 1);
            return -EINVAL;
        }
        cmd_slot(data, binding->param2);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_SLOT_RAM:
        if (binding->param2 < 0 || binding->param2 >= RAM_SLOTS) {
            LOG_ERR("RAM slot index %d out of range (max %d)", binding->param2, RAM_SLOTS - 1);
            return -EINVAL;
        }
        cmd_slot(data, NVS_SLOTS + binding->param2);
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
    .parameter_metadata = &dm_parameter_metadata,
#endif
};

/* -------------------------------------------------------------------------- */
/*  Event listener: capture keycode events during recording                   */
/* -------------------------------------------------------------------------- */

static int dm_event_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (size_t i = 0; i < ARRAY_SIZE(dm_devices); i++) {
        struct behavior_dynamic_macro_data *data = dm_devices[i]->data;

        if (data->suppress_recording) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(dm_devices); i++) {
        struct behavior_dynamic_macro_data *data = dm_devices[i]->data;

        if (data->state != DM_STATE_RECORDING) {
            continue;
        }

        if (data->recording_buffer.event_count >= MAX_EVENTS) {
            LOG_WRN("Dynamic macro recording buffer full (%d events)", MAX_EVENTS);
            feedback_overflow(data);
            continue;
        }

        struct dm_event *rec = &data->recording_buffer.events[data->recording_buffer.event_count];
        rec->usage_page = ev->usage_page;
        rec->keycode = ev->keycode;
        rec->implicit_mods = ev->implicit_modifiers;
        rec->explicit_mods = ev->explicit_modifiers;
        rec->pressed = ev->state;
        data->recording_buffer.event_count++;

        LOG_DBG("Recorded event %d/%d: page=0x%02x key=0x%02x %s",
                data->recording_buffer.event_count, MAX_EVENTS,
                ev->usage_page, ev->keycode, ev->state ? "press" : "release");
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dynamic_macro, dm_event_listener);
ZMK_SUBSCRIPTION(dynamic_macro, zmk_keycode_state_changed);

/* -------------------------------------------------------------------------- */
/*  Initialization                                                            */
/* -------------------------------------------------------------------------- */

static int behavior_dynamic_macro_init(const struct device *dev) {
    struct behavior_dynamic_macro_data *data = dev->data;

    memset(data, 0, sizeof(*data));
    data->dev = dev;
    data->state = DM_STATE_IDLE;
    data->move_source_slot = -1;
    data->playback_slot = -1;

    k_work_init_delayable(&data->assign_timeout_work, assign_timeout_handler);
    k_work_init(&data->playback_work, playback_work_handler);
    k_timer_init(&data->playback_timer, playback_timer_handler, NULL);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
    init_storage_work_queue();
#endif
#if DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF
    k_work_init(&data->feedback_work, feedback_work_handler);
    k_timer_init(&data->feedback_timer, feedback_timer_handler, NULL);
    data->feedback_post_save_slot = -1;
#endif

    LOG_DBG("Dynamic macro behavior initialized (%d slots, %d max events)",
            MAX_SLOTS, MAX_EVENTS);
    return 0;
}

#define DM_INST(n)                                                                            \
    static struct behavior_dynamic_macro_data behavior_dynamic_macro_data_##n = {};            \
    static const struct behavior_dynamic_macro_config behavior_dynamic_macro_config_##n = {     \
        .settings_name = DEVICE_DT_NAME(DT_DRV_INST(n)),                                       \
    };                                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_dynamic_macro_init, NULL,                              \
                            &behavior_dynamic_macro_data_##n,                                  \
                            &behavior_dynamic_macro_config_##n, POST_KERNEL,                   \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                               \
                            &behavior_dynamic_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DM_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
