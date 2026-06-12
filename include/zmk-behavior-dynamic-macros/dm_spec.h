/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_spec — the feedback message spec, the one collapsed description of "which
 * message to type". Pure (no Zephyr, no I/O): it is the value the machine builds
 * and hands across the speak seam, and the value dm_feedback_build consumes.
 *
 * It lives in its own header so the pure dm_machine can build a spec without
 * depending on dm_feedback's builder, and dm_feedback_build can consume one
 * without depending on the machine. Both include this; neither owns it.
 *
 * The machine emits ONE spec per transition rather than naming a dedicated
 * per-kind callback, so the message enum is carried across the seam exactly as it
 * is inside the builder.
 */

#ifndef DM_SPEC_H
#define DM_SPEC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* DM_SPEC_H */
