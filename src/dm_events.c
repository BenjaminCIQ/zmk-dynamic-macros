/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_events — notifications + the read-only query projection.
 *
 * Raises zmk_dynamic_macro_state_changed and implements the dm_get_* widget API
 * as a projection over the single instance's slot_store + machine. Widgets ask by
 * slot index, not by device, so the single-instance assumption is resolved HERE,
 * in one place, anchored to the BUILD_ASSERT(<=1) at the behavior-shell wiring
 * site. The string/number projection delegates to the pure dm_query; this file is
 * only the Zephyr edge (event raising, instance resolution, counts over the
 * store).
 *
 * Compiled only under EVENTS.
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zmk-behavior-dynamic-macros/dm_kconfig.h>
#include <zmk-behavior-dynamic-macros/dm_query.h>
#include <zmk-behavior-dynamic-macros/dm_shell.h>
#include <zmk-behavior-dynamic-macros/events/dynamic_macro_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)

/* Machine notify codes — mirror the DM_EVT_* in dm_machine.c. The machine speaks
 * these (an int) so it stays Zephyr-free; this is the one place they are turned
 * into the widget event enum. Kept as an explicit map because the two orderings
 * deliberately diverge past PREVIEW_READY. */
enum {
    DM_EVT_RECORDING_STARTED = 0,
    DM_EVT_RECORDING_STOPPED,
    DM_EVT_SAVED,
    DM_EVT_DELETED,
    DM_EVT_MOVED,
    DM_EVT_PLAY_STARTED,
    DM_EVT_PLAY_FINISHED,
    DM_EVT_PREVIEW_READY,
    DM_EVT_ERROR_NO_RECORDING,
    DM_EVT_ERROR_SLOT_EMPTY,
    DM_EVT_ERROR_OVERFLOW,
    DM_EVT_ERROR_SAVE_FAILED,
    DM_EVT_ERROR_DELETE_FAILED,
    DM_EVT_ERROR_QUEUE_FULL,
};

static enum zmk_dynamic_macro_event_type map_event(int machine_event) {
    switch (machine_event) {
    case DM_EVT_RECORDING_STARTED:  return ZMK_DYNAMIC_MACRO_RECORDING_STARTED;
    case DM_EVT_RECORDING_STOPPED:  return ZMK_DYNAMIC_MACRO_RECORDING_STOPPED;
    case DM_EVT_SAVED:              return ZMK_DYNAMIC_MACRO_SAVED;
    case DM_EVT_DELETED:            return ZMK_DYNAMIC_MACRO_DELETED;
    case DM_EVT_MOVED:              return ZMK_DYNAMIC_MACRO_MOVED;
    case DM_EVT_PLAY_STARTED:       return ZMK_DYNAMIC_MACRO_PLAY_STARTED;
    case DM_EVT_PLAY_FINISHED:      return ZMK_DYNAMIC_MACRO_PLAY_FINISHED;
    case DM_EVT_PREVIEW_READY:      return ZMK_DYNAMIC_MACRO_PREVIEW_READY;
    case DM_EVT_ERROR_NO_RECORDING: return ZMK_DYNAMIC_MACRO_ERROR_NO_RECORDING;
    case DM_EVT_ERROR_SLOT_EMPTY:   return ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY;
    case DM_EVT_ERROR_OVERFLOW:     return ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW;
    case DM_EVT_ERROR_SAVE_FAILED:  return ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED;
    case DM_EVT_ERROR_DELETE_FAILED:return ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED;
    case DM_EVT_ERROR_QUEUE_FULL:   return ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL;
    default:                        return ZMK_DYNAMIC_MACRO_ERROR_NO_RECORDING;
    }
}

static enum zmk_dynamic_macro_state coarse_state(struct dm_inst *inst) {
    switch (dm_machine_state(&inst->machine)) {
    case DM_STATE_RECORDING: return ZMK_DYNAMIC_MACRO_STATE_RECORDING;
    case DM_STATE_PLAYING:   return ZMK_DYNAMIC_MACRO_STATE_PLAYING;
    default:                 return ZMK_DYNAMIC_MACRO_STATE_IDLE;
    }
}

void dm_events_raise(struct dm_inst *inst, int machine_event, int slot) {
    enum zmk_dynamic_macro_event_type ev = map_event(machine_event);

    LOG_DBG("dm_event: type=%d slot=%d", (int)ev, slot);

    raise_zmk_dynamic_macro_state_changed((struct zmk_dynamic_macro_state_changed){
        .state = coarse_state(inst),
        .event = ev,
        .slot = slot,
        .slot_is_nvs = slot >= 0 ? slot_is_nvs(slot) : false,
    });
}

/* ---- the dm_get_* widget query API ---------------------------------------- */
/*
 * Built only for the new stack (the parity harness / post-cutover firmware).
 * The old behavior_dynamic_macro.c still defines these symbols on the live path;
 * DM_NEW_STACK selects the new projection so the two never collide in one image.
 */
#if defined(DM_NEW_STACK)

static dm_render_slot_view view_for(slot_store *store, int slot_idx) {
    const struct dm_slot *s = slot_store_get(store, slot_idx);
    if (!s) {
        return (dm_render_slot_view){.event_count = 0, .events = NULL};
    }
    return (dm_render_slot_view){.event_count = s->event_count, .events = s->events};
}

bool dm_is_slot_empty(int slot_idx) {
    struct dm_inst *inst = dm_shell_instance();
    if (!inst || slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        return true;
    }
    return slot_store_is_empty(&inst->store, slot_idx);
}

const struct dm_event *dm_get_slot_events(int slot_idx, uint32_t *count) {
    if (slot_idx < 0 || slot_idx >= MAX_SLOTS || !count) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    struct dm_inst *inst = dm_shell_instance();
    const struct dm_slot *s = inst ? slot_store_get(&inst->store, slot_idx) : NULL;
    if (!s) {
        *count = 0;
        return NULL;
    }
    *count = s->event_count;
    return s->events;
}

int dm_get_preview_string(int slot_idx, char *buf, size_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';

    struct dm_inst *inst = dm_shell_instance();
    if (!inst || slot_idx < 0 || slot_idx >= MAX_SLOTS) {
        return 0;
    }
    const struct dm_slot *s = slot_store_get(&inst->store, slot_idx);
    if (!s) {
        return 0;
    }

    dm_render_slot_view view = view_for(&inst->store, slot_idx);
    return dm_query_preview_string(&view, DM_LOCALE, s->event_count,
                                   DM_TYPING_ENABLED, buf, len);
}

int dm_get_used_nvs_slots(void) {
    struct dm_inst *inst = dm_shell_instance();
    return inst ? slot_store_count(&inst->store, DM_SLOT_CLASS_NVS) : 0;
}

int dm_get_used_ram_slots(void) {
    struct dm_inst *inst = dm_shell_instance();
    return inst ? slot_store_count(&inst->store, DM_SLOT_CLASS_RAM) : 0;
}

int dm_get_total_nvs_slots(void) {
    return NVS_SLOTS;
}

int dm_get_total_ram_slots(void) {
    return RAM_SLOTS;
}

enum zmk_dynamic_macro_state dm_get_state(void) {
    struct dm_inst *inst = dm_shell_instance();
    return inst ? coarse_state(inst) : ZMK_DYNAMIC_MACRO_STATE_IDLE;
}

uint32_t dm_get_recording_event_count(void) {
    struct dm_inst *inst = dm_shell_instance();
    if (!inst || dm_machine_state(&inst->machine) != DM_STATE_RECORDING) {
        return 0;
    }
    return slot_store_draft_count(&inst->store);
}

#endif /* DM_NEW_STACK */

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS */
