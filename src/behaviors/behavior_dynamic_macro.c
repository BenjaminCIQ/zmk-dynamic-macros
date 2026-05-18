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
#include <zephyr/sys/atomic.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>
#include <zmk/keymap.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/dynamic_macros.h>
#include <zmk-dynamic-macros/dm_internal.h>
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
#include <zmk-dynamic-macros/events/dynamic_macro_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* -------------------------------------------------------------------------- */
/*  Constants and types                                                       */
/* -------------------------------------------------------------------------- */

#define TAP_DELAY CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY

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
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) <= DM_PREVIEW,          \
                              "Dynamic macro param1 is not a valid command (expected 0-7)");),    \
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
                         "DM_SLOT_RAM index exceeds configured RAM dynamic macro slots")          \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_PREVIEW)

#define DM_VALIDATE_KEYMAP_LAYER(layer)                                                           \
    COND_CODE_1(DT_NODE_HAS_PROP(layer, bindings),                                                \
                (LISTIFY(DT_PROP_LEN(layer, bindings), DM_VALIDATE_KEYMAP_BINDING, (), layer)),   \
                ())

ZMK_KEYMAP_LAYERS_FOREACH(DM_VALIDATE_KEYMAP_LAYER)

BUILD_ASSERT(sizeof(struct dm_event) == 8, "dm_event must be 8 bytes packed");

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
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
static const struct behavior_parameter_value_metadata dm_param_preview[] = {
    DM_COMMAND_VALUE("Preview", DM_PREVIEW),
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
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    {
        .param1_values_len = ARRAY_SIZE(dm_param_preview),
        .param1_values = dm_param_preview,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
#endif
};

static const struct behavior_parameter_metadata dm_parameter_metadata = {
    .sets_len = ARRAY_SIZE(dm_parameter_metadata_sets),
    .sets = dm_parameter_metadata_sets,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

static char slot_storage_prefix(int slot_idx) {
    return slot_is_nvs(slot_idx) ? 'N' : 'R';
}

/* -------------------------------------------------------------------------- */
/*  NVS Persistence                                                           */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)

static void save_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    dm_storage_save_slot(data, slot_idx);
}

static int delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx) {
    return dm_storage_delete_slot(data, slot_idx);
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
/*  Event notification                                                        */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
static void raise_dm_state_changed(struct behavior_dynamic_macro_data *data,
                                   enum zmk_dynamic_macro_event_type event, int slot) {
    enum zmk_dynamic_macro_state state;

    switch (data->state) {
    case DM_STATE_RECORDING:
        state = ZMK_DYNAMIC_MACRO_STATE_RECORDING;
        break;
    case DM_STATE_PLAYING:
        state = ZMK_DYNAMIC_MACRO_STATE_PLAYING;
        break;
    default:
        state = ZMK_DYNAMIC_MACRO_STATE_IDLE;
        break;
    }

    LOG_DBG("dm_event: type=%d slot=%d state=%d", event, slot, state);

    raise_zmk_dynamic_macro_state_changed((struct zmk_dynamic_macro_state_changed){
        .state = state,
        .event = event,
        .slot = slot,
        .slot_is_nvs = slot >= 0 ? slot_is_nvs(slot) : false,
    });
}
#endif

/* -------------------------------------------------------------------------- */
/*  Feedback: text output via simulated keystrokes                            */
/* -------------------------------------------------------------------------- */

#if DM_TYPING_ENABLED

struct hid_keycode {
    uint8_t keycode;
    bool shift;
};

static bool feedback_enabled(int level) {
    return DM_FEEDBACK_LEVEL >= level;
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

static inline uint8_t ring_count(struct behavior_dynamic_macro_data *data) {
    return (data->ring_head - data->ring_tail) & (FB_RING_SIZE - 1);
}

static inline uint8_t ring_space(struct behavior_dynamic_macro_data *data) {
    return FB_RING_SIZE - 1 - ring_count(data);
}

static inline bool ring_empty(struct behavior_dynamic_macro_data *data) {
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

static void fb_append_str(struct behavior_dynamic_macro_data *data, const char *str) {
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
#define DM_MSG_MOV_CANCEL "DM MOV CANCEL"
#define DM_MSG_CHAIN     "DM PLUS"
#define DM_MSG_PREVIEW_START ""
#define DM_MSG_PREVIEW_END   ""
#define DM_MSG_SLOT_START ""
#define DM_MSG_SLOT_END   ""
#define DM_MSG_EVENTS    " EVENTS"
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
#define DM_MSG_MOV_CANCEL "[DM MOV CANCEL]"
#define DM_MSG_CHAIN     "[DM +"
#define DM_MSG_PREVIEW_START ": '"
#define DM_MSG_PREVIEW_END   "'"
#define DM_MSG_SLOT_START "["
#define DM_MSG_SLOT_END   "]"
#define DM_MSG_EVENTS    ""
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

#define MOD_SHIFT_MASK (0x02 | 0x20)
#define MOD_NON_SHIFT_MASK (~MOD_SHIFT_MASK & 0xFF)

static bool is_modifier_key(uint16_t usage_page, uint32_t keycode) {
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
static const char *mod_names[] = {"LCTL", "LSFT", "LALT", "LGUI",
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

/*
 * Stream preview content from slot. Returns true if more work remains.
 * Caller must set data->preview_slot before first call.
 * Resumes from data->preview_idx with modifier state in data->preview_mods.
 */
static bool render_slot_contents_stream(struct behavior_dynamic_macro_data *data) {
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
            /* Printable char: 1 fb_event */
            if (ring_space(data) < 1) {
                return true; /* need drain */
            }
            uint8_t emit_mods = (mods & MOD_SHIFT_MASK) ? 0x02 : 0x00;
            fb_append_hid(data, ev->keycode, emit_mods);
        } else {
            /* Action token: variable size */
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

static int filled_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)

static int filled_nvs_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = 0; i < NVS_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

static int filled_ram_slot_count(struct behavior_dynamic_macro_data *data) {
    int filled = 0;

    for (int i = NVS_SLOTS; i < MAX_SLOTS; i++) {
        if (!slot_is_empty(data, i)) {
            filled++;
        }
    }

    return filled;
}

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS */

/*
 * Render status slot. For non-empty slots with preview, sets up streaming
 * and returns true (caller must handle streaming completion).
 * Returns false if no streaming needed.
 */
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
        /* Suffix will be added by status_slot_suffix after streaming */
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

static void status_slot_suffix(struct behavior_dynamic_macro_data *data, int slot_idx) {
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

static void feedback_complete(struct behavior_dynamic_macro_data *data) {
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
        save_slot(data, post_save_slot);
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

static void feedback_rec(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STARTED, -1);
#endif
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_RECORDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_REC);
    start_feedback(data, DM_STATE_RECORDING, -1);
}

static void feedback_stop(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STOPPED, -1);
#endif
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
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

static void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_SAVED, slot_idx);
#endif
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        save_slot(data, slot_idx);
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SAVED);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    if (feedback_enabled(DM_FEEDBACK_VERBOSE)) {
        fb_append_str(data, DM_MSG_PREVIEW_START);
        data->preview_slot = slot;
        data->preview_idx = 0;
        data->preview_mods = 0;
        data->preview_done = false;
        data->needs_suffix = true;
        render_slot_contents_stream(data);
    } else {
#if !DM_LOCALE_PLAIN
        fb_append_char(data, ']');
#endif
    }
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
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_DELETED, slot_idx);
#endif
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_DEL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if !DM_LOCALE_PLAIN
    fb_append_str(data, "]");
#endif
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED, slot_idx);
#endif
    if (!feedback_enabled(DM_FEEDBACK_ERROR)) {
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
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED, slot_idx);
#endif
    if (!feedback_enabled(DM_FEEDBACK_ERROR) || data->state != DM_STATE_IDLE) {
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SAVE_FAIL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if !DM_LOCALE_PLAIN
    fb_append_char(data, ']');
#endif
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    if (!feedback_enabled(DM_FEEDBACK_ERROR) || data->state != DM_STATE_IDLE) {
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_SAVE_FULL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if !DM_LOCALE_PLAIN
    fb_append_char(data, ']');
#endif
    start_feedback(data, DM_STATE_IDLE, -1);
}

void dm_feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    if (!feedback_enabled(DM_FEEDBACK_ERROR)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_DEL_FULL);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if !DM_LOCALE_PLAIN
    fb_append_char(data, ']');
#endif
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY, slot_idx);
#endif
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
    fb_append_str(data, DM_MSG_SLOT);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
    fb_append_str(data, DM_MSG_EMPTY);
    start_feedback(data, return_state, -1);
}

static void feedback_overflow(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW, -1);
#endif
    if (!feedback_enabled(DM_FEEDBACK_ERROR)) {
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

static void feedback_status(struct behavior_dynamic_macro_data *data) {
    if (!status_enabled(DM_STATUS_COUNT)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = status_enabled(DM_STATUS_USED) && MAX_SLOTS > 0;
    data->status_next_slot = 1;
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
    if (data->status_mode) {
        bool show_preview = status_enabled(DM_STATUS_USED_PREVIEW);
        bool show_all = status_enabled(DM_STATUS_FULL);
        if (show_all || !slot_is_empty(data, 0)) {
            render_status_slot(data, 0, show_preview);
        }
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
    fb_append_str(data, DM_MSG_MOV);
    start_feedback(data, DM_STATE_MOVE_PENDING, -1);
}

static void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_MOVE_PENDING;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV_SRC);
    fb_append_char(data, slot_storage_prefix(slot_idx));
    fb_append_number(data, slot_idx);
#if !DM_LOCALE_PLAIN
    fb_append_char(data, ']');
#endif
    start_feedback(data, DM_STATE_MOVE_PENDING, -1);
}

static void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_MOVED, dst);
#endif
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV_DEST);
    fb_append_char(data, slot_storage_prefix(src));
    fb_append_number(data, src);
#if DM_LOCALE_PLAIN
    fb_append_str(data, " TO ");
#else
    fb_append_str(data, "->");
#endif
    fb_append_char(data, slot_storage_prefix(dst));
    fb_append_number(data, dst);
#if !DM_LOCALE_PLAIN
    fb_append_char(data, ']');
#endif
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_move_cancelled(struct behavior_dynamic_macro_data *data) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
        data->state = DM_STATE_IDLE;
        return;
    }

    data->status_mode = false;
    fb_reset(data);
    fb_append_str(data, DM_MSG_MOV_CANCEL);
    start_feedback(data, DM_STATE_IDLE, -1);
}

static void feedback_chain_insert(struct behavior_dynamic_macro_data *data, int slot_idx,
                                  const struct dm_slot *slot) {
    (void)slot_idx;

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

static void feedback_chain_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
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

static void feedback_chain_no_room(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!feedback_enabled(DM_FEEDBACK_BASIC)) {
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

#else /* !DM_TYPING_ENABLED */

/*
 * Feedback OFF stubs - state transitions without typed output
 *
 * Function                      | Next State           | Side Effects
 * ------------------------------|----------------------|---------------------------
 * feedback_rec                  | RECORDING            |
 * feedback_stop                 | PENDING_ASSIGN       | reschedule timeout
 * feedback_saved                | IDLE                 | save_slot()
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

static void feedback_rec(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STARTED, -1);
#endif
    data->state = DM_STATE_RECORDING;
}
static void feedback_stop(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_RECORDING_STOPPED, -1);
#endif
    data->state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
}
static void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_SAVED, slot_idx);
#endif
    (void)slot;
    data->state = DM_STATE_IDLE;
    save_slot(data, slot_idx);
}
static void feedback_slot_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    if (data->state != DM_STATE_MOVE_PENDING) {
        data->state = DM_STATE_PENDING_ASSIGN;
    }
    k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
}
void dm_feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_DELETED, slot_idx);
#endif
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
void dm_feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED, slot_idx);
#endif
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
void dm_feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED, slot_idx);
#endif
    (void)data;
    (void)slot_idx;
}
void dm_feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    (void)data;
    (void)slot_idx;
}
void dm_feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL, slot_idx);
#endif
    (void)slot_idx;
    data->state = DM_STATE_IDLE;
}
static void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY, slot_idx);
#endif
    (void)slot_idx;
    if (data->state == DM_STATE_MOVE_PENDING) {
        k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
        return;
    }
    data->state = DM_STATE_IDLE;
}
static void feedback_overflow(struct behavior_dynamic_macro_data *data) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW, -1);
#endif
    data->state = DM_STATE_PENDING_ASSIGN;
    k_work_reschedule(&data->assign_timeout_work, ASSIGN_TIMEOUT);
}
static void feedback_status(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_IDLE;
}
static void feedback_move_prompt(struct behavior_dynamic_macro_data *data) {
    data->state = DM_STATE_MOVE_PENDING;
}
static void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)slot_idx;
    data->state = DM_STATE_MOVE_PENDING;
}
static void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_MOVED, dst);
#endif
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

#undef ASSIGN_TIMEOUT

#endif /* DM_TYPING_ENABLED */

/* -------------------------------------------------------------------------- */
/*  Unified emit pump (playback + feedback typing)                            */
/* -------------------------------------------------------------------------- */

/*
 * Unified emit handler for both macro playback and feedback typing.
 * Playback emits recorded dm_events directly; feedback emits fb_events
 * from ring buffer with streaming refill for previews.
 */
static void emit_work_handler(struct k_work *work) {
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(work, struct behavior_dynamic_macro_data, emit_work);

    if (data->state == DM_STATE_PLAYING) {
        if (data->playback_slot < 0) {
            k_timer_stop(&data->emit_timer);
            return;
        }

        struct dm_slot *slot = &data->slots[data->playback_slot];
        if (data->playback_event >= slot->event_count) {
            data->state = DM_STATE_IDLE;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
            raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_PLAY_FINISHED, data->playback_slot);
#endif
            data->playback_slot = -1;
            k_timer_stop(&data->emit_timer);
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
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
            raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_PLAY_FINISHED, data->playback_slot);
#endif
            data->playback_slot = -1;
            k_timer_stop(&data->emit_timer);
        } else {
            k_timer_start(&data->emit_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
        }
        return;
    }

#if DM_TYPING_ENABLED
    if (data->state == DM_STATE_TYPING_FEEDBACK) {
        /* Refill ring from streaming preview if needed */
        if (ring_empty(data) && !data->preview_done) {
            bool more = render_slot_contents_stream(data);
            if (!more) {
                data->preview_done = true;
                if (data->needs_suffix) {
                    /* feedback_saved suffix */
                    fb_append_str(data, DM_MSG_PREVIEW_END);
#if !DM_LOCALE_PLAIN
                    fb_append_char(data, ']');
#endif
                    data->needs_suffix = false;
                } else if (data->status_mode && data->status_current_slot >= 0) {
                    /* status slot suffix */
                    status_slot_suffix(data, data->status_current_slot);
                    data->status_current_slot = -1;
                }
            }
        }

        if (ring_empty(data)) {
            feedback_complete(data);
            return;
        }

        /* Peek at tail without removing */
        struct fb_event *ev = &data->ring[data->ring_tail];
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
            /* Advance tail after both press and release */
            data->ring_tail = (data->ring_tail + 1) & (FB_RING_SIZE - 1);
        }

        k_timer_start(&data->emit_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
        return;
    }
#endif
}

static void emit_timer_handler(struct k_timer *timer) {
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(timer, struct behavior_dynamic_macro_data, emit_timer);

    k_work_submit(&data->emit_work);
}

/* -------------------------------------------------------------------------- */
/*  Assign/delete timeout                                                     */
/* -------------------------------------------------------------------------- */

static void assign_timeout_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct behavior_dynamic_macro_data *data =
        CONTAINER_OF(delayable, struct behavior_dynamic_macro_data, assign_timeout_work);

    if (data->state == DM_STATE_PENDING_ASSIGN || data->state == DM_STATE_DELETE_PENDING ||
        data->state == DM_STATE_MOVE_PENDING
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
        || data->state == DM_STATE_PREVIEW_PENDING
#endif
    ) {
        LOG_DBG("Dynamic macro assign/delete/preview timed out");
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
        if (data->slots[slot_idx].event_count > 0 && !atomic_test_bit(data->pending_delete, slot_idx)) {
            feedback_slot_full(data, slot_idx);
            return;
        }
        atomic_clear_bit(data->pending_delete, slot_idx);
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
                atomic_set_bit(data->pending_delete, slot_idx);
                data->state = DM_STATE_IDLE;
                int rc = delete_slot_from_storage(data, slot_idx);
                if (rc) {
                    atomic_clear_bit(data->pending_delete, slot_idx);
                    return;
                }

                LOG_DBG("Queued slot %d for deletion", slot_idx);
                break;
            }

            data->slots[slot_idx].event_count = 0;
            dm_feedback_deleted(data, slot_idx);
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
        atomic_clear_bit(data->pending_delete, dst);
        data->slot_generation[dst]++;
        memcpy(&data->slots[dst], &data->slots[src], sizeof(struct dm_slot));

        atomic_clear_bit(data->pending_delete, src);
        data->slot_generation[src]++;
        memset(&data->slots[src], 0, sizeof(struct dm_slot));

        save_slot(data, dst);
        delete_slot_from_storage(data, src);

        data->move_source_slot = -1;
        LOG_DBG("Moved slot %d -> slot %d", src, dst);
        feedback_moved(data, src, dst);
        break;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    case DM_STATE_PREVIEW_PENDING:
        k_work_cancel_delayable(&data->assign_timeout_work);
        data->state = DM_STATE_IDLE;
        raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_PREVIEW_READY, slot_idx);
        LOG_DBG("Preview requested for slot %d", slot_idx);
        break;
#endif

    case DM_STATE_IDLE:
        if (slot_is_empty(data, slot_idx)) {
            LOG_DBG("Slot %d is empty, nothing to play", slot_idx);
            feedback_slot_empty(data, slot_idx);
            return;
        }
        data->state = DM_STATE_PLAYING;
        data->playback_slot = slot_idx;
        data->playback_event = 0;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
        raise_dm_state_changed(data, ZMK_DYNAMIC_MACRO_PLAY_STARTED, slot_idx);
#endif
        LOG_DBG("Playing slot %d (%d events)", slot_idx, data->slots[slot_idx].event_count);
        k_timer_start(&data->emit_timer, K_NO_WAIT, K_NO_WAIT);
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
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    case DM_PREVIEW:
        if (data->state == DM_STATE_IDLE) {
            data->state = DM_STATE_PREVIEW_PENDING;
            k_work_reschedule(&data->assign_timeout_work,
                              K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
        }
        return ZMK_BEHAVIOR_OPAQUE;
#endif
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

#define DM_DEVICE(inst) DEVICE_DT_INST_GET(inst),
const struct device *dm_devices[] = {DT_INST_FOREACH_STATUS_OKAY(DM_DEVICE)};
const size_t dm_devices_len = ARRAY_SIZE(dm_devices);

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
        rec->keycode = (uint16_t)ev->keycode;
        rec->implicit_mods = ev->implicit_modifiers;
        rec->explicit_mods = ev->explicit_modifiers;
        rec->pressed = ev->state;
        rec->_reserved = 0;
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
    k_work_init(&data->emit_work, emit_work_handler);
    k_timer_init(&data->emit_timer, emit_timer_handler, NULL);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
    dm_storage_init();
#endif
#if DM_TYPING_ENABLED
    data->feedback_post_save_slot = -1;
    data->status_current_slot = -1;
    data->preview_done = true;
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

/* -------------------------------------------------------------------------- */
/*  Query API for display widgets                                             */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)

static struct behavior_dynamic_macro_data *get_first_dm_data(void) {
    if (dm_devices_len == 0) {
        return NULL;
    }
    return dm_devices[0]->data;
}

bool dm_is_slot_empty(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        return true;
    }
    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data) {
        return true;
    }
    return slot_is_empty(data, slot_idx);
}

const struct dm_event *dm_get_slot_events(int slot_idx, uint32_t *count) {
    if (slot_idx < 0 || slot_idx >= MAX_SLOTS || !count) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data) {
        *count = 0;
        return NULL;
    }
    if (slot_is_empty(data, slot_idx)) {
        *count = 0;
        return NULL;
    }
    *count = data->slots[slot_idx].event_count;
    return data->slots[slot_idx].events;
}

int dm_get_preview_string(int slot_idx, char *buf, size_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';

    if (slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        return 0;
    }

    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data || slot_is_empty(data, slot_idx)) {
        return 0;
    }

#if DM_TYPING_ENABLED
    const struct dm_slot *slot = &data->slots[slot_idx];
    size_t pos = 0;
    uint8_t active_mods = 0;

    for (uint32_t i = 0; i < slot->event_count && pos < len - 1; i++) {
        const struct dm_event *ev = &slot->events[i];

        if (is_modifier_key(ev->usage_page, ev->keycode)) {
            uint8_t mod_bit = mod_bits[ev->keycode - 0xE0];
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
        char c;
        if (ev->usage_page == HID_USAGE_KEY &&
            printable_char_for_keycode(ev->keycode, (mods & MOD_SHIFT_MASK) != 0, &c)) {
            buf[pos++] = c;
        } else {
            const char *name = action_name(ev->usage_page, ev->keycode);
            size_t name_len = strlen(name);
            if (pos + name_len + 2 < len - 1) {
                buf[pos++] = '<';
                memcpy(&buf[pos], name, name_len);
                pos += name_len;
                buf[pos++] = '>';
            }
        }
    }

    buf[pos] = '\0';
    return (int)pos;
#else
    int n = snprintf(buf, len, "(%u events)", data->slots[slot_idx].event_count);
    return n < (int)len ? n : (int)len - 1;
#endif
}

int dm_get_used_nvs_slots(void) {
    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data) {
        return 0;
    }
    return filled_nvs_slot_count(data);
}

int dm_get_used_ram_slots(void) {
    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data) {
        return 0;
    }
    return filled_ram_slot_count(data);
}

int dm_get_total_nvs_slots(void) {
    return NVS_SLOTS;
}

int dm_get_total_ram_slots(void) {
    return RAM_SLOTS;
}

enum zmk_dynamic_macro_state dm_get_state(void) {
    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data) {
        return ZMK_DYNAMIC_MACRO_STATE_IDLE;
    }
    switch (data->state) {
    case DM_STATE_RECORDING:
        return ZMK_DYNAMIC_MACRO_STATE_RECORDING;
    case DM_STATE_PLAYING:
        return ZMK_DYNAMIC_MACRO_STATE_PLAYING;
    default:
        return ZMK_DYNAMIC_MACRO_STATE_IDLE;
    }
}

uint32_t dm_get_recording_event_count(void) {
    struct behavior_dynamic_macro_data *data = get_first_dm_data();
    if (!data || data->state != DM_STATE_RECORDING) {
        return 0;
    }
    return data->recording_buffer.event_count;
}

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
