/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_feedback_pump — the Zephyr feedback emitter around the pure dm_feedback_build.
 *
 * Owns the per-instance typing machinery the pure core deliberately has no
 * business touching: the fb_event ring, the TAP_DELAY emit timer/work, the
 * press/release phasing, the resumable preview cursor, the runtime knobs
 * (level/style/auto-erase) and their persistence, and the auto-erase scheduler.
 * The message bytes themselves come from dm_feedback_build; this module is the
 * thing that turns those bytes into timed keystrokes and reports completion up
 * to dm_machine.
 *
 * speak(spec) is the single entry the machine's speak_* callbacks funnel into:
 * gate on the runtime level, reset the ring, build the message, start the timer.
 * When the level types nothing it calls dm_machine_typing_finished() synchronously
 * (the OFF-path rule) — so one rule covers OFF, below-level, and empty-message.
 *
 * Completion flows UP only: the emit loop calls dm_machine_typing_finished when
 * the ring drains; erase reports erase_due/erase_cancel. This module never writes
 * dm_state.
 */

#ifndef DM_FEEDBACK_PUMP_H
#define DM_FEEDBACK_PUMP_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* dm_internal.h FIRST: it supplies the Kconfig-derived slot sizing (MAX_EVENTS,
 * NVS_SLOTS, ...) so struct dm_slot has the SAME layout in this Zephyr TU as in
 * the shell — dm_config.h's host defaults must not win here. */
#include <zmk-behavior-dynamic-macros/dm_internal.h>
#include <zmk-behavior-dynamic-macros/dm_feedback_build.h>
#include <zmk-behavior-dynamic-macros/dm_machine.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feedback verbosity levels (mirror the Kconfig ladder; min_level gates speak). */
#define DM_FB_LEVEL_OFF     0
#define DM_FB_LEVEL_ERROR   1
#define DM_FB_LEVEL_COMMAND 2
#define DM_FB_LEVEL_BASIC   3
#define DM_FB_LEVEL_VERBOSE 4

/* Status-detail levels (compile-time DM_STATUS_DETAIL gate, independent of level). */
#define DM_FB_STATUS_OFF          0
#define DM_FB_STATUS_COUNT        1
#define DM_FB_STATUS_USED         2
#define DM_FB_STATUS_USED_PREVIEW 3
#define DM_FB_STATUS_FULL         4

#ifndef DM_FB_RING_SIZE
#define DM_FB_RING_SIZE 64
#endif

typedef struct dm_feedback dm_feedback;

/*
 * Everything the pump needs that lives outside it: the machine it reports up to,
 * the store it reads for previews/counts, and the compile-time facts (locale,
 * status detail, slot geometry) the firmware fixes at link time. Wired once at
 * init by the behavior shell.
 */
typedef struct {
    dm_machine *machine;
    slot_store *store;

    dm_locale locale;
    int       status_detail;   /* DM_STATUS_DETAIL — compile-time status gate */
    int       nvs_slots;       /* NVS_SLOTS */
    int       max_slots;       /* MAX_SLOTS */

    /* Initial / restored knob defaults (Kconfig DM_FEEDBACK_LEVEL etc.). */
    uint8_t default_level;
    uint8_t default_style;
    bool    default_erase;

    /*
     * Raise a keycode press/release. Indirected so the pump stays free of a
     * direct ZMK event-manager dependency in the host/parity build; the firmware
     * wires raise_zmk_keycode_state_changed, the parity harness wires a capture.
     */
    void (*raise_keycode)(void *ctx, uint16_t keycode, uint8_t mods, bool pressed);

    /* Persist the knobs (level/style/erase) — thin downward arrow to dm_nvs.
     * NULL in a RAM-only / no-persist build. */
    void (*save_knobs)(void *ctx, uint8_t level, uint8_t style, bool erase);

    /* Toggle the listener's recording suppression while the pump types, so the
     * emitted keystrokes are not themselves recorded. Owned by the shell. */
    void (*set_suppress)(void *ctx, bool suppress);

    void *ctx; /* passed to raise_keycode / save_knobs / set_suppress */
} dm_feedback_config;

/* ---- lifecycle ------------------------------------------------------------ */

void dm_feedback_pump_init(dm_feedback *f, const dm_feedback_config *cfg);

/* ---- the speak entry (the machine's speak_* callbacks funnel here) --------- */

/*
 * Gate → reset → build → start. If the runtime level types nothing for this
 * spec, calls dm_machine_typing_finished() synchronously and returns without
 * starting the timer. `return_state` is parked by the machine before it calls a
 * speak_, so the pump does not carry it.
 */
void dm_feedback_speak(dm_feedback *f, const dm_feedback_spec *spec);

/* ---- knob commands (effect + persist + confirmation speech) --------------- */

/*
 * The machine routes FEEDBACK_INC/DEC, STYLE_TOGGLE, ERASE_TOGGLE as IDLE-only
 * commands and parks return_state=IDLE; do_knob leaves the change to the shell's
 * feedback adapter. These perform the knob change, persist it, and speak the
 * confirmation (which ends by reporting typing_finished, like any speak).
 */
void dm_feedback_knob_level(dm_feedback *f, int direction); /* +1 / -1, clamped */
void dm_feedback_knob_style_toggle(dm_feedback *f);
void dm_feedback_knob_erase_toggle(dm_feedback *f);

/* ---- boot restore (dm_nvs delivers decoded knob values UP into the owner) -- */

/*
 * The one place the ARROW-requires-full-punctuation-locale rule lives: a
 * restored ARROW style on a plain locale is silently kept as FULL, by
 * construction. Each setter is a no-op if the value is out of range, matching
 * the old dm_settings_set validation.
 */
void dm_feedback_restore_level(dm_feedback *f, uint8_t level);
void dm_feedback_restore_style(dm_feedback *f, uint8_t style);
void dm_feedback_restore_erase(dm_feedback *f, bool erase);

/* Current knob values — for the export read-back (dm_nvs reads through this). */
uint8_t dm_feedback_level(const dm_feedback *f);
uint8_t dm_feedback_style(const dm_feedback *f);
bool    dm_feedback_erase(const dm_feedback *f);

/* ---- erase cancellation ---------------------------------------------------- */

/*
 * Cancel a scheduled auto-erase and abort one already mid-emission. The shell
 * calls this from any DM binding press and from any keycode through the listener
 * (the two cancel paths). On an in-progress erase it drains the ring, drops
 * suppression, and reports dm_machine_erase_cancel so the machine restores the
 * parked return-state. A no-op if no erase is pending or running.
 */
void dm_feedback_pump_cancel_erase(dm_feedback *f);

#ifdef __cplusplus
}
#endif

/* The private struct is exposed so the shell can embed it in dev->data without a
 * heap allocation, matching dm_machine / slot_store. Treat as opaque. */
#include <zmk-behavior-dynamic-macros/dm_feedback_pump_priv.h>

#endif /* DM_FEEDBACK_PUMP_H */
