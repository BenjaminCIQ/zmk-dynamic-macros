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
#include <zmk-behavior-dynamic-macros/dm_internal.h>
#include <zmk-behavior-dynamic-macros/dm_feedback.h>
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
#include <zmk-behavior-dynamic-macros/events/dynamic_macro_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* -------------------------------------------------------------------------- */
/*  Constants and types                                                       */
/* -------------------------------------------------------------------------- */

#define TAP_DELAY CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY

BUILD_ASSERT(MAX_EVENTS > 0, "Dynamic macros require at least 1 event per slot");
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
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) <= DM_TEST_RELOAD,      \
                              "Dynamic macro param1 is not a valid command (expected 0-12)");),   \
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
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_PREVIEW)                                             \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_FEEDBACK_INC)                                        \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_FEEDBACK_DEC)                                       \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_STYLE_TOGGLE)                                       \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_ERASE_TOGGLE)                                       \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_TEST_RELOAD)

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
#if DM_TYPING_ENABLED
static const struct behavior_parameter_value_metadata dm_param_feedback_inc[] = {
    DM_COMMAND_VALUE("Feedback+", DM_FEEDBACK_INC),
};
static const struct behavior_parameter_value_metadata dm_param_feedback_dec[] = {
    DM_COMMAND_VALUE("Feedback-", DM_FEEDBACK_DEC),
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
#if DM_TYPING_ENABLED
    {
        .param1_values_len = ARRAY_SIZE(dm_param_feedback_inc),
        .param1_values = dm_param_feedback_inc,
        .param2_values_len = ARRAY_SIZE(dm_param_unused),
        .param2_values = dm_param_unused,
    },
    {
        .param1_values_len = ARRAY_SIZE(dm_param_feedback_dec),
        .param1_values = dm_param_feedback_dec,
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

/* -------------------------------------------------------------------------- */
/*  NVS Persistence                                                           */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)

void dm_save_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    dm_storage_save_slot(data, slot_idx);
}

int dm_delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx) {
    return dm_storage_delete_slot(data, slot_idx);
}

#else /* !CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */

void dm_save_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)data;
    (void)slot_idx;
}

int dm_delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx) {
    (void)data;
    (void)slot_idx;
    return 0;
}

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */

/* -------------------------------------------------------------------------- */
/*  Event notification                                                        */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
void dm_raise_state_changed(struct behavior_dynamic_macro_data *data,
                            int event, int slot) {
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
            dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_PLAY_FINISHED, data->playback_slot);
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
            dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_PLAY_FINISHED, data->playback_slot);
#endif
            data->playback_slot = -1;
            k_timer_stop(&data->emit_timer);
        } else {
            k_timer_start(&data->emit_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
        }
        return;
    }

#if DM_TYPING_ENABLED
    if (data->state == DM_STATE_TYPING_FEEDBACK || data->state == DM_STATE_TYPING_ERASE) {
        /* Refill ring from streaming preview if needed */
        if (ring_empty(data) && !data->preview_done) {
            bool more = render_slot_contents_stream(data);
            if (!more) {
                data->preview_done = true;
                if (data->needs_suffix) {
                    dm_feedback_preview_suffix(data);
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
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE)
            if (data->state == DM_STATE_TYPING_FEEDBACK && ev->keycode != 0x28) {
                data->erase_char_count++;
            }
#endif
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
    if (data->state == DM_STATE_PLAYING || data->state == DM_STATE_TYPING_FEEDBACK ||
        data->state == DM_STATE_TYPING_ERASE || data->state == DM_STATE_DELETE_PENDING ||
        data->state == DM_STATE_MOVE_PENDING) {
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
            LOG_ERR("Assign rejected: slot %d is occupied (%u events)", slot_idx,
                    (unsigned int)data->slots[slot_idx].event_count);
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
                int rc = dm_delete_slot_from_storage(data, slot_idx);
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

        dm_save_slot(data, dst);
        dm_delete_slot_from_storage(data, src);

        data->move_source_slot = -1;
        LOG_DBG("Moved slot %d -> slot %d", src, dst);
        feedback_moved(data, src, dst);
        break;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    case DM_STATE_PREVIEW_PENDING:
        k_work_cancel_delayable(&data->assign_timeout_work);
        data->state = DM_STATE_IDLE;
        dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_PREVIEW_READY, slot_idx);
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
        dm_raise_state_changed(data, ZMK_DYNAMIC_MACRO_PLAY_STARTED, slot_idx);
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

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE)
    dm_feedback_cancel_erase(data);
#endif

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
#if DM_TYPING_ENABLED
    case DM_FEEDBACK_INC:
        cmd_feedback_adjust(data, 1);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_FEEDBACK_DEC:
        cmd_feedback_adjust(data, -1);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STYLE_TOGGLE:
        cmd_style_toggle(data);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_ERASE_TOGGLE:
        cmd_erase_toggle(data);
        return ZMK_BEHAVIOR_OPAQUE;
#endif
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
    case DM_TEST_RELOAD:
        if (data->state != DM_STATE_IDLE) {
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("Test reload: flushing storage and reloading from NVS");
        dm_storage_flush();
        dm_storage_test_reload();
        LOG_DBG("Test reload: complete, NVS slots restored");
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

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE)
    for (size_t i = 0; i < ARRAY_SIZE(dm_devices); i++) {
        struct behavior_dynamic_macro_data *data = dm_devices[i]->data;
        dm_feedback_cancel_erase(data);
    }
#endif

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
    data->current_feedback_level = DM_FEEDBACK_LEVEL;
    data->current_feedback_style = DM_FEEDBACK_STYLE;
    data->auto_erase_enabled = IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE);
    data->feedback_post_save_slot = -1;
    data->status_current_slot = -1;
    data->preview_done = true;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE)
    dm_feedback_erase_init(data);
#endif
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
        char c;
        if (ev->usage_page == HID_USAGE_KEY &&
            printable_char_for_keycode(ev->keycode, (mods & MOD_SHIFT_MASK) != 0, &c)) {
            buf[pos++] = c;
        } else {
            size_t needed = token_size(mods, ev->usage_page, ev->keycode);
            if (pos + needed < len) {
                pos = render_token_to_buf(buf, pos, len, mods, ev->usage_page, ev->keycode);
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
