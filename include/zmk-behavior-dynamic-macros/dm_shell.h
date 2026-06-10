/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_shell — the per-instance aggregate that the behavior driver composes, plus
 * the single-instance resolution the query/notification shell shares.
 *
 * Composes the modules rather than owning their fields: each sub-module
 * (dm_machine, slot_store, dm_feedback) keeps its own private struct and is
 * embedded here only so dev->data is one allocation. The shell owns just the
 * wiring the modules cannot: the listener suppression flag, the assign/move
 * timeout work, and the playback emitter state.
 *
 * Zephyr-coupled: built only in the firmware build, never the host unit loop.
 */

#ifndef DM_SHELL_H
#define DM_SHELL_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zmk-behavior-dynamic-macros/dm_kconfig.h> /* DM_TYPING_ENABLED, MAX_SLOTS, slot_is_nvs */
#include <zmk-behavior-dynamic-macros/dm_machine.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>
/* full slot_store layout: struct dm_inst embeds it by value (caller-owned storage) */
#include <zmk-behavior-dynamic-macros/slot_store_priv.h>
#if DM_TYPING_ENABLED
#include <zmk-behavior-dynamic-macros/dm_feedback_pump.h>
#endif

/* Per-instance config (the settings key prefix). The definition guard keeps it
 * idempotent if a translation unit reaches it through more than one include. */
#ifndef DM_BEHAVIOR_CONFIG_DEFINED
#define DM_BEHAVIOR_CONFIG_DEFINED
struct behavior_dynamic_macro_config {
    const char *settings_name;
};
#endif

struct dm_inst {
    const struct device *dev;

    /* the composed modules (each owns its own data) */
    dm_machine  machine;
    slot_store  store;
#if DM_TYPING_ENABLED
    dm_feedback feedback;
#endif

    /* the machine's downward vtable (store_* + speak_* + notify), wired at init.
     * Held here so its lifetime matches the instance. */
    dm_machine_callbacks callbacks;

    /* shell-owned wiring the modules deliberately don't carry */
    bool suppress_recording;          /* listener gate while emitters type */
    struct k_work_delayable timeout_work; /* assign/move/delete/preview timeout */

    /* playback emitter (co-located primitive: replays a slot's dm_events) */
    int            playback_slot;     /* -1 = idle */
    uint32_t       playback_event;
    struct k_timer playback_timer;
    struct k_work  playback_work;
};

/* Single-instance resolution lives in exactly one place (the query shell),
 * anchored to BUILD_ASSERT(<=1). dm_devices[] follows ZMK convention. */
extern const struct device *dm_devices[];
extern const size_t dm_devices_len;

struct dm_inst *dm_shell_instance(void); /* dm_devices[0]->data, or NULL */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
/*
 * Raise zmk_dynamic_macro_state_changed for a machine notify event code
 * (DM_EVT_* in dm_machine.c). Maps the machine's code to the widget event enum
 * and derives the coarse RECORDING/PLAYING/IDLE state from the machine. The
 * machine calls this through the notify vtable slot BEFORE feedback speaks, so
 * notifications fire at every feedback level. PLAY_FINISHED is raised by the
 * playback emitter directly, not the machine.
 */
void dm_events_raise(struct dm_inst *inst, int machine_event, int slot);
#endif

#endif /* DM_SHELL_H */
