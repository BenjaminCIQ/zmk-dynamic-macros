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
#include "../../src/slot_store_priv.h"

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
};

static struct fake *g; /* current fake for the callback thunks */

static void logf(const char *tag) {
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

/* store thunks */
static dm_result cb_move(void *c, int s, int d) {
    (void)c;
    g->last_moved_src = s;
    g->last_moved_dst = d;
    logf("move");
    return g->move_rc;
}
static dm_result cb_delete(void *c, int i) { (void)c; (void)i; logf("delete"); return g->delete_rc; }
static dm_result cb_persist(void *c, int i) {
    (void)c;
    g->last_persist_slot = i;
    logf("persist");
    return g->persist_rc;
}
static dm_result cb_commit(void *c, int d) { (void)c; (void)d; logf("commit"); return g->commit_rc; }
static dm_result cb_chain(void *c, int s) { (void)c; (void)s; logf("chain"); return g->chain_rc; }
static int  cb_draft_count(void *c) { (void)c; return g->draft_count; }
static bool cb_is_empty(void *c, int i) { (void)c; return slot_store_is_empty(&g->store, i); }
static void cb_draft_reset(void *c) { (void)c; logf("draft_reset"); }
static void cb_mark_playing(void *c, int i) { (void)c; (void)i; logf("mark_playing"); }
static void cb_clear_playing(void *c) { (void)c; logf("clear_playing"); }

/* speak thunks */
static void cb_rec(void *c) { (void)c; logf("rec"); }
static void cb_stop(void *c) { (void)c; logf("stop"); }
static void cb_no_recording(void *c) { (void)c; logf("no_recording"); }
static void cb_saved(void *c, int s) { (void)c; g->last_saved_slot = s; logf("saved"); }
static void cb_deleted(void *c, int s) { (void)c; (void)s; logf("deleted"); }
static void cb_slot_empty(void *c, int s) { (void)c; (void)s; logf("slot_empty"); }
static void cb_slot_full(void *c, int s) { (void)c; (void)s; logf("slot_full"); }
static void cb_chain_insert(void *c, int s) { (void)c; (void)s; logf("chain_insert"); }
static void cb_chain_empty(void *c, int s) { (void)c; (void)s; logf("chain_empty"); }
static void cb_chain_no_room(void *c, int s) { (void)c; (void)s; logf("chain_no_room"); }
static void cb_overflow(void *c) { (void)c; logf("overflow"); }
static void cb_move_prompt(void *c) { (void)c; logf("move_prompt"); }
static void cb_move_src_sel(void *c, int s) { (void)c; (void)s; logf("move_src_sel"); }
static void cb_move_cancelled(void *c) { (void)c; logf("move_cancelled"); }
static void cb_moved(void *c, int s, int d) { (void)c; (void)s; (void)d; logf("moved"); }
static void cb_save_qfull(void *c, int s) { (void)c; (void)s; logf("save_qfull"); }
static void cb_delete_qfull(void *c, int s) { (void)c; (void)s; logf("delete_qfull"); }
static void cb_status(void *c) { (void)c; logf("status"); }
static void cb_preview(void *c, int s) { (void)c; (void)s; logf("preview"); }
static void cb_async_deleted(void *c, int s) { (void)c; (void)s; logf("async_deleted"); }
static void cb_async_save_failed(void *c, int s) { (void)c; (void)s; logf("async_save_failed"); }
static void cb_async_delete_failed(void *c, int s) { (void)c; (void)s; logf("async_delete_failed"); }
static void cb_erase(void *c) { (void)c; logf("erase"); }
static void cb_notify(void *c, int e, int s) {
    (void)c;
    g->last_notify_event = e;
    g->last_notify_slot = s;
    logf("notify");
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
    .speak_rec = cb_rec,
    .speak_stop = cb_stop,
    .speak_no_recording = cb_no_recording,
    .speak_saved = cb_saved,
    .speak_deleted = cb_deleted,
    .speak_slot_empty = cb_slot_empty,
    .speak_slot_full = cb_slot_full,
    .speak_chain_insert = cb_chain_insert,
    .speak_chain_empty = cb_chain_empty,
    .speak_chain_no_room = cb_chain_no_room,
    .speak_overflow = cb_overflow,
    .speak_move_prompt = cb_move_prompt,
    .speak_move_source_selected = cb_move_src_sel,
    .speak_move_cancelled = cb_move_cancelled,
    .speak_moved = cb_moved,
    .speak_save_queue_full = cb_save_qfull,
    .speak_delete_queue_full = cb_delete_qfull,
    .speak_status = cb_status,
    .speak_preview = cb_preview,
    .speak_async_deleted = cb_async_deleted,
    .speak_async_save_failed = cb_async_save_failed,
    .speak_async_delete_failed = cb_async_delete_failed,
    .speak_erase = cb_erase,
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

ZTEST(dm_machine, assign_to_empty_commits_and_defers_persist) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.log_n = 0;
    /* slot RAM0 is empty -> commit succeeds */
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("commit"), NULL);
    zassert_true(log_has("saved"), NULL);
    /* persist is DEFERRED to typing_finished, not fired during the command */
    zassert_false(log_has("persist"), NULL);
}

ZTEST(dm_machine, assign_persist_fires_on_typing_finished) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    cmd(DM_CMD_SLOT, RAM0);
    fx.log_n = 0;
    dm_machine_typing_finished(&fx.m);
    zassert_true(log_has("persist"), NULL);
    zassert_equal(fx.last_persist_slot, RAM0, NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
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

ZTEST(dm_machine, erase_due_parks_state_and_types) {
    setup();
    /* park from IDLE */
    dm_machine_erase_due(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_TYPING_ERASE, NULL);
    zassert_true(log_has("erase"), NULL);
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
