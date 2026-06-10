/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_notify — the internal notification code the machine produces and dm_events
 * consumes. Pure (no Zephyr): the Zephyr-free machine raises one of these via its
 * notify callback BEFORE feedback speaks, so notifications fire at every feedback
 * level; dm_events maps it to the public zmk_dynamic_macro_event_type and raises
 * the widget event.
 *
 * It lives in its own pure header so the one code list has a single home — the
 * machine, dm_events, and the behavior shell all include it instead of each
 * keeping a hand-synced copy (the shell's playback emitter raises PLAY_FINISHED
 * directly, the machine raises the rest).
 *
 * This is the INTERNAL ordering. The public widget enum
 * (zmk_dynamic_macro_event_type) is deliberately ordered differently, so
 * dm_events::map_event translates between the two — that map is real, not a
 * duplicate, and stays.
 */

#ifndef DM_NOTIFY_H
#define DM_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
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
    DM_EVT__COUNT, /* sentinel — keep last; lets map_event be exhaustively checked */
} dm_notify_code;

#ifdef __cplusplus
}
#endif

#endif /* DM_NOTIFY_H */
