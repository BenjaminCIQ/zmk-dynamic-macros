/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_feedback_build — the pure message-builder core of dm_feedback.
 *
 * Turns a feedback spec (which message + slot + whether to show a preview) into
 * the exact sequence of HID keystrokes the feedback pump types, emitting each as
 * an fb_event {keycode, mods} to an abstract sink. This is the part of feedback
 * that has no business touching Zephyr: message-table selection, ASCII -> HID
 * mapping, slot-label/count formatting, and the preview walk (delegated to
 * dm_render) all live here, so the live-typing output and any host golden assert
 * the SAME bytes.
 *
 * PURE: no Zephyr, no I/O, no global state. The pump (k_timer/k_work, keycode
 * raising, the machine up-calls) is the thin Zephyr shell that drives this.
 *
 * Style (FULL/ARROW) and level are runtime-mutable and passed in live; locale is
 * link-time-fixed and selects the dm_render table. The builder must never bake
 * style or level into a cached structure.
 */

#ifndef DM_FEEDBACK_BUILD_H
#define DM_FEEDBACK_BUILD_H

#include <stdbool.h>
#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_event.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- runtime knobs (passed live; never baked in) -------------------------- */

typedef enum { DM_FB_STYLE_FULL = 0, DM_FB_STYLE_ARROW = 1 } dm_fb_style;

/* ---- the fb_event sink — one HID press/release pair per emitted keystroke -- */

/*
 * Abstract keystroke sink. The pump's adapter pushes into the live ring; the
 * host tests push into a flat array and assert the sequence. space_for(n) is the
 * ring-backpressure point (same contract as dm_render's char sink, but counted
 * in fb_events). emit appends one keystroke.
 */
typedef struct {
    void (*emit)(void *ctx, uint16_t keycode, uint8_t mods);
    bool (*space_for)(void *ctx, uint8_t n);
    void *ctx;
} dm_fb_sink;

/* ---- the message spec — replaces the 23 near-clone feedback_* functions ---- */

/*
 * Which message to build. The builder maps this + the live style/locale to the
 * concrete string parts and slot formatting. `slot` is the slot index a message
 * names (or -1); `slot2` is the move destination (or -1). `show_preview` streams
 * the slot's contents through dm_render after the scaffolding.
 */
typedef enum {
    DM_FB_REC = 0,
    DM_FB_STOP,
    DM_FB_NO_REC,
    DM_FB_SAVED,         /* slot; show_preview at VERBOSE */
    DM_FB_SLOT_FULL,     /* slot */
    DM_FB_SLOT_EMPTY,    /* slot */
    DM_FB_OVERFLOW,
    DM_FB_MOVE_PROMPT,
    DM_FB_MOVE_SRC,      /* slot */
    DM_FB_MOVED,         /* slot (src), slot2 (dst) */
    DM_FB_MOVE_CANCEL,
    DM_FB_CHAIN_INSERT,  /* slot; preview only */
    DM_FB_CHAIN_EMPTY,   /* slot */
    DM_FB_CHAIN_NO_ROOM, /* slot */
    DM_FB_DELETED,       /* slot */
    DM_FB_DELETE_FAILED, /* slot */
    DM_FB_SAVE_FAILED,   /* slot */
    DM_FB_SAVE_QFULL,    /* slot */
    DM_FB_DELETE_QFULL,  /* slot */
    DM_FB_KNOB,          /* knob_text */
    DM_FB_STATUS_HEADER, /* status header line */
    DM_FB_STATUS_SLOT,   /* slot; one status slot line, optional preview */
} dm_fb_kind;

typedef struct {
    dm_fb_kind  kind;
    int         slot;
    int         slot2;
    bool        show_preview;
    const char *knob_text; /* for DM_FB_KNOB: "VERBOSE", "ARROW", "ERASE ON", ... */
} dm_feedback_spec;

/* ---- slot facts the builder needs (passed in; the builder reads no slots) -- */

/*
 * The builder formats counts and slot ranges but never reads slot bytes. The
 * caller supplies the counts; the preview view (events+count) is handed to
 * dm_render. is_nvs decides the 'N'/'R' storage prefix.
 */
typedef struct {
    int  filled_count;          /* filled slots across the whole store (status header) */
    int  nvs_slots;             /* NVS_SLOTS — for the slot-range line */
    int  max_slots;             /* MAX_SLOTS */
    int  preview_event_count;   /* slot's event count, for the count suffix */
    bool slot_is_empty;         /* status slot: render "-" instead of a preview */
} dm_fb_facts;

/* ---- the build entry points ----------------------------------------------- */

/*
 * Build the scaffolding for `spec` (everything up to but not including the
 * preview). Emits the message-table parts + slot label/number to `sink`. The
 * preview, if any, is streamed separately via dm_feedback_build_preview so the
 * pump can pause/resume it under ring backpressure. Returns true if a preview
 * follows (the caller should then drive build_preview), false if the message is
 * complete.
 */
bool dm_feedback_build(const dm_feedback_spec *spec, dm_fb_style style, dm_locale locale,
                       const dm_fb_facts *facts, dm_fb_sink *sink);

/*
 * Stream the preview portion: walk `view` through dm_render, mapping each
 * rendered char back to an fb_event via the locale's ASCII->HID. Resumable under
 * backpressure with the caller-owned dm_render_cursor (NULL = one-shot). Returns
 * true when the preview is complete, false when paused (re-enter after drain).
 */
bool dm_feedback_build_preview(const dm_render_slot_view *view, dm_locale locale, dm_fb_sink *sink,
                               dm_render_cursor *cursor);

/*
 * Emit the preview's trailing parts (closing quote + count suffix), after the
 * preview walk completes. Split out because it runs once the cursor is done.
 */
void dm_feedback_build_preview_suffix(const dm_feedback_spec *spec, dm_fb_style style,
                                      dm_locale locale, const dm_fb_facts *facts, dm_fb_sink *sink);

/* ---- ASCII -> HID, exposed for the pump's direct char emits and tests ------ */

void dm_feedback_emit_ascii(dm_locale locale, dm_fb_sink *sink, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* DM_FEEDBACK_BUILD_H */
