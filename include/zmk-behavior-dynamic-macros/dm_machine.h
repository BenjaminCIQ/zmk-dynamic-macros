/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_machine — the only writer of `state`.
 *
 * Owns state, parked return-states, and the assign/move-source timeout pending
 * flag. Every state transition goes through dm_machine_command(); the legality
 * matrix answers "does this command do anything in this state?" (ALLOWED /
 * IGNORED) and is exhaustively testable: 9 states × 14 commands. Guards run
 * only on ALLOWED and are data-dependent (ask slot_store, never peek at bytes).
 *
 * PURE: no Zephyr, no I/O. The callback vtable (dm_machine_callbacks) is the
 * only downward dependency — injected at init so the full orchestration flow is
 * host-testable against fakes.
 */

#ifndef DM_MACHINE_H
#define DM_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_result.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- State ---------------------------------------------------------------- */

typedef enum {
    DM_STATE_IDLE = 0,
    DM_STATE_RECORDING,
    DM_STATE_PENDING_ASSIGN,
    DM_STATE_DELETE_PENDING,
    DM_STATE_MOVE_PENDING,
    DM_STATE_PREVIEW_PENDING,
    DM_STATE_PLAYING,
    DM_STATE_TYPING_FEEDBACK,
    DM_STATE_TYPING_ERASE,
} dm_state;

/* ---- Commands (the public vocabulary; DM_CMD_OVERFLOW is internal) -------- */

typedef enum {
    DM_CMD_REC = 0,
    DM_CMD_STP,
    DM_CMD_DEL,
    DM_CMD_MOV,
    DM_CMD_SLOT,
    DM_CMD_STATE,
    DM_CMD_PREVIEW,
    DM_CMD_FEEDBACK_INC,
    DM_CMD_FEEDBACK_DEC,
    DM_CMD_STYLE_TOGGLE,
    DM_CMD_ERASE_TOGGLE,
    DM_CMD_TEST_RELOAD,
    DM_CMD_OVERFLOW, /* internal: draft_append returned false; shell issues this */
    DM_CMD__COUNT,   /* sentinel — keep last */
} dm_command;

/* ---- Callbacks — the machine's only downward dependencies ---------------- */

/*
 * Every effect the machine triggers goes through this vtable. Injected at
 * init; the firmware wires real slot_store + feedback + events. The host
 * tests wire fakes that record calls and return controlled results.
 *
 * Return values:
 *   - slot_* : dm_result (DM_OK or a rejection / queue-full code)
 *   - speak_* : void (fire-and-forget; the machine has already written state)
 *   - notify  : void
 */
typedef struct {
    /* slot_store operations — the machine asks these but never reads bytes */
    dm_result (*store_move)(void *ctx, int src, int dst);
    dm_result (*store_delete)(void *ctx, int idx);
    dm_result (*store_persist)(void *ctx, int idx);
    dm_result (*store_draft_commit)(void *ctx, int dst);
    dm_result (*store_draft_chain)(void *ctx, int src);
    int       (*store_draft_count)(void *ctx);
    bool      (*store_is_empty)(void *ctx, int idx);
    void      (*store_draft_reset)(void *ctx);
    void      (*store_mark_playing)(void *ctx, int idx);
    void      (*store_clear_playing)(void *ctx);

    /* feedback — called after state is written */
    void (*speak_rec)(void *ctx);
    void (*speak_stop)(void *ctx);
    void (*speak_no_recording)(void *ctx);
    void (*speak_saved)(void *ctx, int slot);
    void (*speak_deleted)(void *ctx, int slot);
    void (*speak_slot_empty)(void *ctx, int slot);
    void (*speak_slot_full)(void *ctx, int slot);
    void (*speak_chain_insert)(void *ctx, int slot);
    void (*speak_chain_empty)(void *ctx, int slot);
    void (*speak_chain_no_room)(void *ctx, int slot);
    void (*speak_overflow)(void *ctx);
    void (*speak_move_prompt)(void *ctx);
    void (*speak_move_source_selected)(void *ctx, int slot);
    void (*speak_move_cancelled)(void *ctx);
    void (*speak_moved)(void *ctx, int src, int dst);
    void (*speak_save_queue_full)(void *ctx, int slot);
    void (*speak_delete_queue_full)(void *ctx, int slot);
    void (*speak_status)(void *ctx);
    void (*speak_preview)(void *ctx, int slot);
    void (*speak_async_deleted)(void *ctx, int slot);
    void (*speak_async_save_failed)(void *ctx, int slot);
    void (*speak_async_delete_failed)(void *ctx, int slot);
    void (*speak_erase)(void *ctx);

    /* notifications (raised before speak — fire at every feedback level) */
    void (*notify)(void *ctx, int event, int slot);

    void *ctx;
} dm_machine_callbacks;

/* ---- Handle -------------------------------------------------------------- */

typedef struct dm_machine dm_machine;

/* ---- Lifecycle ------------------------------------------------------------ */

void dm_machine_init(dm_machine *m, slot_store *s, const dm_machine_callbacks *cb);

/* ---- Primary command interface -------------------------------------------- */

/*
 * Two-phase dispatch: legality gate (matrix) → guard (data-dependent) →
 * single state= write → effect (via callbacks). Returns DM_OK on IGNORED or
 * on a successful transition; returns a dm_result rejection code if a guard
 * fails. The caller (behavior shell) does not need to check return for
 * routing — feedback is driven through callbacks.
 */
dm_result dm_machine_command(dm_machine *m, dm_command cmd, int param);

dm_state dm_machine_state(const dm_machine *m);

/* ---- Up-calls — completion reports from feedback and storage ------------- */

/*
 * Called by the feedback emitter when the ring drains (typing complete), OR
 * called synchronously by speak when the level gate types nothing (the OFF-path
 * rule). Applies the parked return-state, reschedules any pending
 * timeout, fires slot_store_persist for the post-save slot, kicks auto-erase.
 */
void dm_machine_typing_finished(dm_machine *m);

/*
 * Called by dm_nvs on the system queue after a deferred NVS op completes.
 * IDLE-suppression rule lives here: drop the outcome unless IDLE, else drive
 * the matching speak_ callback.
 */
void dm_machine_deliver_async(dm_machine *m, dm_result outcome, int slot);

/*
 * Auto-erase up-calls. erase_due is called by the erase scheduler's delayable
 * work. erase_cancel is called by any binding press or keycode through the
 * listener. The erase state lifecycle (parked state, batch continuation,
 * status exclusion) stays feedback-internal.
 */
void dm_machine_erase_due(dm_machine *m);
void dm_machine_erase_cancel(dm_machine *m);

/* ---- Private struct (defined in dm_machine.c; exposed for caller-owned
 *      storage so no heap allocation is needed) ---------------------------- */

struct dm_machine {
    dm_state          state;
    dm_state          return_state;        /* parked by speak; applied by typing_finished */
    dm_state          erase_return_state;  /* parked on erase_due */
    int               move_source_slot;    /* -1 = no source selected yet */
    int               post_save_slot;      /* slot to persist at typing_finished; -1 = none */
    bool              timeout_pending;     /* reschedule flag for typing_finished */
    bool              erase_active;        /* true while in TYPING_ERASE */
    slot_store       *store;
    const dm_machine_callbacks *cb;
};

#ifdef __cplusplus
}
#endif

#endif /* DM_MACHINE_H */
