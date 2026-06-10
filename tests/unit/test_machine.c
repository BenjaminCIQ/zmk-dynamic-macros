/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_machine unit tests. Test-first: these specify the legality matrix, the
 * data-dependent guards, and the completion up-calls before the module exists.
 *
 * The machine reaches slot_store and feedback only through a callback vtable;
 * the tests wire a fake that records every call and lets a test inject a
 * controlled dm_result, so the full orchestration flow is host-verifiable.
 */

#include "ztest_shim.h"

#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_machine.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>
#include <zmk-behavior-dynamic-macros/slot_store_priv.h>

/* ---- fake callbacks ------------------------------------------------------- */

#define LOG_CAP 32

struct fake {
    slot_store store;
    dm_machine m;

    /* call log — one short tag per speak/notify so a test can assert sequence */
    char log[LOG_CAP][32];
    int  log_n;

    /* controllable store results */
    dm_result move_rc, delete_rc, persist_rc, commit_rc, chain_rc;
    int       draft_count;

    /* recorded args */
    int last_persist_slot;
    int last_saved_slot;
    int last_moved_src, last_moved_dst;
    int last_notify_event, last_notify_slot;

    /* recorded knob commands (apply_knob) */
    dm_command last_knob_cmd;
    int        knob_calls;

    /* when true, speak does NOT auto-drive typing_finished — simulates the
     * live-typing case where the drain is a separate later event. Default false
     * (the OFF/instant path: speak finishes synchronously). */
    bool suppress_auto_finish;
};

static struct fake *g; /* current fake for the callback thunks */

static void log_tag(const char *tag) {
    if (g->log_n < LOG_CAP) {
        size_t n = strlen(tag);
        if (n > 31) {
            n = 31;
        }
        memcpy(g->log[g->log_n], tag, n);
        g->log[g->log_n][n] = '\0';
        g->log_n++;
    }
}

static bool log_has(const char *tag) {
    for (int i = 0; i < g->log_n; i++) {
        if (strcmp(g->log[i], tag) == 0) {
            return true;
        }
    }
    return false;
}

/* index of the first log entry matching tag, or LOG_CAP if absent (so a missing
 * tag sorts after any present one in an ordering assertion). */
static int log_index(const char *tag) {
    for (int i = 0; i < g->log_n; i++) {
        if (strcmp(g->log[i], tag) == 0) {
            return i;
        }
    }
    return LOG_CAP;
}

/* store thunks */
static dm_result cb_move(void *c, int s, int d) {
    (void)c;
    g->last_moved_src = s;
    g->last_moved_dst = d;
    log_tag("move");
    return g->move_rc;
}
static dm_result cb_delete(void *c, int i) { (void)c; (void)i; log_tag("delete"); return g->delete_rc; }
static dm_result cb_persist(void *c, int i) {
    (void)c;
    g->last_persist_slot = i;
    log_tag("persist");
    return g->persist_rc;
}
static dm_result cb_commit(void *c, int d) { (void)c; (void)d; log_tag("commit"); return g->commit_rc; }
static dm_result cb_chain(void *c, int s) { (void)c; (void)s; log_tag("chain"); return g->chain_rc; }
static int  cb_draft_count(void *c) { (void)c; return g->draft_count; }
static bool cb_is_empty(void *c, int i) { (void)c; return slot_store_is_empty(&g->store, i); }
static void cb_draft_reset(void *c) { (void)c; log_tag("draft_reset"); }
static void cb_mark_playing(void *c, int i) { (void)c; (void)i; log_tag("mark_playing"); }
static void cb_clear_playing(void *c) { (void)c; log_tag("clear_playing"); }

/*
 * speak — one thunk for every message. It maps spec->kind back to the short tag
 * the assertions use, and records the slot args a few tests check (saved slot,
 * moved src/dst). One spec call per transition replaces the old 24 speak_X
 * pointers — the test surface tracks the collapse.
 *
 * Auto-finish mirrors the real OFF/below-level path: a synchronous transition
 * speak happens while the machine is in TYPING_FEEDBACK (it parked a
 * return-state), and the firmware's OFF build drains it by calling
 * typing_finished. The async deferred-completion speaks happen from IDLE (the
 * deliver_async suppression guard already passed) and are NOT typing
 * transitions, so they must not auto-finish — keying off the state reproduces
 * exactly that split without a per-kind flag.
 */
static const char *kind_tag(const dm_feedback_spec *s, bool async) {
    switch (s->kind) {
    case DM_FB_REC:          return "rec";
    case DM_FB_STOP:         return "stop";
    case DM_FB_NO_REC:       return "no_recording";
    case DM_FB_SAVED:        return "saved";
    case DM_FB_SLOT_FULL:    return "slot_full";
    case DM_FB_SLOT_EMPTY:   return "slot_empty";
    case DM_FB_OVERFLOW:     return "overflow";
    case DM_FB_MOVE_PROMPT:  return "move_prompt";
    case DM_FB_MOVE_SRC:     return "move_src_sel";
    case DM_FB_MOVED:        return "moved";
    case DM_FB_MOVE_CANCEL:  return "move_cancelled";
    case DM_FB_CHAIN_INSERT: return "chain_insert";
    case DM_FB_CHAIN_EMPTY:  return "chain_empty";
    case DM_FB_CHAIN_NO_ROOM:return "chain_no_room";
    case DM_FB_DELETED:      return async ? "async_deleted" : "deleted";
    case DM_FB_DELETE_FAILED:return "async_delete_failed";
    case DM_FB_SAVE_FAILED:  return "async_save_failed";
    case DM_FB_SAVE_QFULL:   return "save_qfull";
    case DM_FB_DELETE_QFULL: return "delete_qfull";
    case DM_FB_KNOB:         return "knob";
    case DM_FB_STATUS_HEADER:return "status";
    case DM_FB_STATUS_SLOT:  return "status_slot";
    default:                 return "?";
    }
}

static void fake_speak(void *c, const dm_feedback_spec *spec) {
    (void)c;
    /* async deferred completions speak from IDLE; sync transitions from TYPING_FEEDBACK */
    bool async = dm_machine_state(&g->m) != DM_STATE_TYPING_FEEDBACK;
    log_tag(kind_tag(spec, async));
    if (spec->kind == DM_FB_SAVED) {
        g->last_saved_slot = spec->slot;
    }
    if (spec->kind == DM_FB_MOVED) {
        g->last_moved_src = spec->slot;
        g->last_moved_dst = spec->slot2;
    }
    if (!async && !g->suppress_auto_finish) {
        dm_machine_typing_finished(&g->m);
    }
}

/* apply_knob — record the command; the real pump would change/persist/confirm
 * and report typing_finished, so settle the parked TYPING_FEEDBACK -> IDLE. */
static void fake_apply_knob(void *c, dm_command cmd) {
    (void)c;
    g->last_knob_cmd = cmd;
    g->knob_calls++;
    log_tag("knob");
    if (!g->suppress_auto_finish) {
        dm_machine_typing_finished(&g->m);
    }
}

static void cb_notify(void *c, int e, int s) {
    (void)c;
    g->last_notify_event = e;
    g->last_notify_slot = s;
    log_tag("notify");
}

static const dm_machine_callbacks fake_cb = {
    .store_move = cb_move,
    .store_delete = cb_delete,
    .store_persist = cb_persist,
    .store_draft_commit = cb_commit,
    .store_draft_chain = cb_chain,
    .store_draft_count = cb_draft_count,
    .store_is_empty = cb_is_empty,
    .store_draft_reset = cb_draft_reset,
    .store_mark_playing = cb_mark_playing,
    .store_clear_playing = cb_clear_playing,
    .speak = fake_speak,
    .apply_knob = fake_apply_knob,
    .notify = cb_notify,
};

/* ---- harness -------------------------------------------------------------- */

static struct fake fx;

static void setup(void) {
    memset(&fx, 0, sizeof(fx));
    g = &fx;
    slot_store_init(&fx.store, NULL); /* RAM-only store for guard queries */
    dm_machine_init(&fx.m, &fx.store, &fake_cb);
}

/* Put a non-empty macro in a RAM slot so is_empty()==false for guard tests.
 * RAM slots live at [NVS_SLOTS, MAX_SLOTS); index 0 may be NVS, so use a
 * known-RAM index via slot_store_load. */
#define RAM0 NVS_SLOTS

static void occupy(int idx) {
    struct dm_event ev = {0};
    ev.keycode = 0x04;
    slot_store_load(&fx.store, idx, &ev, 1);
}

/* Drive a command, returning rc. */
static dm_result cmd(dm_command c, int p) {
    return dm_machine_command(&fx.m, c, p);
}

/* ---- legality matrix: every (state, command) cell -------------------------
 * The matrix is two-valued: a command in a state is either ALLOWED (does
 * something) or IGNORED (DM_OK, no side effect). We drive the machine into
 * each state and assert the IGNORED cells produce no callback at all. */

/* Force the machine into a target state via the public command path. */
static void goto_state(dm_state want) {
    switch (want) {
    case DM_STATE_IDLE:
        break;
    case DM_STATE_RECORDING:
        cmd(DM_CMD_REC, 0);
        break;
    case DM_STATE_PENDING_ASSIGN:
        fx.draft_count = 1;
        cmd(DM_CMD_REC, 0);
        cmd(DM_CMD_STP, 0);
        break;
    case DM_STATE_DELETE_PENDING:
        cmd(DM_CMD_DEL, 0);
        break;
    case DM_STATE_MOVE_PENDING:
        cmd(DM_CMD_MOV, 0);
        break;
    case DM_STATE_PREVIEW_PENDING:
        cmd(DM_CMD_PREVIEW, 0);
        break;
    case DM_STATE_PLAYING:
        occupy(RAM0);
        cmd(DM_CMD_SLOT, RAM0);
        break;
    default:
        break;
    }
}

ZTEST(dm_machine, init_is_idle) {
    setup();
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, rec_from_idle_starts_recording) {
    setup();
    dm_result rc = cmd(DM_CMD_REC, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
    zassert_true(log_has("draft_reset"), NULL);
    zassert_true(log_has("rec"), NULL);
}

ZTEST(dm_machine, rec_during_playing_is_ignored) {
    setup();
    goto_state(DM_STATE_PLAYING);
    fx.log_n = 0; /* clear playback noise */
    dm_result rc = cmd(DM_CMD_REC, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_PLAYING, NULL);
    zassert_false(log_has("draft_reset"), NULL); /* no side effect */
}

ZTEST(dm_machine, stp_from_idle_is_ignored) {
    setup();
    dm_result rc = cmd(DM_CMD_STP, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
    zassert_equal(fx.log_n, 0, NULL);
}

ZTEST(dm_machine, stp_with_draft_goes_pending_assign) {
    setup();
    cmd(DM_CMD_REC, 0);
    fx.draft_count = 3;
    dm_result rc = cmd(DM_CMD_STP, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_PENDING_ASSIGN, NULL);
    zassert_true(log_has("stop"), NULL);
}

ZTEST(dm_machine, stp_empty_draft_back_to_idle) {
    setup();
    cmd(DM_CMD_REC, 0);
    fx.draft_count = 0;
    dm_result rc = cmd(DM_CMD_STP, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
    zassert_true(log_has("no_recording"), NULL);
}

/* ---- guards: assign --------------------------------------------------------*/

ZTEST(dm_machine, assign_to_empty_commits_then_persists_on_finish) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.log_n = 0;
    /* slot RAM0 is empty -> commit succeeds; the SAVED cue routes through
     * TYPING_FEEDBACK and persist fires from typing_finished, which the fake
     * speak drives synchronously (the OFF-path). The ORDER is the invariant:
     * commit (RAM-only) before persist (the deferred enqueue). */
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("commit"), NULL);
    zassert_true(log_has("saved"), NULL);
    zassert_true(log_has("persist"), NULL);
    zassert_true(log_index("commit") < log_index("persist"), NULL);
    zassert_equal(fx.last_persist_slot, RAM0, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, assign_persist_is_deferred_until_finish) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.log_n = 0;
    /* With a speak that does NOT auto-finish (the live-typing case), the commit
     * happens but persist must wait for typing_finished. */
    fx.suppress_auto_finish = true;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("commit"), NULL);
    zassert_false(log_has("persist"), NULL); /* deferred */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_TYPING_FEEDBACK, NULL);
    dm_machine_typing_finished(&fx.m);
    zassert_true(log_has("persist"), NULL);
    zassert_equal(fx.last_persist_slot, RAM0, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
    fx.suppress_auto_finish = false;
}

ZTEST(dm_machine, assign_to_occupied_rejects_and_keeps_pending) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    occupy(RAM0);
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_REJECTED_OCCUPIED, NULL);
    zassert_true(log_has("slot_full"), NULL);
    /* preserved pending state, not dropped to idle */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_PENDING_ASSIGN, NULL);
    zassert_false(log_has("commit"), NULL);
}

/* ---- guards: move --------------------------------------------------------- */

ZTEST(dm_machine, move_empty_source_rejected_stays_pending) {
    setup();
    goto_state(DM_STATE_MOVE_PENDING);
    fx.log_n = 0;
    /* RAM0 empty -> source selection rejected */
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_REJECTED_EMPTY, NULL);
    zassert_true(log_has("slot_empty"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_MOVE_PENDING, NULL);
}

ZTEST(dm_machine, move_source_then_dest_completes) {
    setup();
    goto_state(DM_STATE_MOVE_PENDING);
    occupy(RAM0);
    cmd(DM_CMD_SLOT, RAM0); /* select source */
    zassert_true(log_has("move_src_sel"), NULL);
    fx.log_n = 0;
    fx.move_rc = DM_OK;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0 + 1); /* dest (empty) */
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("move"), NULL);
    zassert_equal(fx.last_moved_src, RAM0, NULL);
    zassert_equal(fx.last_moved_dst, RAM0 + 1, NULL);
    zassert_true(log_has("moved"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, same_slot_move_is_cancel_not_rejection) {
    setup();
    goto_state(DM_STATE_MOVE_PENDING);
    occupy(RAM0);
    cmd(DM_CMD_SLOT, RAM0); /* select source */
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0); /* same slot */
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("move_cancelled"), NULL);
    zassert_false(log_has("move"), NULL); /* never reaches the store */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, move_dst_save_queue_full_surfaces) {
    setup();
    goto_state(DM_STATE_MOVE_PENDING);
    occupy(RAM0);
    cmd(DM_CMD_SLOT, RAM0);
    fx.log_n = 0;
    fx.move_rc = DM_SAVE_QUEUE_FULL;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0 + 1);
    zassert_equal(rc, DM_SAVE_QUEUE_FULL, NULL);
    zassert_true(log_has("save_qfull"), NULL);
    zassert_false(log_has("moved"), NULL);
    /* state settled before the effect (transaction rule) */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, move_src_delete_queue_full_surfaces) {
    setup();
    goto_state(DM_STATE_MOVE_PENDING);
    occupy(RAM0);
    cmd(DM_CMD_SLOT, RAM0);
    fx.log_n = 0;
    fx.move_rc = DM_DELETE_QUEUE_FULL;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0 + 1);
    zassert_equal(rc, DM_DELETE_QUEUE_FULL, NULL);
    zassert_true(log_has("delete_qfull"), NULL);
    zassert_false(log_has("moved"), NULL);
}

/* ---- guards: delete ------------------------------------------------------- */

ZTEST(dm_machine, delete_empty_slot_back_to_idle) {
    setup();
    goto_state(DM_STATE_DELETE_PENDING);
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0); /* empty */
    zassert_equal(rc, DM_REJECTED_EMPTY, NULL);
    zassert_true(log_has("slot_empty"), NULL);
    /* the one drop-to-idle case */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, delete_occupied_slot_deletes_and_idles) {
    setup();
    goto_state(DM_STATE_DELETE_PENDING);
    occupy(RAM0);
    fx.log_n = 0;
    fx.delete_rc = DM_OK;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("delete"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

/* ---- recording: chain ----------------------------------------------------- */

ZTEST(dm_machine, chain_empty_source_stays_recording) {
    setup();
    goto_state(DM_STATE_RECORDING);
    fx.log_n = 0;
    /* RAM0 empty -> chain rejected, stays RECORDING */
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_REJECTED_EMPTY, NULL);
    zassert_true(log_has("chain_empty"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}

ZTEST(dm_machine, chain_no_room_stays_recording) {
    setup();
    goto_state(DM_STATE_RECORDING);
    occupy(RAM0);
    fx.log_n = 0;
    fx.chain_rc = DM_REJECTED_FULL;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_REJECTED_FULL, NULL);
    zassert_true(log_has("chain_no_room"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}

ZTEST(dm_machine, chain_ok_inserts_stays_recording) {
    setup();
    goto_state(DM_STATE_RECORDING);
    occupy(RAM0);
    fx.log_n = 0;
    fx.chain_rc = DM_OK;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("chain_insert"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}

ZTEST(dm_machine, overflow_during_recording_goes_pending_assign) {
    setup();
    goto_state(DM_STATE_RECORDING);
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_OVERFLOW, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("overflow"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_PENDING_ASSIGN, NULL);
}

ZTEST(dm_machine, overflow_outside_recording_is_ignored) {
    setup();
    dm_result rc = cmd(DM_CMD_OVERFLOW, 0); /* from IDLE */
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
    zassert_false(log_has("overflow"), NULL);
}

/* ---- REC-during-pending-assign discards the take -------------------------- */

ZTEST(dm_machine, rec_from_pending_assign_discards_take) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.draft_count = 5; /* a finished-but-unassigned recording */
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_REC, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("draft_reset"), NULL); /* the take is discarded */
    zassert_true(log_has("rec"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}

/* ---- play ----------------------------------------------------------------- */

ZTEST(dm_machine, play_empty_slot_rejected) {
    setup();
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0); /* empty, from IDLE */
    zassert_equal(rc, DM_REJECTED_EMPTY, NULL);
    zassert_true(log_has("slot_empty"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, play_occupied_slot_starts_playing) {
    setup();
    occupy(RAM0);
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("mark_playing"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_PLAYING, NULL);
}

/* ---- notifications fire before speak, at every level ---------------------- */

ZTEST(dm_machine, notify_precedes_speak_on_saved) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.log_n = 0;
    cmd(DM_CMD_SLOT, RAM0);
    /* notify must appear before saved in the log */
    int ni = -1, si = -1;
    for (int i = 0; i < fx.log_n; i++) {
        if (strcmp(fx.log[i], "notify") == 0 && ni < 0) ni = i;
        if (strcmp(fx.log[i], "saved") == 0 && si < 0) si = i;
    }
    zassert_true(ni >= 0, NULL);
    zassert_true(si >= 0, NULL);
    zassert_true(ni < si, NULL);
}

/* ---- deferred async completion: IDLE-suppression -------------------------- */

ZTEST(dm_machine, deliver_async_deleted_speaks_when_idle) {
    setup();
    fx.log_n = 0;
    dm_machine_deliver_async(&fx.m, DM_OK, RAM0);
    zassert_true(log_has("async_deleted"), NULL);
}

ZTEST(dm_machine, deliver_async_suppressed_when_busy) {
    setup();
    goto_state(DM_STATE_RECORDING);
    fx.log_n = 0;
    dm_machine_deliver_async(&fx.m, DM_DELETE_FAILED, RAM0);
    /* dropped: not IDLE, must not hijack the active op */
    zassert_false(log_has("async_delete_failed"), NULL);
}

ZTEST(dm_machine, deliver_async_save_failed_speaks_when_idle) {
    setup();
    fx.log_n = 0;
    dm_machine_deliver_async(&fx.m, DM_SAVE_FAILED, RAM0);
    zassert_true(log_has("async_save_failed"), NULL);
}

/* ---- auto-erase up-calls -------------------------------------------------- */

ZTEST(dm_machine, erase_due_parks_state) {
    setup();
    /* park from IDLE. erase_due only writes TYPING_ERASE; the erase emission is
     * armed by the pump's scheduler itself (it pushes backspaces right after
     * calling erase_due), so the machine does NOT speak here — there is no
     * phantom speak_erase. */
    dm_machine_erase_due(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_TYPING_ERASE, NULL);
    zassert_false(log_has("knob"), NULL);
    zassert_false(log_has("erase"), NULL);
}

ZTEST(dm_machine, erase_cancel_restores_parked_state) {
    setup();
    dm_machine_erase_due(&fx.m);
    dm_machine_erase_cancel(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, erase_due_parks_once_across_batches) {
    setup();
    dm_machine_erase_due(&fx.m); /* parks IDLE, -> TYPING_ERASE */
    dm_machine_erase_due(&fx.m); /* second batch keeps the original parked state */
    dm_machine_erase_cancel(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

/* ---- timeout drops a pending state to IDLE -------------------------------- */

ZTEST(dm_machine, timeout_from_move_pending_idles_and_clears_source) {
    setup();
    goto_state(DM_STATE_MOVE_PENDING);
    occupy(RAM0);
    cmd(DM_CMD_SLOT, RAM0); /* select a move source */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_MOVE_PENDING, NULL);
    dm_machine_timeout(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
    /* source cleared: a later move starts fresh (no stale source carried over) */
    goto_state(DM_STATE_MOVE_PENDING);
    occupy(RAM0 + 1);
    fx.log_n = 0;
    cmd(DM_CMD_SLOT, RAM0 + 1);
    zassert_true(log_has("move_src_sel"), NULL);
}

ZTEST(dm_machine, timeout_from_pending_assign_idles) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    dm_machine_timeout(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, timeout_outside_pending_is_noop) {
    setup();
    goto_state(DM_STATE_RECORDING);
    dm_machine_timeout(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}

/* ---- knob commands are IDLE-only ----------------------------------------- */

ZTEST(dm_machine, style_toggle_idle_only) {
    setup();
    goto_state(DM_STATE_RECORDING);
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_STYLE_TOGGLE, 0);
    zassert_equal(rc, DM_OK, NULL); /* IGNORED outside IDLE */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}

ZTEST(dm_machine, status_idle_only) {
    setup();
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_STATE, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("status"), NULL);
}

/* ---- knob commands drive apply_knob INSIDE the transition ----------------- */

ZTEST(dm_machine, knob_from_idle_applies_inside_transition) {
    setup();
    fx.log_n = 0;
    /* ALLOWED in IDLE -> the machine parks IDLE and calls apply_knob with the
     * command (the effect/persist/confirm are feedback's, driven from here, not
     * re-run by the shell). */
    dm_result rc = cmd(DM_CMD_STYLE_TOGGLE, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_equal(fx.knob_calls, 1, NULL);
    zassert_equal(fx.last_knob_cmd, DM_CMD_STYLE_TOGGLE, NULL);
    zassert_true(log_has("knob"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, knob_ignored_outside_idle_does_not_apply) {
    setup();
    goto_state(DM_STATE_RECORDING);
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_FEEDBACK_INC, 0);
    zassert_equal(rc, DM_OK, NULL); /* IGNORED */
    zassert_equal(fx.knob_calls, 0, NULL); /* apply_knob never reached */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
}
