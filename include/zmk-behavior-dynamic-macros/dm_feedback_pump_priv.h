/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Private layout of dm_feedback (the pump). Exposed only so the behavior shell
 * can embed it in dev->data without a heap allocation — the same caller-owned
 * pattern as dm_machine / slot_store. No module outside dm_feedback_pump.c reads
 * these fields.
 */

#ifndef DM_FEEDBACK_PUMP_PRIV_H
#define DM_FEEDBACK_PUMP_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <zmk-behavior-dynamic-macros/dm_feedback_build.h>
#include <zmk-behavior-dynamic-macros/dm_feedback_pump.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>

struct dm_fb_event {
    uint16_t keycode;
    uint8_t  mods;
};

struct dm_feedback {
    /* wiring (copied from dm_feedback_config at init) */
    dm_machine *machine;
    slot_store *store;
    dm_locale   locale;
    int         status_detail;
    int         nvs_slots;
    int         max_slots;
    void      (*raise_keycode)(void *ctx, uint16_t keycode, uint8_t mods, bool pressed);
    void      (*save_knobs)(void *ctx, uint8_t level, uint8_t style, bool erase);
    void      (*set_suppress)(void *ctx, bool suppress);
    void       *ctx;

    /* runtime knobs (read live on the emit path; never baked into a spec) */
    uint8_t level;
    uint8_t style;     /* DM_FB_STYLE_FULL / DM_FB_STYLE_ARROW */
    bool    erase_enabled;

    /* the keystroke ring (power-of-two, masked head/tail) */
    struct dm_fb_event ring[DM_FB_RING_SIZE];
    uint8_t ring_head;
    uint8_t ring_tail;
    bool    press_phase; /* true = next emit is the press half */

    /* timer/work driving one press/release per TAP_DELAY */
    struct k_timer emit_timer;
    struct k_work  emit_work;
    bool           emit_active; /* a message/erase is draining; false makes a stale
                                 * emit_iteration (a timer fire already in flight when
                                 * an erase was cancelled) inert instead of reporting a
                                 * phantom typing_finished. */

    /* the spec currently being typed + its preview-streaming continuation */
    dm_feedback_spec spec;
    bool             have_spec;
    bool             preview_pending;  /* a preview portion still to stream */
    bool             suffix_pending;   /* trailing preview parts still to emit */
    dm_render_cursor cursor;           /* resumable preview walk position */

    /* status continuation: walk all/used slots, one line each */
    bool status_mode;
    int  status_next_slot;   /* next slot to render after this line finishes */

    /* auto-erase scheduler */
    uint16_t erase_char_count;     /* non-RET feedback chars typed this round */
    bool     erase_pending;        /* a delayed erase is scheduled */
    bool     erase_in_progress;    /* currently typing backspaces */
    struct k_work_delayable erase_work;
};

#endif /* DM_FEEDBACK_PUMP_PRIV_H */
