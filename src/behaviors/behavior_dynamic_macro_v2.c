/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * behavior_dynamic_macro (new shell) — the thin wiring/dispatch layer.
 *
 * Owns nothing but wiring: it composes the deep modules (dm_machine, slot_store,
 * dm_feedback, dm_nvs, dm_events) into one dev->data, parses a binding into a
 * dm_command, runs the keymap-validation BUILD_ASSERTs and metadata, owns the
 * listener + its recording-suppression flag, and drives the co-located playback
 * emitter. State is written ONLY by dm_machine; persistence ordering lives ONLY
 * in slot_store; message formatting lives ONLY in dm_feedback/dm_render.
 *
 * Built only for the new stack (DM_NEW_STACK): until the step-8 cut-over the old
 * behavior_dynamic_macro.c is the live path and defines the same symbols; the
 * guard keeps the two out of one image. The native_sim parity harness defines
 * DM_NEW_STACK to reach this shell.
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/dynamic_macros.h>

#include <zmk-behavior-dynamic-macros/dm_kconfig.h>
#include <zmk-behavior-dynamic-macros/dm_machine.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>
#include <zmk-behavior-dynamic-macros/dm_shell.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>
#if DM_TYPING_ENABLED
#include <zmk-behavior-dynamic-macros/dm_feedback_pump.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
#include <zmk-behavior-dynamic-macros/dm_nvs.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if defined(DM_NEW_STACK) && DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define TAP_DELAY CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY

/* -------------------------------------------------------------------------- */
/*  Build-time validation (carried over verbatim — definition-of-done)        */
/* -------------------------------------------------------------------------- */

BUILD_ASSERT(MAX_EVENTS > 0, "Dynamic macros require at least 1 event per slot");
BUILD_ASSERT(NVS_SLOTS <= 16, "Dynamic macros support at most 16 NVS slots");
BUILD_ASSERT(RAM_SLOTS <= 48, "Dynamic macros support at most 48 RAM slots");
BUILD_ASSERT(MAX_SLOTS <= 64, "Dynamic macros support at most 64 total slots");
BUILD_ASSERT(sizeof(struct dm_event) == 8, "dm_event must be 8 bytes packed");

/* The single-instance assumption is anchored HERE, in one place (ADR-0002): the
 * storage backend, query resolution, and listener suppression all rely on it. */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "Only one zmk,behavior-dynamic-macro instance is supported. The "
             "per-instance scaffolding follows ZMK convention; the query API and "
             "recording suppression assume a single instance.");

#if MAX_SLOTS == 0
#warning "Dynamic macro has zero slots; all dynamic macro slot bindings are invalid"
#endif

#define DM_IS_DM_BINDING(idx, layer)                                                                \
    DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(layer, bindings, idx), zmk_behavior_dynamic_macro)

#define DM_VALIDATE_SLOT_CMD(idx, layer, command, limit, msg)                                       \
    COND_CODE_1(DM_IS_DM_BINDING(idx, layer),                                                       \
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) != command ||             \
                                  DT_PHA_BY_IDX(layer, bindings, idx, param2) < (limit),            \
                              msg);),                                                                \
                ())

#define DM_VALIDATE_CMD_RANGE(idx, layer)                                                           \
    COND_CODE_1(DM_IS_DM_BINDING(idx, layer),                                                       \
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) <= DM_TEST_RELOAD,        \
                              "Dynamic macro param1 is not a valid command (expected 0-12)");),     \
                ())

#define DM_VALIDATE_CMD_NO_PARAM2(idx, layer, command)                                              \
    COND_CODE_1(DM_IS_DM_BINDING(idx, layer),                                                       \
                (BUILD_ASSERT(DT_PHA_BY_IDX(layer, bindings, idx, param1) != command ||             \
                                  DT_PHA_BY_IDX(layer, bindings, idx, param2) == 0,                 \
                              #command " does not use param2 (must be 0)");),                        \
                ())

#define DM_VALIDATE_KEYMAP_BINDING(idx, layer)                                                      \
    DM_VALIDATE_CMD_RANGE(idx, layer)                                                               \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_REC)                                                   \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_STP)                                                   \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_DEL)                                                   \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_STATE)                                                 \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_MOV)                                                   \
    DM_VALIDATE_SLOT_CMD(idx, layer, DM_SLOT_NVS, NVS_SLOTS,                                        \
                         "DM_SLOT_NVS index exceeds configured NVS dynamic macro slots")            \
    DM_VALIDATE_SLOT_CMD(idx, layer, DM_SLOT_RAM, RAM_SLOTS,                                        \
                         "DM_SLOT_RAM index exceeds configured RAM dynamic macro slots")            \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_PREVIEW)                                               \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_FEEDBACK_INC)                                          \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_FEEDBACK_DEC)                                          \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_STYLE_TOGGLE)                                          \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_ERASE_TOGGLE)                                          \
    DM_VALIDATE_CMD_NO_PARAM2(idx, layer, DM_TEST_RELOAD)

#define DM_VALIDATE_KEYMAP_LAYER(layer)                                                             \
    COND_CODE_1(DT_NODE_HAS_PROP(layer, bindings),                                                  \
                (LISTIFY(DT_PROP_LEN(layer, bindings), DM_VALIDATE_KEYMAP_BINDING, (), layer)),     \
                ())

ZMK_KEYMAP_LAYERS_FOREACH(DM_VALIDATE_KEYMAP_LAYER)

/* -------------------------------------------------------------------------- */
/*  Behavior parameter metadata (carried over verbatim)                       */
/* -------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

#define DM_COMMAND_VALUE(name, command)                                                             \
    {.display_name = name, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = command}

static const struct behavior_parameter_value_metadata dm_param_rec[] = {
    DM_COMMAND_VALUE("Record", DM_REC)};
static const struct behavior_parameter_value_metadata dm_param_stop[] = {
    DM_COMMAND_VALUE("Stop", DM_STP)};
static const struct behavior_parameter_value_metadata dm_param_delete[] = {
    DM_COMMAND_VALUE("Delete", DM_DEL)};
static const struct behavior_parameter_value_metadata dm_param_state[] = {
    DM_COMMAND_VALUE("State", DM_STATE)};
static const struct behavior_parameter_value_metadata dm_param_move[] = {
    DM_COMMAND_VALUE("Move", DM_MOV)};
#if NVS_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_slot_nvs[] = {
    DM_COMMAND_VALUE("NVS Slot", DM_SLOT_NVS)};
#endif
#if RAM_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_slot_ram[] = {
    DM_COMMAND_VALUE("RAM Slot", DM_SLOT_RAM)};
#endif
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
static const struct behavior_parameter_value_metadata dm_param_preview[] = {
    DM_COMMAND_VALUE("Preview", DM_PREVIEW)};
#endif
#if DM_TYPING_ENABLED
static const struct behavior_parameter_value_metadata dm_param_feedback_inc[] = {
    DM_COMMAND_VALUE("Feedback+", DM_FEEDBACK_INC)};
static const struct behavior_parameter_value_metadata dm_param_feedback_dec[] = {
    DM_COMMAND_VALUE("Feedback-", DM_FEEDBACK_DEC)};
#endif
static const struct behavior_parameter_value_metadata dm_param_unused[] = {
    {.display_name = "Unused", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_NIL}};

#if NVS_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_nvs_slot_index[] = {
    {.display_name = "NVS slot index",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
     .range = {.min = 0, .max = NVS_SLOTS - 1}}};
#endif
#if RAM_SLOTS > 0
static const struct behavior_parameter_value_metadata dm_param_ram_slot_index[] = {
    {.display_name = "RAM slot index",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
     .range = {.min = 0, .max = RAM_SLOTS - 1}}};
#endif

static const struct behavior_parameter_metadata_set dm_parameter_metadata_sets[] = {
    {.param1_values_len = ARRAY_SIZE(dm_param_rec), .param1_values = dm_param_rec,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
    {.param1_values_len = ARRAY_SIZE(dm_param_stop), .param1_values = dm_param_stop,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
    {.param1_values_len = ARRAY_SIZE(dm_param_delete), .param1_values = dm_param_delete,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
    {.param1_values_len = ARRAY_SIZE(dm_param_state), .param1_values = dm_param_state,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
    {.param1_values_len = ARRAY_SIZE(dm_param_move), .param1_values = dm_param_move,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
#if NVS_SLOTS > 0
    {.param1_values_len = ARRAY_SIZE(dm_param_slot_nvs), .param1_values = dm_param_slot_nvs,
     .param2_values_len = ARRAY_SIZE(dm_param_nvs_slot_index),
     .param2_values = dm_param_nvs_slot_index},
#endif
#if RAM_SLOTS > 0
    {.param1_values_len = ARRAY_SIZE(dm_param_slot_ram), .param1_values = dm_param_slot_ram,
     .param2_values_len = ARRAY_SIZE(dm_param_ram_slot_index),
     .param2_values = dm_param_ram_slot_index},
#endif
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    {.param1_values_len = ARRAY_SIZE(dm_param_preview), .param1_values = dm_param_preview,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
#endif
#if DM_TYPING_ENABLED
    {.param1_values_len = ARRAY_SIZE(dm_param_feedback_inc), .param1_values = dm_param_feedback_inc,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
    {.param1_values_len = ARRAY_SIZE(dm_param_feedback_dec), .param1_values = dm_param_feedback_dec,
     .param2_values_len = ARRAY_SIZE(dm_param_unused), .param2_values = dm_param_unused},
#endif
};

static const struct behavior_parameter_metadata dm_parameter_metadata = {
    .sets_len = ARRAY_SIZE(dm_parameter_metadata_sets),
    .sets = dm_parameter_metadata_sets,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

/* -------------------------------------------------------------------------- */
/*  Single-instance resolution (dm_shell.h)                                   */
/* -------------------------------------------------------------------------- */

#define DM_DEVICE(inst) DEVICE_DT_INST_GET(inst),
const struct device *dm_devices[] = {DT_INST_FOREACH_STATUS_OKAY(DM_DEVICE)};
const size_t dm_devices_len = ARRAY_SIZE(dm_devices);

struct dm_inst *dm_shell_instance(void) {
    if (dm_devices_len == 0) {
        return NULL;
    }
    return dm_devices[0]->data;
}

/* -------------------------------------------------------------------------- */
/*  Recording-suppression ownership (the shell owns the flag + the listener)  */
/* -------------------------------------------------------------------------- */

/* The one inline through which emitters set/clear suppression — feedback typing
 * and playback both flip it via this so it lives in exactly one owner. */
static void dm_set_suppress(void *ctx, bool suppress) {
    struct dm_inst *inst = ctx;
    inst->suppress_recording = suppress;
}

#if DM_TYPING_ENABLED
/* The pump emits feedback keystrokes through this — a HID keyboard press/release.
 * Suppression is already held by the pump while it types, so these are not
 * recorded. */
static void dm_raise_feedback_keycode(void *ctx, uint16_t keycode, uint8_t mods, bool pressed) {
    (void)ctx;
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = HID_USAGE_KEY,
        .keycode = keycode,
        .implicit_modifiers = mods,
        .explicit_modifiers = 0,
        .state = pressed,
        .timestamp = k_uptime_get(),
    });
}
#endif

#if DM_TYPING_ENABLED && IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
/* Adapt the pump's save_knobs hook (ctx-carrying) to the file-scoped dm_nvs entry
 * (single-instance, no handle). */
static void dm_save_knobs(void *ctx, uint8_t level, uint8_t style, bool erase) {
    (void)ctx;
    dm_nvs_save_knobs(level, style, erase);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
/* PLAY_FINISHED is the one notification the machine does not raise (it has no
 * playback-completion transition of its own); the playback emitter raises it
 * directly. Mirrors the DM_EVT_* ordering in dm_machine.c. */
#define DM_EVT_PLAY_FINISHED 6
#endif

/* -------------------------------------------------------------------------- */
/*  Playback emitter (co-located primitive: replay a slot's dm_events)        */
/* -------------------------------------------------------------------------- */

static void playback_finish(struct dm_inst *inst) {
    int slot = inst->playback_slot;
    inst->playback_slot = -1;
    slot_store_clear_playing(&inst->store);
    k_timer_stop(&inst->playback_timer);
    /* Settle the machine to IDLE FIRST, then raise PLAY_FINISHED — the event's
     * coarse state field is derived from the machine, and the old emit handler
     * raised it after writing state=IDLE, so the widget sees IDLE. */
    dm_machine_play_finished(&inst->machine);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    dm_events_raise(inst, DM_EVT_PLAY_FINISHED, slot);
#endif
}

static void playback_work_handler(struct k_work *work) {
    struct dm_inst *inst = CONTAINER_OF(work, struct dm_inst, playback_work);

    if (inst->playback_slot < 0) {
        k_timer_stop(&inst->playback_timer);
        return;
    }
    const struct dm_slot *slot = slot_store_get(&inst->store, inst->playback_slot);
    if (slot == NULL || inst->playback_event >= slot->event_count) {
        playback_finish(inst);
        return;
    }

    const struct dm_event *ev = &slot->events[inst->playback_event++];
    struct zmk_keycode_state_changed kc = {
        .usage_page = ev->usage_page,
        .keycode = ev->keycode,
        .implicit_modifiers = ev->implicit_mods,
        .explicit_modifiers = ev->explicit_mods,
        .state = ev->pressed,
        .timestamp = k_uptime_get(),
    };

    inst->suppress_recording = true;
    raise_zmk_keycode_state_changed(kc);
    inst->suppress_recording = false;

    if (inst->playback_event >= slot->event_count) {
        playback_finish(inst);
    } else {
        k_timer_start(&inst->playback_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
    }
}

static void playback_timer_handler(struct k_timer *timer) {
    struct dm_inst *inst = CONTAINER_OF(timer, struct dm_inst, playback_timer);
    k_work_submit(&inst->playback_work);
}

/* -------------------------------------------------------------------------- */
/*  Assign/move/delete/preview timeout                                        */
/* -------------------------------------------------------------------------- */

static void timeout_handler(struct k_work *work) {
    struct k_work_delayable *d = k_work_delayable_from_work(work);
    struct dm_inst *inst = CONTAINER_OF(d, struct dm_inst, timeout_work);
    dm_machine_timeout(&inst->machine);
}

/* The machine drives these through its vtable whenever it enters / resolves a
 * *_PENDING state, so the timer tracks the pending state wherever the transition
 * originates (command, listener overflow, or a typing_finished up-call inside the
 * pump) — no polling needed. */
static void cb_arm_timeout(void *ctx) {
    struct dm_inst *inst = ctx;
    k_work_reschedule(&inst->timeout_work,
                      K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT));
}

static void cb_cancel_timeout(void *ctx) {
    struct dm_inst *inst = ctx;
    k_work_cancel_delayable(&inst->timeout_work);
}

/* -------------------------------------------------------------------------- */
/*  Machine callback vtable — store_* + notify + speak_* (the latter funnel    */
/*  into dm_feedback_speak)                                                    */
/* -------------------------------------------------------------------------- */

/* store_* adapters: thin wrappers over slot_store, except mark_playing also kicks
 * the playback emitter (the machine wrote PLAYING; the emitter must start). */
static dm_result cb_store_move(void *ctx, int src, int dst) {
    struct dm_inst *inst = ctx;
    return slot_store_move(&inst->store, src, dst);
}
static dm_result cb_store_delete(void *ctx, int idx) {
    struct dm_inst *inst = ctx;
    return slot_store_delete(&inst->store, idx);
}
static dm_result cb_store_persist(void *ctx, int idx) {
    struct dm_inst *inst = ctx;
    return slot_store_persist(&inst->store, idx);
}
static dm_result cb_store_draft_commit(void *ctx, int dst) {
    struct dm_inst *inst = ctx;
    return slot_store_draft_commit(&inst->store, dst);
}
static dm_result cb_store_draft_chain(void *ctx, int src) {
    struct dm_inst *inst = ctx;
    return slot_store_draft_chain(&inst->store, src);
}
static int cb_store_draft_count(void *ctx) {
    struct dm_inst *inst = ctx;
    return (int)slot_store_draft_count(&inst->store);
}
static bool cb_store_is_empty(void *ctx, int idx) {
    struct dm_inst *inst = ctx;
    return slot_store_is_empty(&inst->store, idx);
}
static void cb_store_draft_reset(void *ctx) {
    struct dm_inst *inst = ctx;
    slot_store_draft_reset(&inst->store);
}
static void cb_store_mark_playing(void *ctx, int idx) {
    struct dm_inst *inst = ctx;
    slot_store_mark_playing(&inst->store, idx);
    inst->playback_slot = idx;
    inst->playback_event = 0;
    k_timer_start(&inst->playback_timer, K_NO_WAIT, K_NO_WAIT);
}
static void cb_store_clear_playing(void *ctx) {
    struct dm_inst *inst = ctx;
    slot_store_clear_playing(&inst->store);
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
static void cb_notify(void *ctx, int event, int slot) {
    dm_events_raise(ctx, event, slot);
}
#else
static void cb_notify(void *ctx, int event, int slot) {
    (void)ctx; (void)event; (void)slot;
}
#endif

#if DM_TYPING_ENABLED
/* speak_* adapters: build a dm_feedback_spec and funnel into the pump. The
 * machine has already written state + parked the return-state, so these are pure
 * presentation. ctx is the dm_inst *; the pump is &inst->feedback. */
#define FB(inst_ctx) (&((struct dm_inst *)(inst_ctx))->feedback)

static void sp_rec(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_REC, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_stop(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_STOP, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_no_rec(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_NO_REC, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_saved(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_SAVED, .slot = slot, .slot2 = -1, .show_preview = true};
    dm_feedback_speak(FB(c), &s);
}
static void sp_deleted(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_DELETED, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_slot_empty(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_SLOT_EMPTY, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_slot_full(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_SLOT_FULL, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_chain_insert(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_CHAIN_INSERT, .slot = slot, .slot2 = -1, .show_preview = true};
    dm_feedback_speak(FB(c), &s);
}
static void sp_chain_empty(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_CHAIN_EMPTY, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_chain_no_room(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_CHAIN_NO_ROOM, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_overflow(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_OVERFLOW, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_move_prompt(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_MOVE_PROMPT, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_move_src(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_MOVE_SRC, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_move_cancel(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_MOVE_CANCEL, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_moved(void *c, int src, int dst) {
    dm_feedback_spec s = {.kind = DM_FB_MOVED, .slot = src, .slot2 = dst};
    dm_feedback_speak(FB(c), &s);
}
static void sp_save_qfull(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_SAVE_QFULL, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_delete_qfull(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_DELETE_QFULL, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_status(void *c) {
    dm_feedback_spec s = {.kind = DM_FB_STATUS_HEADER, .slot = -1, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_preview(void *c, int slot) {
    /* PREVIEW types nothing (it is a query-API readout); the machine raised the
     * widget notification already, so finish instantly. */
    (void)slot;
    dm_machine_typing_finished(&((struct dm_inst *)c)->machine);
}
static void sp_async_deleted(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_DELETED, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_async_save_failed(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_SAVE_FAILED, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_async_delete_failed(void *c, int slot) {
    dm_feedback_spec s = {.kind = DM_FB_DELETE_FAILED, .slot = slot, .slot2 = -1};
    dm_feedback_speak(FB(c), &s);
}
static void sp_erase(void *c) {
    /* erase emission is armed by the erase scheduler before erase_due returns. */
    (void)c;
}
#else /* !DM_TYPING_ENABLED — feedback compiled out: every speak finishes now */
static void sp_finish(void *c) { dm_machine_typing_finished(&((struct dm_inst *)c)->machine); }
static void sp_finish_slot(void *c, int s) { (void)s; sp_finish(c); }
static void sp_finish_2(void *c, int a, int b) { (void)a; (void)b; sp_finish(c); }
#endif /* DM_TYPING_ENABLED */

static void wire_callbacks(struct dm_inst *inst) {
    dm_machine_callbacks *cb = &inst->callbacks;
    memset(cb, 0, sizeof(*cb));

    cb->ctx = inst;
    cb->store_move = cb_store_move;
    cb->store_delete = cb_store_delete;
    cb->store_persist = cb_store_persist;
    cb->store_draft_commit = cb_store_draft_commit;
    cb->store_draft_chain = cb_store_draft_chain;
    cb->store_draft_count = cb_store_draft_count;
    cb->store_is_empty = cb_store_is_empty;
    cb->store_draft_reset = cb_store_draft_reset;
    cb->store_mark_playing = cb_store_mark_playing;
    cb->store_clear_playing = cb_store_clear_playing;
    cb->notify = cb_notify;
    cb->arm_timeout = cb_arm_timeout;
    cb->cancel_timeout = cb_cancel_timeout;

#if DM_TYPING_ENABLED
    cb->speak_rec = sp_rec;
    cb->speak_stop = sp_stop;
    cb->speak_no_recording = sp_no_rec;
    cb->speak_saved = sp_saved;
    cb->speak_deleted = sp_deleted;
    cb->speak_slot_empty = sp_slot_empty;
    cb->speak_slot_full = sp_slot_full;
    cb->speak_chain_insert = sp_chain_insert;
    cb->speak_chain_empty = sp_chain_empty;
    cb->speak_chain_no_room = sp_chain_no_room;
    cb->speak_overflow = sp_overflow;
    cb->speak_move_prompt = sp_move_prompt;
    cb->speak_move_source_selected = sp_move_src;
    cb->speak_move_cancelled = sp_move_cancel;
    cb->speak_moved = sp_moved;
    cb->speak_save_queue_full = sp_save_qfull;
    cb->speak_delete_queue_full = sp_delete_qfull;
    cb->speak_status = sp_status;
    cb->speak_preview = sp_preview;
    cb->speak_async_deleted = sp_async_deleted;
    cb->speak_async_save_failed = sp_async_save_failed;
    cb->speak_async_delete_failed = sp_async_delete_failed;
    cb->speak_erase = sp_erase;
#else
    cb->speak_rec = sp_finish;
    cb->speak_stop = sp_finish;
    cb->speak_no_recording = sp_finish;
    cb->speak_saved = sp_finish_slot;
    cb->speak_deleted = sp_finish_slot;
    cb->speak_slot_empty = sp_finish_slot;
    cb->speak_slot_full = sp_finish_slot;
    cb->speak_chain_insert = sp_finish_slot;
    cb->speak_chain_empty = sp_finish_slot;
    cb->speak_chain_no_room = sp_finish_slot;
    cb->speak_overflow = sp_finish;
    cb->speak_move_prompt = sp_finish;
    cb->speak_move_source_selected = sp_finish_slot;
    cb->speak_move_cancelled = sp_finish;
    cb->speak_moved = sp_finish_2;
    cb->speak_save_queue_full = sp_finish_slot;
    cb->speak_delete_queue_full = sp_finish_slot;
    cb->speak_status = sp_finish;
    cb->speak_preview = sp_finish_slot;
    cb->speak_async_deleted = sp_finish_slot;
    cb->speak_async_save_failed = sp_finish_slot;
    cb->speak_async_delete_failed = sp_finish_slot;
    cb->speak_erase = sp_finish;
#endif
}

/* -------------------------------------------------------------------------- */
/*  Binding dispatch                                                          */
/* -------------------------------------------------------------------------- */

static void dispatch(struct dm_inst *inst, dm_command cmd, int param) {
    dm_machine_command(&inst->machine, cmd, param);
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct dm_inst *inst = dev->data;

#if DM_TYPING_ENABLED
    /* any DM binding press cancels a scheduled / in-progress auto-erase */
    dm_feedback_pump_cancel_erase(&inst->feedback);
#endif

    switch (binding->param1) {
    case DM_REC:
        dispatch(inst, DM_CMD_REC, 0);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STP:
        dispatch(inst, DM_CMD_STP, 0);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_DEL:
        dispatch(inst, DM_CMD_DEL, 0);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_MOV:
        dispatch(inst, DM_CMD_MOV, 0);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STATE:
        dispatch(inst, DM_CMD_STATE, 0);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_SLOT_NVS:
        if (binding->param2 < 0 || binding->param2 >= NVS_SLOTS) {
            LOG_ERR("NVS slot index %d out of range (max %d)", binding->param2, NVS_SLOTS - 1);
            return -EINVAL;
        }
        dispatch(inst, DM_CMD_SLOT, binding->param2);
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_SLOT_RAM:
        if (binding->param2 < 0 || binding->param2 >= RAM_SLOTS) {
            LOG_ERR("RAM slot index %d out of range (max %d)", binding->param2, RAM_SLOTS - 1);
            return -EINVAL;
        }
        dispatch(inst, DM_CMD_SLOT, NVS_SLOTS + binding->param2);
        return ZMK_BEHAVIOR_OPAQUE;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    case DM_PREVIEW:
        dispatch(inst, DM_CMD_PREVIEW, 0);
        return ZMK_BEHAVIOR_OPAQUE;
#endif
#if DM_TYPING_ENABLED
    /*
     * Knob commands: the machine gates them IDLE-only (legality) and parks
     * TYPING_FEEDBACK -> IDLE via do_knob, but the change + persist + confirmation
     * are feedback-internal (the deferred seam). We run the knob effect only if the
     * machine actually accepted the command — i.e. it is now in TYPING_FEEDBACK; an
     * IGNORED command leaves the prior state untouched and we do nothing.
     */
    case DM_FEEDBACK_INC:
        dm_machine_command(&inst->machine, DM_CMD_FEEDBACK_INC, 0);
        if (dm_machine_state(&inst->machine) == DM_STATE_TYPING_FEEDBACK) {
            dm_feedback_knob_level(&inst->feedback, 1);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_FEEDBACK_DEC:
        dm_machine_command(&inst->machine, DM_CMD_FEEDBACK_DEC, 0);
        if (dm_machine_state(&inst->machine) == DM_STATE_TYPING_FEEDBACK) {
            dm_feedback_knob_level(&inst->feedback, -1);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_STYLE_TOGGLE:
        dm_machine_command(&inst->machine, DM_CMD_STYLE_TOGGLE, 0);
        if (dm_machine_state(&inst->machine) == DM_STATE_TYPING_FEEDBACK) {
            dm_feedback_knob_style_toggle(&inst->feedback);
        }
        return ZMK_BEHAVIOR_OPAQUE;
    case DM_ERASE_TOGGLE:
        dm_machine_command(&inst->machine, DM_CMD_ERASE_TOGGLE, 0);
        if (dm_machine_state(&inst->machine) == DM_STATE_TYPING_FEEDBACK) {
            dm_feedback_knob_erase_toggle(&inst->feedback);
        }
        return ZMK_BEHAVIOR_OPAQUE;
#endif
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
    case DM_TEST_RELOAD:
        /* IDLE-only, like any other; reload is dm_nvs mechanics, not a machine
         * transition (the machine IGNOREs TEST_RELOAD). */
        if (dm_machine_state(&inst->machine) != DM_STATE_IDLE) {
            return ZMK_BEHAVIOR_OPAQUE;
        }
        dm_nvs_test_reload();
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
/*  Event listener: capture keycodes during recording                         */
/* -------------------------------------------------------------------------- */

static int dm_event_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* single-instance: any instance suppressing means the emitted keystrokes are
     * our own playback/feedback output — do not record them. */
    for (size_t i = 0; i < dm_devices_len; i++) {
        struct dm_inst *inst = dm_devices[i]->data;
        if (inst->suppress_recording) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    }

#if DM_TYPING_ENABLED
    /* a real keycode cancels a scheduled / in-progress auto-erase */
    for (size_t i = 0; i < dm_devices_len; i++) {
        struct dm_inst *inst = dm_devices[i]->data;
        dm_feedback_pump_cancel_erase(&inst->feedback);
    }
#endif

    for (size_t i = 0; i < dm_devices_len; i++) {
        struct dm_inst *inst = dm_devices[i]->data;
        if (dm_machine_state(&inst->machine) != DM_STATE_RECORDING) {
            continue;
        }

        struct dm_event rec = {
            .usage_page = ev->usage_page,
            .keycode = (uint16_t)ev->keycode,
            .implicit_mods = ev->implicit_modifiers,
            .explicit_mods = ev->explicit_modifiers,
            .pressed = ev->state,
            ._reserved = 0,
        };
        if (!slot_store_draft_append(&inst->store, &rec)) {
            /* draft full: the overflow is a machine event so the RECORDING ->
             * PENDING_ASSIGN transition stays a machine write. */
            LOG_WRN("Dynamic macro recording buffer full (%d events)", MAX_EVENTS);
            dispatch(inst, DM_CMD_OVERFLOW, 0);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dynamic_macro, dm_event_listener);
ZMK_SUBSCRIPTION(dynamic_macro, zmk_keycode_state_changed);

/* -------------------------------------------------------------------------- */
/*  Initialization                                                           */
/* -------------------------------------------------------------------------- */

static int behavior_dynamic_macro_init(const struct device *dev) {
    struct dm_inst *inst = dev->data;

    memset(inst, 0, sizeof(*inst));
    inst->dev = dev;
    inst->playback_slot = -1;

    const dm_nvs_sink *sink = NULL;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
    sink = dm_nvs_sink_get();
#endif
    slot_store_init(&inst->store, sink);

    wire_callbacks(inst);
    dm_machine_init(&inst->machine, &inst->store, &inst->callbacks);

    k_work_init_delayable(&inst->timeout_work, timeout_handler);
    k_timer_init(&inst->playback_timer, playback_timer_handler, NULL);
    k_work_init(&inst->playback_work, playback_work_handler);

#if DM_TYPING_ENABLED
    dm_feedback_config fcfg = {
        .machine = &inst->machine,
        .store = &inst->store,
        .locale = (dm_locale)DM_LOCALE,
        .status_detail = DM_STATUS_DETAIL,
        .nvs_slots = NVS_SLOTS,
        .max_slots = MAX_SLOTS,
        .default_level = DM_FEEDBACK_LEVEL,
        .default_style = DM_FEEDBACK_STYLE,
        .default_erase = IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE),
        .raise_keycode = dm_raise_feedback_keycode,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
        .save_knobs = dm_save_knobs,
#else
        .save_knobs = NULL,
#endif
        .set_suppress = dm_set_suppress,
        .ctx = inst,
    };
    dm_feedback_pump_init(&inst->feedback, &fcfg);
#endif

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
    {
        const struct behavior_dynamic_macro_config *config = dev->config;
        dm_nvs_init(&inst->store, &inst->machine,
#if DM_TYPING_ENABLED
                    &inst->feedback,
#else
                    NULL,
#endif
                    config->settings_name);
    }
#endif

    LOG_DBG("Dynamic macro behavior (v2) initialized (%d slots, %d max events)", MAX_SLOTS,
            MAX_EVENTS);
    return 0;
}

#define DM_INST(n)                                                                                  \
    static struct dm_inst dm_inst_##n = {};                                                         \
    static const struct behavior_dynamic_macro_config behavior_dynamic_macro_config_##n = {         \
        .settings_name = DEVICE_DT_NAME(DT_DRV_INST(n)),                                            \
    };                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_dynamic_macro_init, NULL, &dm_inst_##n,                     \
                            &behavior_dynamic_macro_config_##n, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                     \
                            &behavior_dynamic_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DM_INST)

#endif /* DM_NEW_STACK && DT_HAS_COMPAT_STATUS_OKAY */
