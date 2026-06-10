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
#include <zmk-behavior-dynamic-macros/dm_notify.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>

/* Notification codes (dm_notify_code) come from dm_notify.h, the one owner. The
 * machine raises them via the notify callback before calling speak, so they fire
 * at every feedback level; dm_events maps them to the public widget enum. */

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
    [DM_STATE_PREVIEW_PENDING]={1,   0,   0,   0,   1,   0,   0,   0,   0,   0,   0,   0,   0 },
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

/* Build a spec from the parts the transition already holds and type it. One
 * call per transition replaces the old per-message speak_X pointer. */
static void speak(dm_machine *m, dm_fb_kind kind, int slot, int slot2, bool show_preview) {
    dm_feedback_spec spec = {
        .kind = kind, .slot = slot, .slot2 = slot2, .show_preview = show_preview, .knob_text = 0,
    };
    m->cb->speak(m->cb->ctx, &spec);
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
    /* REC restarts recording from IDLE, RECORDING (re-record), PENDING_ASSIGN
     * (a finished-but-unassigned take is discarded), and PREVIEW_PENDING. Cancel
     * any pending-state timeout so a stale timer cannot fire mid-recording — the
     * old cmd_record cancels assign_timeout_work unconditionally. */
    cancel_timeout(m);
    /* REC from PENDING_ASSIGN with a non-empty draft discards the unassigned
     * take in favor of a fresh recording. */
    (void)m->cb->store_draft_count(m->cb->ctx); /* observe; discard is the reset below */
    m->cb->store_draft_reset(m->cb->ctx);
    /* notify BEFORE the state write so the event's coarse state reflects the
     * pre-transition state. The cue then types while suppressed and returns to
     * RECORDING; parking the destination as the return-state (not writing it
     * directly) is what lets the OFF-path typing_finished restore RECORDING
     * instead of clobbering to IDLE. */
    notify(m, DM_EVT_RECORDING_STARTED, -1);
    enter_typing(m, DM_STATE_RECORDING);
    speak(m, DM_FB_REC, -1, -1, false);
    return DM_OK;
}

static dm_result do_stop(dm_machine *m) {
    if (m->cb->store_draft_count(m->cb->ctx) == 0) {
        notify(m, DM_EVT_ERROR_NO_RECORDING, -1);
        enter_typing(m, DM_STATE_IDLE);
        speak(m, DM_FB_NO_REC, -1, -1, false);
        return DM_OK;
    }
    /* raise STOPPED while still RECORDING (coarse state = RECORDING), as the old
     * feedback_stop does, before parking PENDING_ASSIGN. */
    notify(m, DM_EVT_RECORDING_STOPPED, -1);
    enter_typing(m, DM_STATE_PENDING_ASSIGN);
    speak(m, DM_FB_STOP, -1, -1, false);
    return DM_OK;
}

static dm_result do_overflow(dm_machine *m) {
    notify(m, DM_EVT_ERROR_OVERFLOW, -1);
    enter_typing(m, DM_STATE_PENDING_ASSIGN);
    speak(m, DM_FB_OVERFLOW, -1, -1, false);
    return DM_OK;
}

static dm_result do_delete_mode(dm_machine *m) {
    /* no cue — enters the pending state directly and arms its own timeout. */
    m->state = DM_STATE_DELETE_PENDING;
    arm_timeout(m);
    return DM_OK;
}

static dm_result do_move_mode(dm_machine *m) {
    m->move_source_slot = -1;
    /* the prompt cue returns to MOVE_PENDING; typing_finished re-arms the timeout
     * for the returned pending state, so no explicit arm here. */
    enter_typing(m, DM_STATE_MOVE_PENDING);
    speak(m, DM_FB_MOVE_PROMPT, -1, -1, false);
    return DM_OK;
}

static dm_result do_status(dm_machine *m) {
    /* STATUS renders from IDLE; it returns to IDLE when typing finishes. The
     * status header is the entry message; the pump streams the slot lines. */
    enter_typing(m, DM_STATE_IDLE);
    speak(m, DM_FB_STATUS_HEADER, -1, -1, false);
    return DM_OK;
}

static dm_result do_preview(dm_machine *m) {
    m->state = DM_STATE_PREVIEW_PENDING;
    arm_timeout(m);
    return DM_OK;
}

/* knob commands: adjust + persist + speak live in feedback, but the machine
 * routes them as IDLE-only commands and drives the effect through apply_knob
 * INSIDE the transition — so the ALLOWED/IGNORED verdict never leaks to the
 * shell to reconstruct from state. apply_knob's confirmation reports
 * typing_finished like any speak. */
static dm_result do_knob(dm_machine *m, dm_command cmd) {
    enter_typing(m, DM_STATE_IDLE);
    m->cb->apply_knob(m->cb->ctx, cmd);
    return DM_OK;
}

/* SLOT in RECORDING = chain source into the draft. */
static dm_result slot_recording(dm_machine *m, int idx) {
    if (slot_empty(m, idx)) {
        /* raise while still RECORDING (coarse=RECORDING), as old feedback_chain_empty */
        notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
        enter_typing(m, DM_STATE_RECORDING);
        speak(m, DM_FB_CHAIN_EMPTY, idx, -1, false);
        return DM_REJECTED_EMPTY; /* returns to RECORDING */
    }
    dm_result rc = m->cb->store_draft_chain(m->cb->ctx, idx);
    if (rc == DM_REJECTED_FULL) {
        enter_typing(m, DM_STATE_RECORDING);
        speak(m, DM_FB_CHAIN_NO_ROOM, idx, -1, false);
        return DM_REJECTED_FULL; /* returns to RECORDING */
    }
    enter_typing(m, DM_STATE_RECORDING);
    speak(m, DM_FB_CHAIN_INSERT, idx, -1, true);
    return DM_OK; /* returns to RECORDING */
}

/* SLOT in PENDING_ASSIGN = commit the draft into idx (RAM-only); persist is
 * deferred to typing_finished. */
static dm_result slot_assign(dm_machine *m, int idx) {
    cancel_timeout(m);
    if (!slot_empty(m, idx)) {
        /* preserved pending state: press another slot, or let the timeout fire.
         * typing_finished restores PENDING_ASSIGN and re-arms its timeout. */
        enter_typing(m, DM_STATE_PENDING_ASSIGN);
        speak(m, DM_FB_SLOT_FULL, idx, -1, false);
        return DM_REJECTED_OCCUPIED;
    }
    dm_result rc = m->cb->store_draft_commit(m->cb->ctx, idx);
    if (rc != DM_OK) {
        enter_typing(m, DM_STATE_PENDING_ASSIGN);
        speak(m, DM_FB_SLOT_FULL, idx, -1, false);
        return rc;
    }
    /* state settles before the SAVED feedback types; persist fires at finish. */
    m->post_save_slot = idx;
    enter_typing(m, DM_STATE_IDLE);
    notify(m, DM_EVT_SAVED, idx);
    speak(m, DM_FB_SAVED, idx, -1, true);
    return DM_OK;
}

/* SLOT in DELETE_PENDING = delete idx (or reject empty, dropping to IDLE). */
static dm_result slot_delete(dm_machine *m, int idx) {
    cancel_timeout(m);
    if (slot_empty(m, idx)) {
        enter_typing(m, DM_STATE_IDLE);
        notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
        speak(m, DM_FB_SLOT_EMPTY, idx, -1, false);
        return DM_REJECTED_EMPTY;
    }
    /* park the IDLE return-state before the effect (transaction rule): a RAM
     * delete speaks immediately and a queue-full failure both type from a settled
     * return-state. The NVS-delete success defers its DELETED speech to
     * deliver_async, which runs after this typing finishes. */
    enter_typing(m, DM_STATE_IDLE);
    dm_result rc = m->cb->store_delete(m->cb->ctx, idx);
    if (rc == DM_DELETE_QUEUE_FULL) {
        notify(m, DM_EVT_ERROR_QUEUE_FULL, idx);
        speak(m, DM_FB_DELETE_QFULL, idx, -1, false);
        return rc;
    }
    if (rc == DM_DELETE_DEFERRED) {
        /* NVS delete enqueued: the DELETED notification + speech wait for
         * deliver_async on completion (which runs after this typing finishes and
         * the machine is IDLE). The transition still typed nothing, so
         * typing_finished must run to settle the parked IDLE return-state; speak
         * does that on the OFF-path, but there is no speak here, so report it
         * directly — the machine, not a phantom speak, owns the no-type case. */
        dm_machine_typing_finished(m);
        return rc;
    }
    /* RAM delete: completed synchronously (the store zeroed the slot), so speak
     * the confirmation now. */
    notify(m, DM_EVT_DELETED, idx);
    speak(m, DM_FB_DELETED, idx, -1, false);
    return DM_OK;
}

/* SLOT in MOVE_PENDING = select source, cancel (same slot), or perform move. */
static dm_result slot_move(dm_machine *m, int idx) {
    if (m->move_source_slot < 0) {
        if (slot_empty(m, idx)) {
            /* returns to MOVE_PENDING; typing_finished re-arms the prompt timeout */
            enter_typing(m, DM_STATE_MOVE_PENDING);
            notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
            speak(m, DM_FB_SLOT_EMPTY, idx, -1, false);
            return DM_REJECTED_EMPTY;
        }
        m->move_source_slot = idx;
        /* source selected; the cue returns to MOVE_PENDING. (The MOVE prompt
         * timeout keeps running — typing_finished re-arms it on return, leaving
         * the existing timeout in place.) */
        enter_typing(m, DM_STATE_MOVE_PENDING);
        speak(m, DM_FB_MOVE_SRC, idx, -1, false);
        return DM_OK;
    }

    int src = m->move_source_slot;
    int dst = idx;

    if (src == dst) {
        /* same-slot move is a CANCEL, not a rejection — never reaches the store */
        m->move_source_slot = -1;
        cancel_timeout(m);
        enter_typing(m, DM_STATE_IDLE);
        speak(m, DM_FB_MOVE_CANCEL, -1, -1, false);
        return DM_OK;
    }

    if (!slot_empty(m, dst)) {
        enter_typing(m, DM_STATE_MOVE_PENDING); /* returns to MOVE_PENDING */
        speak(m, DM_FB_SLOT_FULL, dst, -1, false);
        return DM_REJECTED_OCCUPIED;
    }

    /* park the IDLE return-state before the effect (transaction rule): the
     * queue-full feedback paths type from a settled return-state and the MOVED
     * cue returns to IDLE. */
    m->move_source_slot = -1;
    cancel_timeout(m);
    enter_typing(m, DM_STATE_IDLE);

    dm_result rc = m->cb->store_move(m->cb->ctx, src, dst);
    if (rc == DM_SAVE_QUEUE_FULL) {
        notify(m, DM_EVT_ERROR_QUEUE_FULL, dst);
        speak(m, DM_FB_SAVE_QFULL, dst, -1, false);
        return rc;
    }
    if (rc == DM_DELETE_QUEUE_FULL) {
        notify(m, DM_EVT_ERROR_QUEUE_FULL, src);
        speak(m, DM_FB_DELETE_QFULL, src, -1, false);
        return rc;
    }
    notify(m, DM_EVT_MOVED, dst);
    speak(m, DM_FB_MOVED, src, dst, false);
    return DM_OK;
}

/* SLOT in PREVIEW_PENDING = request the preview for idx. */
static dm_result slot_preview(dm_machine *m, int idx) {
    cancel_timeout(m);
    /* preview types nothing (the cue is a query-API readout). Park the IDLE
     * return-state, raise the widget notification, and report typing_finished
     * inline — the machine owns the no-type case directly, with no phantom speak
     * that does not speak. */
    enter_typing(m, DM_STATE_IDLE);
    notify(m, DM_EVT_PREVIEW_READY, idx);
    dm_machine_typing_finished(m);
    return DM_OK;
}

/* SLOT in IDLE = play idx (or reject empty). */
static dm_result slot_play(dm_machine *m, int idx) {
    if (slot_empty(m, idx)) {
        notify(m, DM_EVT_ERROR_SLOT_EMPTY, idx);
        speak(m, DM_FB_SLOT_EMPTY, idx, -1, false);
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
    /* fire the deferred post-save persist, then settle (matching the old
     * feedback_complete order: state restored, timeout rescheduled, dm_save_slot
     * last). Capture the slot before clearing so a queue-full outcome can name it
     * AFTER the state has settled. */
    int persist_slot = m->post_save_slot;
    m->post_save_slot = -1;

    m->state = m->return_state;
    /* a return into a *_PENDING state re-arms the assign/move timeout (the old
     * feedback_complete reschedules assign_timeout_work for exactly these). */
    if (m->state == DM_STATE_PENDING_ASSIGN || m->state == DM_STATE_DELETE_PENDING ||
        m->state == DM_STATE_MOVE_PENDING || m->state == DM_STATE_PREVIEW_PENDING) {
        arm_timeout(m);
    }

    if (persist_slot >= 0) {
        dm_result rc = m->cb->store_persist(m->cb->ctx, persist_slot);
        if (rc == DM_SAVE_QUEUE_FULL) {
            /* the deferred assign-persist enqueue was refused: the macro will not
             * survive a reboot. Speak it from the settled state (a fresh feedback
             * typing, exactly as the old dm_feedback_save_queue_full did inside
             * feedback_complete), parking the current state as the return-state. */
            notify(m, DM_EVT_ERROR_QUEUE_FULL, persist_slot);
            enter_typing(m, m->state);
            speak(m, DM_FB_SAVE_QFULL, persist_slot, -1, false);
        }
    }
}

void dm_machine_deliver_async(dm_machine *m, dm_result outcome, int slot) {
    /* IDLE-suppression: a deferred completion only speaks when nothing else is
     * active; otherwise it is dropped rather than hijacking the live op. A
     * DM_DELETE_STALE completion (the op was superseded) is dropped here too. */
    if (m->state != DM_STATE_IDLE) {
        return;
    }
    int notify_evt;
    dm_fb_kind kind;
    switch (outcome) {
    case DM_OK:            notify_evt = DM_EVT_DELETED;            kind = DM_FB_DELETED;       break;
    case DM_SAVE_FAILED:   notify_evt = DM_EVT_ERROR_SAVE_FAILED;  kind = DM_FB_SAVE_FAILED;   break;
    case DM_DELETE_FAILED: notify_evt = DM_EVT_ERROR_DELETE_FAILED; kind = DM_FB_DELETE_FAILED; break;
    default:               return; /* DM_DELETE_STALE and anything else: drop */
    }
    /* Park the IDLE return-state and enter TYPING_FEEDBACK before speaking, so the
     * machine reads as busy while the deferred message types and settles back to
     * IDLE on typing_finished — exactly as a synchronous transition does (the old
     * deferred dm_feedback_deleted typed through TYPING_FEEDBACK the same way). */
    notify(m, notify_evt, slot);
    enter_typing(m, DM_STATE_IDLE);
    speak(m, kind, slot, -1, false);
}

void dm_machine_erase_due(dm_machine *m) {
    /* park ONCE: a continuation batch arriving while already in TYPING_ERASE
     * keeps the original parked state. */
    if (!m->erase_active) {
        m->erase_return_state = m->state;
        m->erase_active = true;
    }
    m->state = DM_STATE_TYPING_ERASE;
    /* The erase emission is armed by the erase scheduler itself (the pump pushes
     * backspaces right after this returns); the machine only parks the state. The
     * old speak_erase slot was a no-op in the typing build and dead in the
     * no-typing build (no scheduler), so it is gone. */
}

/* Both the clean-drain (erase_finished) and the abort (erase_cancel) paths
 * restore the state parked at erase_due and clear erase_active. Without this on
 * the clean-drain path, erase_active would stay set and a later keycode would
 * fire a phantom erase_cancel that resurrects the long-settled parked state. */
static void erase_restore(dm_machine *m) {
    if (!m->erase_active) {
        return;
    }
    m->state = m->erase_return_state;
    m->erase_active = false;
}

void dm_machine_erase_finished(dm_machine *m) {
    erase_restore(m);
}

void dm_machine_erase_cancel(dm_machine *m) {
    erase_restore(m);
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

