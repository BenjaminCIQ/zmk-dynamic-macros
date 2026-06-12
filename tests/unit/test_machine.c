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
#include <zmk-behavior-dynamic-macros/dm_notify.h>
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

    /* set by deliver_async() (the test wrapper) for the duration of the call, so
     * fake_speak can tag the deferred-completion messages distinctly and NOT
     * auto-finish them — they are not synchronous transition speaks. Both sync
     * and async paths now enter TYPING_FEEDBACK before speaking, so the calling
     * context (not the state) is the honest signal. */
    bool in_deliver_async;
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
 * moved src/dst). One spec call per transition carries every message kind.
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
    /* deferred-completion speaks come from deliver_async; everything else is a
     * synchronous transition speak. The real pump drives typing_finished later
     * for both, but a sync transition's OFF-path finishes synchronously. */
    bool async = g->in_deliver_async;
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

/* Drive a deferred completion, flagging the context so fake_speak tags it async
 * and does not auto-finish (the real pump drives typing_finished later). */
static void deliver_async(dm_result outcome, int slot) {
    fx.in_deliver_async = true;
    dm_machine_deliver_async(&fx.m, outcome, slot);
    fx.in_deliver_async = false;
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
    case DM_STATE_TYPING_FEEDBACK:
        /* a speak transition parks a return-state and enters TYPING_FEEDBACK;
         * suppress the auto-finish so the machine stays there (the live-typing
         * case where the ring drain is a separate later event). */
        fx.suppress_auto_finish = true;
        cmd(DM_CMD_REC, 0);
        break;
    case DM_STATE_TYPING_ERASE:
        dm_machine_erase_due(&fx.m); /* parks IDLE, writes TYPING_ERASE */
        break;
    default:
        break;
    }
}

/* ---- exhaustive legality sweep: every (state, command) cell ----------------
 *
 * The design doc (§2.2) claims the legality[state][command] matrix is
 * "exhaustively testable (9 states x 13 commands), every cell a known verdict".
 * The hand-picked cell tests below prove representative cells; this sweep proves
 * ALL of them, so a single stray flipped cell (exactly the class of the
 * REC-from-PREVIEW_PENDING defect caught at the pre-cut-over review) is caught
 * mechanically.
 *
 * The expected table here is derived INDEPENDENTLY from the prose rules — it is
 * NOT a copy of the production `legality[][]` (that would be tautological). The
 * rules, verbatim from §2.2 / the matrix comment:
 *   - REC: restarts recording from IDLE, RECORDING, PENDING_ASSIGN, PREVIEW_PENDING.
 *   - STP: only in RECORDING.
 *   - DEL / MOV / STATE / PREVIEW / the four knobs / TEST_RELOAD: IDLE-only.
 *
 * One documented exception to the effect-witness: TEST_RELOAD is matrix-ALLOWED
 * in IDLE (so the gate passes it to the shell, which performs the reload — §2.4)
 * but the MACHINE handler returns DM_OK with no transition ("dispatched by
 * dm_nvs, not a transition", dm_machine.c). It is therefore the lone ALLOWED cell
 * that produces no machine-side observable, so the effect-witness can't see it;
 * the sweep skips the effect check for exactly that cell and asserts its
 * gate-passes-through verdict separately below.
 *   - SLOT: every state that consumes a slot press — IDLE (play), RECORDING
 *     (chain), PENDING_ASSIGN (assign), DELETE_PENDING, MOVE_PENDING,
 *     PREVIEW_PENDING; NOT PLAYING/TYPING_*.
 *   - OVERFLOW: internal, only in RECORDING.
 *   - PLAYING, TYPING_FEEDBACK, TYPING_ERASE: machine-busy, ignore everything.
 *
 * Witness: a command is ALLOWED iff it produces an observable effect — a state
 * change OR at least one logged callback (notify/speak/apply_knob/store). An
 * IGNORED command returns DM_OK before any handler, so state is unchanged AND
 * log_n == 0. (The lone ALLOWED-without-a-log cell, DEL from IDLE -> arms the
 * timeout + writes DELETE_PENDING, is caught by the state-change half; the fake
 * does not wire arm/cancel_timeout, so those never log.)
 */
enum { A = 1, I = 0 }; /* ALLOWED / IGNORED, local so it can't alias production */

static const uint8_t expected_legality[9][DM_CMD__COUNT] = {
    /* state                  REC STP DEL MOV SLOT STATE PRE F+  F-  STY ERS RLD OVF */
    [DM_STATE_IDLE]         = { A,  I,  A,  A,  A,   A,   A,  A,  A,  A,  A,  A,  I },
    [DM_STATE_RECORDING]    = { A,  A,  I,  I,  A,   I,   I,  I,  I,  I,  I,  I,  A },
    [DM_STATE_PENDING_ASSIGN]={ A,  I,  I,  I,  A,   I,   I,  I,  I,  I,  I,  I,  I },
    [DM_STATE_DELETE_PENDING]={ I,  I,  I,  I,  A,   I,   I,  I,  I,  I,  I,  I,  I },
    [DM_STATE_MOVE_PENDING] = { I,  I,  I,  I,  A,   I,   I,  I,  I,  I,  I,  I,  I },
    [DM_STATE_PREVIEW_PENDING]={A,  I,  I,  I,  A,   I,   I,  I,  I,  I,  I,  I,  I },
    [DM_STATE_PLAYING]      = { I,  I,  I,  I,  I,   I,   I,  I,  I,  I,  I,  I,  I },
    [DM_STATE_TYPING_FEEDBACK]={I,  I,  I,  I,  I,   I,   I,  I,  I,  I,  I,  I,  I },
    [DM_STATE_TYPING_ERASE] = { I,  I,  I,  I,  I,   I,   I,  I,  I,  I,  I,  I,  I },
};

ZTEST(dm_machine, legality_matrix_every_cell) {
    for (int st = 0; st < 9; st++) {
        for (int c = 0; c < DM_CMD__COUNT; c++) {
            setup();
            goto_state((dm_state)st);
            /* a goto into PENDING_ASSIGN via STP needs a non-empty draft; keep one
             * staged for the whole sweep so the pending states are reachable and
             * SLOT-as-assign has a draft to commit. */
            fx.draft_count = 1;
            dm_state pre = dm_machine_state(&fx.m);
            fx.log_n = 0;

            /* SLOT needs a slot param; use an empty RAM slot so every ALLOWED SLOT
             * context still fires an observable (play-empty/chain-empty/delete-
             * empty/move-source/preview/assign all speak or notify or commit). */
            int param = (c == DM_CMD_SLOT) ? (RAM0 + 1) : 0;
            cmd((dm_command)c, param);

            /* TEST_RELOAD is gate-ALLOWED but effect-free in the machine (see the
             * header comment): the effect-witness genuinely cannot see it, so skip
             * it here and pin its verdict in legality_test_reload_passes_gate. */
            if (c == DM_CMD_TEST_RELOAD) {
                continue;
            }

            bool observed = (dm_machine_state(&fx.m) != pre) || (fx.log_n > 0);
            bool want_allowed = (expected_legality[st][c] == A);
            zassert_equal(observed, want_allowed,
                          "state %d cmd %d: expected %s, observed %s", st, c,
                          want_allowed ? "ALLOWED" : "IGNORED",
                          observed ? "ALLOWED" : "IGNORED");
        }
    }
}

/* The cell the sweep deliberately skips: TEST_RELOAD is matrix-ALLOWED in IDLE
 * (the gate passes it to the shell, which performs the reload — §2.4) yet the
 * machine handler returns DM_OK with no transition. Its observable contract is
 * therefore "never a machine-side effect, in ANY state": IDLE passes the gate to
 * a no-op handler; non-IDLE is dropped by the gate. Both return DM_OK with no
 * state change and no callback, which is exactly what this pins — so the reload
 * dispatch stays the shell's job and never accidentally becomes a transition. */
ZTEST(dm_machine, legality_test_reload_passes_gate) {
    for (int st = 0; st < 9; st++) {
        setup();
        goto_state((dm_state)st);
        dm_state pre = dm_machine_state(&fx.m);
        fx.log_n = 0;
        dm_result rc = cmd(DM_CMD_TEST_RELOAD, 0);
        zassert_equal(rc, DM_OK, "state %d: TEST_RELOAD must return DM_OK", st);
        zassert_equal(dm_machine_state(&fx.m), pre,
                      "state %d: TEST_RELOAD must not transition the machine", st);
        zassert_equal(fx.log_n, 0,
                      "state %d: TEST_RELOAD must not call back", st);
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

ZTEST(dm_machine, rec_from_preview_pending_starts_recording) {
    setup();
    goto_state(DM_STATE_PREVIEW_PENDING);
    fx.log_n = 0;
    /* REC is allowed from PREVIEW_PENDING: it cancels the preview timeout and
     * starts a fresh recording. */
    dm_result rc = cmd(DM_CMD_REC, 0);
    zassert_equal(rc, DM_OK, NULL);
    zassert_true(log_has("draft_reset"), NULL);
    zassert_true(log_has("rec"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_RECORDING, NULL);
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

ZTEST(dm_machine, assign_persist_queue_full_surfaces) {
    setup();
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.log_n = 0;
    /* the deferred assign-persist enqueue is refused: the machine must NOT drop it
     * silently — it speaks SAVE QUEUE FULL and raises ERROR_QUEUE_FULL, naming the
     * slot, from the settled state. */
    fx.persist_rc = DM_SAVE_QUEUE_FULL;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_OK, NULL); /* the assign itself committed */
    zassert_true(log_has("persist"), NULL);
    zassert_true(log_has("save_qfull"), NULL);
    /* the ERROR_QUEUE_FULL notification fired for the slot */
    zassert_equal(fx.last_notify_event, DM_EVT_ERROR_QUEUE_FULL, NULL);
    zassert_equal(fx.last_notify_slot, RAM0, NULL);
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
    /* a synchronous (RAM) delete speaks DELETED now */
    zassert_true(log_has("deleted"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, delete_nvs_defers_speech_to_completion) {
    setup();
    goto_state(DM_STATE_DELETE_PENDING);
    occupy(RAM0);
    fx.log_n = 0;
    /* the NVS store enqueues and returns DM_DELETE_DEFERRED: the transition must
     * NOT notify or speak DELETED — that waits for deliver_async — but it must
     * still settle to IDLE so deliver_async's IDLE-suppression passes. */
    fx.delete_rc = DM_DELETE_DEFERRED;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0);
    zassert_equal(rc, DM_DELETE_DEFERRED, NULL);
    zassert_true(log_has("delete"), NULL);
    zassert_false(log_has("deleted"), NULL);
    zassert_false(log_has("notify"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);

    /* the completion then speaks DELETED exactly once */
    fx.log_n = 0;
    deliver_async(DM_OK, RAM0);
    zassert_true(log_has("async_deleted"), NULL);
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

/* Regression (native_sim core/pool_full: slot 0 played nothing). On the OFF /
 * below-level path, speak() finishes synchronously via typing_finished, which
 * restores m->return_state. The empty-play branch therefore MUST park IDLE as the
 * return-state first; otherwise it restores whatever a PRIOR transition parked.
 *
 * Repro: a rejected assign parks return_state = PENDING_ASSIGN and stays pending;
 * a timeout drops the machine to IDLE; then playing an EMPTY slot speaks
 * SLOT_EMPTY. Pre-fix, the OFF-path typing_finished resurrected PENDING_ASSIGN
 * (and re-armed its timeout), so the NEXT slot press was eaten as an assign
 * instead of a play — exactly the swallowed playback the pool_full native_sim case caught. */
ZTEST(dm_machine, empty_play_does_not_resurrect_parked_state) {
    setup();
    /* a rejected assign: stays PENDING_ASSIGN, parks return_state = PENDING_ASSIGN */
    goto_state(DM_STATE_PENDING_ASSIGN);
    fx.commit_rc = DM_REJECTED_FULL;
    cmd(DM_CMD_SLOT, RAM0); /* RAM0 empty -> slot_full reject path keeps PENDING_ASSIGN */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_PENDING_ASSIGN, NULL);

    /* the pending assign times out back to IDLE */
    dm_machine_timeout(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);

    /* play an empty slot from IDLE: must stay IDLE, NOT bounce to PENDING_ASSIGN */
    fx.log_n = 0;
    dm_result rc = cmd(DM_CMD_SLOT, RAM0 + 1); /* empty */
    zassert_equal(rc, DM_REJECTED_EMPTY, NULL);
    zassert_true(log_has("slot_empty"), NULL);
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE,
                  "empty play must not resurrect the parked PENDING_ASSIGN");
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
    deliver_async(DM_OK, RAM0);
    zassert_true(log_has("async_deleted"), NULL);
}

ZTEST(dm_machine, deliver_async_suppressed_when_busy) {
    setup();
    goto_state(DM_STATE_RECORDING);
    fx.log_n = 0;
    deliver_async(DM_DELETE_FAILED, RAM0);
    /* dropped: not IDLE, must not hijack the active op */
    zassert_false(log_has("async_delete_failed"), NULL);
}

ZTEST(dm_machine, deliver_async_save_failed_speaks_when_idle) {
    setup();
    fx.log_n = 0;
    deliver_async(DM_SAVE_FAILED, RAM0);
    zassert_true(log_has("async_save_failed"), NULL);
}

ZTEST(dm_machine, deliver_async_stale_is_dropped) {
    setup();
    fx.log_n = 0;
    /* a superseded delete-completion (DM_DELETE_STALE) must speak nothing — it is
     * not a real deletion, just an op the slot already moved past. */
    deliver_async(DM_DELETE_STALE, RAM0);
    zassert_false(log_has("async_deleted"), NULL);
    zassert_false(log_has("notify"), NULL);
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

ZTEST(dm_machine, erase_finished_restores_parked_state) {
    setup();
    dm_machine_erase_due(&fx.m); /* parks IDLE, -> TYPING_ERASE */
    dm_machine_erase_finished(&fx.m); /* clean drain restores the parked state */
    zassert_equal(dm_machine_state(&fx.m), DM_STATE_IDLE, NULL);
}

ZTEST(dm_machine, erase_cancel_after_finish_is_noop) {
    setup();
    /* the regression: after a clean erase drain (erase_finished) the erase-active
     * flag must be clear, so a later keycode's erase_cancel does NOT resurrect the
     * already-restored parked state. Drive PLAYING in as a state that erase_cancel
     * would visibly clobber back to the (stale) parked IDLE if the flag leaked. */
    occupy(RAM0);
    cmd(DM_CMD_SLOT, RAM0); /* IDLE -> PLAYING */
    /* an erase that began before play and drained cleanly */
    dm_machine_erase_due(&fx.m);
    dm_machine_erase_finished(&fx.m);
    /* now PLAYING; a phantom erase_cancel must be a no-op (flag cleared) */
    dm_state before = dm_machine_state(&fx.m);
    dm_machine_erase_cancel(&fx.m);
    zassert_equal(dm_machine_state(&fx.m), before,
                  "erase_cancel after a clean finish must not change state");
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
