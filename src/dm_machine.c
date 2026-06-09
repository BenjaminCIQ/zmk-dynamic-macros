/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_machine — the only writer of `state`.
 *
 * Two-phase dispatch: a static const legality matrix answers "does this command
 * do anything in this state?" (ALLOWED / IGNORED), then ALLOWED commands run a
 * data-dependent guard that asks slot_store (never reads slot bytes) and either
 * performs the effect or returns a rejection carrying a return-state. `state` is
 * written exactly once per transition, before any effect that can emit feedback.
 *
 * PURE: no Zephyr, no I/O. All effects and feedback go through the injected
 * callback vtable, so the orchestration is host-testable against fakes.
 */

#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_machine.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>

/* Notification event codes — mirror zmk_dynamic_macro_state_changed's enum so the
 * shell can forward them to dm_events without the machine pulling in Zephyr. The
 * machine raises them before calling speak, so they fire at every feedback level. */
#define DM_EVT_RECORDING_STARTED 0
#define DM_EVT_RECORDING_STOPPED 1
#define DM_EVT_SAVED             2
#define DM_EVT_DELETED           3
#define DM_EVT_MOVED             4
#define DM_EVT_PLAY_STARTED      5
#define DM_EVT_PLAY_FINISHED     6
#define DM_EVT_PREVIEW_READY     7
#define DM_EVT_ERROR_NO_RECORDING 8
#define DM_EVT_ERROR_SLOT_EMPTY   9
#define DM_EVT_ERROR_OVERFLOW     10
#define DM_EVT_ERROR_SAVE_FAILED  11
#define DM_EVT_ERROR_DELETE_FAILED 12
#define DM_EVT_ERROR_QUEUE_FULL   13

/* ---- legality matrix ------------------------------------------------------ */

typedef enum { IGNORED = 0, ALLOWED = 1 } verdict;

/*
 * legality[state][command]. Two-valued: ALLOWED means the command does
 * something in this state (a transition or a guard-checked effect); IGNORED
 * means it is silently dropped (DM_OK, no side effect). Feedback-bearing
 * rejections are NOT in this table — they are data-dependent guard outcomes.
 *
 * Knob commands (FEEDBACK_INC/DEC, STYLE_TOGGLE, ERASE_TOGGLE), STATE, MOV, DEL,
 * PREVIEW, and TEST_RELOAD are IDLE-only. SLOT is context-sensitive (play /
 * chain / assign / delete-target / move-target / preview-target) and so is
 * ALLOWED in every state that consumes a slot press. OVERFLOW is internal and
 * only meaningful while RECORDING.
 */
static const verdict legality[9][DM_CMD__COUNT] = {
    /* state                 REC  STP  DEL  MOV  SLOT STATE PRE  F+   F-   STY  ERS  RLD  OVF */
    [DM_STATE_IDLE]        = {  1,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   0 },
    [DM_STATE_RECORDING]   = {  1,   1,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   1 },
    [DM_STATE_PENDING_ASSIGN]={ 1,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0 },
    [DM_STATE_DELETE_PENDING]={ 0,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0 },
    [DM_STATE_MOVE_PENDING] = {  0,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0 },
    [DM_STATE_PREVIEW_PENDING]={0,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0 },
    [DM_STATE_PLAYING]     = {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    [DM_STATE_TYPING_FEEDBACK]={0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
    [DM_STATE_TYPING_ERASE] = {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },
};

/* ---- lifecycle ------------------------------------------------------------ */

void dm_machine_init(dm_machine *m, slot_store *s, const dm_machine_callbacks *cb) {
    memset(m, 0, sizeof(*m));
    m->state = DM_STATE_IDLE;
    m->return_state = DM_STATE_IDLE;
    m->move_source_slot = -1;
    m->post_save_slot = -1;
    m->store = s;
    m->cb = cb;
}

dm_state dm_machine_state(const dm_machine *m) {
    return m->state;
}

/* ---- small helpers -------------------------------------------------------- */

static void notify(dm_machine *m, int event, int slot) {
    if (m->cb->notify) {
        m->cb->notify(m->cb->ctx, event, slot);
    }
}

/* (Re)start / cancel the shell-owned assign/move/delete/preview timeout. The
 * machine drives these wherever it enters or resolves a *_PENDING state, so the
 * timer tracks the pending state regardless of which call path arrives. */
static void arm_timeout(dm_machine *m) {
    if (m->cb->arm_timeout) {
        m->cb->arm_timeout(m->cb->ctx);
    }
}

static void cancel_timeout(dm_machine *m) {
    if (m->cb->cancel_timeout) {
        m->cb->cancel_timeout(m->cb->ctx);
    }
}

static bool slot_empty(dm_machine *m, int idx) {
    return m->cb->store_is_empty(m->cb->ctx, idx);
}

/* Park the return-state the machine restores at typing_finished, and enter a
 * typing state so a subsequent command sees the machine as busy. */
static void enter_typing(dm_machine *m, dm_state return_to) {
    m->return_state = return_to;
    m->state = DM_STATE_TYPING_FEEDBACK;
}

/* ---- per-state transition logic ------------------------------------------- */

static dm_result do_rec(dm_machine *m) {
    /* REC from PENDING_ASSIGN with a non-empty draft discards the unassigned
     * take in favor of a fresh recording. */
    (void)m->cb->store_draft_count(m->cb->ctx); /* observe; discard is the reset below */
    m->cb->store_draft_reset(m->cb->ctx);
    m->state = DM_STATE_RECORDING;
    notify(m, DM_EVT_RECORDING_STARTED, -1);
    m->cb->speak_rec(m->cb->ctx);
    return DM_OK;
}

static dm_result do_stop(dm_machine *m) {
    if (m->cb->store_draft_count(m->cb->ctx) == 0) {
        m->state = DM_STATE_IDLE;
        notify(m, DM_EVT_ERROR_NO_RECORDING, -1);
        m->cb->speak_no_recording(m->cb->ctx);
        return DM_OK;
    }
    m->state = DM_STATE_PENDING_ASSIGN;
    notify(m, DM_EVT_RECORDING_STOPPED, -1);
    m->cb->speak_stop(m->cb->ctx);
    return DM_OK;
}

static dm_result do_overflow(dm_machine *m) {
    m->state = DM_STATE_PENDING_ASSIGN;
    notify(m, DM_EVT_ERROR_OVERFLOW, -1);
    m->cb->speak_overflow(m->cb->ctx);
    return DM_OK;
}

static dm_result do_delete_mode(dm_machine *m) {
    m->state = DM_STATE_DELETE_PENDING;
    arm_timeout(m);
    return DM_OK;
}

static dm_result do_move_mode(dm_machine *m) {
    m->move_source_slot = -1;
    m->state = DM_STATE_MOVE_PENDING;
    arm_timeout(m);
    m->cb->speak_move_prompt(m->cb->ctx);
    return DM_OK;
}

static dm_result do_status(dm_machine *m) {
    /* STATUS renders from IDLE; it returns to IDLE when typing finishes. */
    enter_typing(m, DM_STATE_IDLE);
    m->cb->speak_status(m->cb->ctx);
    return DM_OK;
}

static dm_result do_preview(dm_machine *m) {
    m->state = DM_STATE_PREVIEW_PENDING;
    arm_timeout(m);
    return DM_OK;
}

/* knob commands: adjust + persist + speak live in feedback; the machine just
 * routes them as IDLE-only commands and asks feedback to confirm. */
static dm_result do_knob(dm_machine *m, dm_command cmd) {
    (void)cmd;
    /* Knob effect + confirmation are feedback-internal; from the machine's view
     * this is a feedback emission that returns to IDLE. The shell's feedback
     * adapter performs the knob change before calling typing_finished. */
    enter_typing(m, DM_STATE_IDLE);
    /* Knob confirmation has no dedicated speak_ in the machine vtable — the
     * shell handles the level/style/erase change and its confirmation directly,
     * then reports typing_finished. The machine only owns the state parking. */
    return DM_OK;
}

/* SLOT in RECORDING = chain source into the draft. */
static dm_result slot_recording(dm_machine *m, int idx) {
    if (slot_empty(m, idx)) {
        notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
        m->cb->speak_chain_empty(m->cb->ctx, idx);
        return DM_REJECTED_EMPTY; /* stays RECORDING */
    }
    dm_result rc = m->cb->store_draft_chain(m->cb->ctx, idx);
    if (rc == DM_REJECTED_FULL) {
        m->cb->speak_chain_no_room(m->cb->ctx, idx);
        return DM_REJECTED_FULL; /* stays RECORDING */
    }
    m->cb->speak_chain_insert(m->cb->ctx, idx);
    return DM_OK; /* stays RECORDING */
}

/* SLOT in PENDING_ASSIGN = commit the draft into idx (RAM-only); persist is
 * deferred to typing_finished. */
static dm_result slot_assign(dm_machine *m, int idx) {
    cancel_timeout(m);
    if (!slot_empty(m, idx)) {
        /* preserved pending state: press another slot, or let the timeout fire */
        m->state = DM_STATE_PENDING_ASSIGN;
        arm_timeout(m);
        m->cb->speak_slot_full(m->cb->ctx, idx);
        return DM_REJECTED_OCCUPIED;
    }
    dm_result rc = m->cb->store_draft_commit(m->cb->ctx, idx);
    if (rc != DM_OK) {
        m->state = DM_STATE_PENDING_ASSIGN;
        arm_timeout(m);
        m->cb->speak_slot_full(m->cb->ctx, idx);
        return rc;
    }
    /* state settles before the SAVED feedback types; persist fires at finish. */
    m->post_save_slot = idx;
    enter_typing(m, DM_STATE_IDLE);
    notify(m, DM_EVT_SAVED, idx);
    m->cb->speak_saved(m->cb->ctx, idx);
    return DM_OK;
}

/* SLOT in DELETE_PENDING = delete idx (or reject empty, dropping to IDLE). */
static dm_result slot_delete(dm_machine *m, int idx) {
    cancel_timeout(m);
    if (slot_empty(m, idx)) {
        m->state = DM_STATE_IDLE;
        notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
        m->cb->speak_slot_empty(m->cb->ctx, idx);
        return DM_REJECTED_EMPTY;
    }
    /* settle to IDLE before the effect: a RAM delete speaks immediately, and a
     * queue-full failure path speaks from a settled state. */
    m->state = DM_STATE_IDLE;
    dm_result rc = m->cb->store_delete(m->cb->ctx, idx);
    if (rc == DM_DELETE_QUEUE_FULL) {
        notify(m, DM_EVT_ERROR_QUEUE_FULL, idx);
        m->cb->speak_delete_queue_full(m->cb->ctx, idx);
        return rc;
    }
    /* NVS delete: the DELETED notification + speech are deferred to
     * deliver_async on completion. A RAM delete completes synchronously, so
     * speak the confirmation now. The store distinguishes them; the machine
     * relies on store_delete returning DM_OK for both and the async path
     * (deliver_async) for the NVS DELETED. For RAM the shell's store adapter
     * has already zeroed the slot, so notify+speak here. */
    notify(m, DM_EVT_DELETED, idx);
    m->cb->speak_deleted(m->cb->ctx, idx);
    return DM_OK;
}

/* SLOT in MOVE_PENDING = select source, cancel (same slot), or perform move. */
static dm_result slot_move(dm_machine *m, int idx) {
    if (m->move_source_slot < 0) {
        if (slot_empty(m, idx)) {
            /* stays MOVE_PENDING — re-arm so the prompt keeps its timeout */
            arm_timeout(m);
            notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
            m->cb->speak_slot_empty(m->cb->ctx, idx);
            return DM_REJECTED_EMPTY;
        }
        m->move_source_slot = idx;
        /* source selected; the original MOVE prompt timeout keeps running (the
         * old path does not re-arm here). */
        m->cb->speak_move_source_selected(m->cb->ctx, idx);
        return DM_OK;
    }

    int src = m->move_source_slot;
    int dst = idx;

    if (src == dst) {
        /* same-slot move is a CANCEL, not a rejection — never reaches the store */
        m->move_source_slot = -1;
        cancel_timeout(m);
        m->state = DM_STATE_IDLE;
        m->cb->speak_move_cancelled(m->cb->ctx);
        return DM_OK;
    }

    if (!slot_empty(m, dst)) {
        arm_timeout(m); /* stays MOVE_PENDING */
        m->cb->speak_slot_full(m->cb->ctx, dst);
        return DM_REJECTED_OCCUPIED;
    }

    /* settle before the effect (transaction rule): the queue-full feedback paths
     * speak from IDLE and drive their own typing. */
    m->move_source_slot = -1;
    cancel_timeout(m);
    m->state = DM_STATE_IDLE;

    dm_result rc = m->cb->store_move(m->cb->ctx, src, dst);
    if (rc == DM_SAVE_QUEUE_FULL) {
        notify(m, DM_EVT_ERROR_QUEUE_FULL, dst);
        m->cb->speak_save_queue_full(m->cb->ctx, dst);
        return rc;
    }
    if (rc == DM_DELETE_QUEUE_FULL) {
        notify(m, DM_EVT_ERROR_QUEUE_FULL, src);
        m->cb->speak_delete_queue_full(m->cb->ctx, src);
        return rc;
    }
    notify(m, DM_EVT_MOVED, dst);
    m->cb->speak_moved(m->cb->ctx, src, dst);
    return DM_OK;
}

/* SLOT in PREVIEW_PENDING = request the preview for idx. */
static dm_result slot_preview(dm_machine *m, int idx) {
    cancel_timeout(m);
    m->state = DM_STATE_IDLE;
    notify(m, DM_EVT_PREVIEW_READY, idx);
    m->cb->speak_preview(m->cb->ctx, idx);
    return DM_OK;
}

/* SLOT in IDLE = play idx (or reject empty). */
static dm_result slot_play(dm_machine *m, int idx) {
    if (slot_empty(m, idx)) {
        notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
        m->cb->speak_slot_empty(m->cb->ctx, idx);
        return DM_REJECTED_EMPTY;
    }
    m->state = DM_STATE_PLAYING;
    m->cb->store_mark_playing(m->cb->ctx, idx);
    notify(m, DM_EVT_PLAY_STARTED, idx);
    return DM_OK;
}

static dm_result do_slot(dm_machine *m, int idx) {
    switch (m->state) {
    case DM_STATE_RECORDING:        return slot_recording(m, idx);
    case DM_STATE_PENDING_ASSIGN:   return slot_assign(m, idx);
    case DM_STATE_DELETE_PENDING:   return slot_delete(m, idx);
    case DM_STATE_MOVE_PENDING:     return slot_move(m, idx);
    case DM_STATE_PREVIEW_PENDING:  return slot_preview(m, idx);
    case DM_STATE_IDLE:             return slot_play(m, idx);
    default:                        return DM_OK; /* unreachable: IGNORED in matrix */
    }
}

/* ---- primary command interface -------------------------------------------- */

dm_result dm_machine_command(dm_machine *m, dm_command cmd, int param) {
    if (cmd >= DM_CMD__COUNT || m->state > DM_STATE_TYPING_ERASE) {
        return DM_OK;
    }
    if (legality[m->state][cmd] == IGNORED) {
        return DM_OK;
    }

    switch (cmd) {
    case DM_CMD_REC:      return do_rec(m);
    case DM_CMD_STP:      return do_stop(m);
    case DM_CMD_DEL:      return do_delete_mode(m);
    case DM_CMD_MOV:      return do_move_mode(m);
    case DM_CMD_SLOT:     return do_slot(m, param);
    case DM_CMD_STATE:    return do_status(m);
    case DM_CMD_PREVIEW:  return do_preview(m);
    case DM_CMD_FEEDBACK_INC:
    case DM_CMD_FEEDBACK_DEC:
    case DM_CMD_STYLE_TOGGLE:
    case DM_CMD_ERASE_TOGGLE: return do_knob(m, cmd);
    case DM_CMD_TEST_RELOAD:  return DM_OK; /* dispatched by dm_nvs, not a transition */
    case DM_CMD_OVERFLOW:     return do_overflow(m);
    default:                  return DM_OK;
    }
}

/* ---- up-calls ------------------------------------------------------------- */

void dm_machine_typing_finished(dm_machine *m) {
    /* fire the deferred post-save persist before settling */
    if (m->post_save_slot >= 0) {
        m->cb->store_persist(m->cb->ctx, m->post_save_slot);
        m->post_save_slot = -1;
    }
    m->state = m->return_state;
    /* a return into a *_PENDING state re-arms the assign/move timeout (the old
     * feedback_complete reschedules assign_timeout_work for exactly these). */
    if (m->state == DM_STATE_PENDING_ASSIGN || m->state == DM_STATE_DELETE_PENDING ||
        m->state == DM_STATE_MOVE_PENDING || m->state == DM_STATE_PREVIEW_PENDING) {
        arm_timeout(m);
    }
}

void dm_machine_deliver_async(dm_machine *m, dm_result outcome, int slot) {
    /* IDLE-suppression: a deferred completion only speaks when nothing else is
     * active; otherwise it is dropped rather than hijacking the live op. */
    if (m->state != DM_STATE_IDLE) {
        return;
    }
    switch (outcome) {
    case DM_OK:
        notify(m, DM_EVT_DELETED, slot);
        m->cb->speak_async_deleted(m->cb->ctx, slot);
        break;
    case DM_SAVE_FAILED:
        notify(m, DM_EVT_ERROR_SAVE_FAILED, slot);
        m->cb->speak_async_save_failed(m->cb->ctx, slot);
        break;
    case DM_DELETE_FAILED:
        notify(m, DM_EVT_ERROR_DELETE_FAILED, slot);
        m->cb->speak_async_delete_failed(m->cb->ctx, slot);
        break;
    default:
        break;
    }
}

void dm_machine_erase_due(dm_machine *m) {
    /* park ONCE: a continuation batch arriving while already in TYPING_ERASE
     * keeps the original parked state. */
    if (!m->erase_active) {
        m->erase_return_state = m->state;
        m->erase_active = true;
    }
    m->state = DM_STATE_TYPING_ERASE;
    m->cb->speak_erase(m->cb->ctx);
}

void dm_machine_erase_cancel(dm_machine *m) {
    if (!m->erase_active) {
        return;
    }
    m->state = m->erase_return_state;
    m->erase_active = false;
}

void dm_machine_timeout(dm_machine *m) {
    /* Only a live pending state times out; a late timer after the state already
     * resolved (slot pressed, cancelled) is a no-op. */
    if (m->state != DM_STATE_PENDING_ASSIGN && m->state != DM_STATE_DELETE_PENDING &&
        m->state != DM_STATE_MOVE_PENDING && m->state != DM_STATE_PREVIEW_PENDING) {
        return;
    }
    m->move_source_slot = -1;
    m->state = DM_STATE_IDLE;
}

void dm_machine_play_finished(dm_machine *m) {
    if (m->state != DM_STATE_PLAYING) {
        return; /* a stray report after a cancel — no-op */
    }
    m->state = DM_STATE_IDLE;
}

