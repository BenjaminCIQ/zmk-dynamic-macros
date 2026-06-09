/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Tests for slot_store. They drive the interface and pin the invariants:
 *
 *   - move with dst-persist-fail rolls back: src intact, dst zeroed
 *   - move dst-ok but src-delete-enqueue-fail: dst safe, error surfaced
 *   - delete-while-playing skips the RAM zero on completion
 *   - a stale-generation completion is ignored
 *   - draft append fills to MAX_EVENTS; chain rejects empty / no-room; commit
 *     copies draft -> slot, RAM-only
 *   - persist is the separate deferred enqueue (DM_OK | DM_SAVE_QUEUE_FULL)
 *   - load / reset are the restore surface: populate without persist echo or
 *     generation bump; reset zeroes slots/pending/generations
 *
 * White-box: the store tests read slots[]/pending_delete/slot_generation via
 * the private layout to assert the dual-write outcome directly.
 *
 * The fake dm_nvs sink records the last op and can be armed to fail the next
 * enqueue (DM_SAVE_QUEUE_FULL / DM_DELETE_QUEUE_FULL), letting the ordering +
 * rollback be exercised with nothing but a C compiler.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ztest_shim.h"

#include <zmk-behavior-dynamic-macros/slot_store.h>
#include "../../src/slot_store_priv.h"

/* ---- fake dm_nvs sink ------------------------------------------------------ */

struct fake_nvs {
    int      save_calls;
    int      del_calls;
    int      last_save_slot;
    int      last_del_slot;
    uint32_t last_save_gen;
    uint32_t last_del_gen;
    /* arm a single-shot enqueue failure for the next save/del */
    bool     fail_next_save;
    bool     fail_next_del;
    /* ordering witness: the sequence of ops as they arrive */
    char     trace[64];
    int      trace_len;
};

static void fake_trace(struct fake_nvs *f, char c, int slot) {
    if (f->trace_len + 2 < (int)sizeof(f->trace)) {
        f->trace[f->trace_len++] = c;
        f->trace[f->trace_len++] = (char)('0' + slot);
        f->trace[f->trace_len] = '\0';
    }
}

static dm_result fake_save(void *ctx, int slot, const struct dm_slot *s, uint32_t generation) {
    struct fake_nvs *f = ctx;
    (void)s;
    if (f->fail_next_save) {
        f->fail_next_save = false;
        return DM_SAVE_QUEUE_FULL;
    }
    f->save_calls++;
    f->last_save_slot = slot;
    f->last_save_gen = generation;
    fake_trace(f, 'S', slot);
    return DM_OK;
}

static dm_result fake_del(void *ctx, int slot, uint32_t generation) {
    struct fake_nvs *f = ctx;
    if (f->fail_next_del) {
        f->fail_next_del = false;
        return DM_DELETE_QUEUE_FULL;
    }
    f->del_calls++;
    f->last_del_slot = slot;
    f->last_del_gen = generation;
    fake_trace(f, 'D', slot);
    return DM_OK;
}

/* ---- fixtures -------------------------------------------------------------- */

/* NVS slots are [0, NVS_SLOTS); pick two low indices that are both NVS-backed.
 * RAM slots are [NVS_SLOTS, MAX_SLOTS). */
#define NVS_A 0
#define NVS_B 1
#define RAM_A NVS_SLOTS

static struct fake_nvs g_nvs;
static slot_store      g_store;

static slot_store *fresh_store(void) {
    memset(&g_nvs, 0, sizeof(g_nvs));
    static dm_nvs_sink sink;
    sink = (dm_nvs_sink){.save = fake_save, .del = fake_del, .ctx = &g_nvs};
    slot_store_init(&g_store, &sink);
    return &g_store;
}

/* Put `n` synthetic events directly into a slot (test helper; bypasses draft). */
static void seed_slot(slot_store *s, int idx, uint32_t n) {
    s->slots[idx].event_count = n;
    for (uint32_t i = 0; i < n && i < MAX_EVENTS; i++) {
        s->slots[idx].events[i].keycode = (uint16_t)(0x04 + i);
    }
}

ZTEST_SUITE(slot_store, NULL, NULL, NULL, NULL, NULL);

/* ---- move: dst-persist-enqueue fails -> roll back, src intact (a2865b3) ---- */
ZTEST(slot_store, move_dst_persist_fail_rolls_back) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 3);
    uint32_t src_gen_before = s->slot_generation[NVS_A];

    g_nvs.fail_next_save = true; /* dst save can't even enqueue */
    dm_result r = slot_store_move(s, NVS_A, NVS_B);

    zassert_equal(r, DM_SAVE_QUEUE_FULL,
                  "dst enqueue failure surfaces as DM_SAVE_QUEUE_FULL (names the failing op)");
    /* src untouched: same events, same generation, no delete attempted */
    zassert_equal(s->slots[NVS_A].event_count, 3, "src kept its events");
    zassert_equal(s->slot_generation[NVS_A], src_gen_before, "src generation unchanged");
    /* dst rolled back to empty */
    zassert_equal(s->slots[NVS_B].event_count, 0, "dst rolled back to empty");
    zassert_equal(g_nvs.del_calls, 0, "no src delete on dst-persist failure");
}

/* ---- move: dst ok, src delete-enqueue fails -> dst safe, error surfaced ---- */
ZTEST(slot_store, move_src_delete_fail_keeps_dst) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 2);

    g_nvs.fail_next_del = true; /* dst saves fine; src delete can't enqueue */
    dm_result r = slot_store_move(s, NVS_A, NVS_B);

    zassert_equal(r, DM_DELETE_QUEUE_FULL,
                  "src delete failure surfaces as DM_DELETE_QUEUE_FULL (names the failing op)");
    zassert_equal(s->slots[NVS_B].event_count, 2, "dst persisted and kept");
    zassert_equal(g_nvs.save_calls, 1, "dst was saved exactly once");
    /* dst is written+persisted BEFORE src is deleted: ordering witness */
    zassert_str_equal(g_nvs.trace, "S1", "only dst save reached the sink (src delete failed)");
}

/* ---- move: happy path -> dst saved THEN src deleted, src zeroed ------------ */
ZTEST(slot_store, move_happy_orders_dst_then_src) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 4);

    dm_result r = slot_store_move(s, NVS_A, NVS_B);

    zassert_equal(r, DM_OK, "clean move returns DM_OK");
    zassert_equal(s->slots[NVS_B].event_count, 4, "dst holds the macro");
    zassert_equal(s->slots[NVS_A].event_count, 0, "src zeroed after delete enqueued");
    zassert_str_equal(g_nvs.trace, "S1D0", "dst saved (S1) strictly before src deleted (D0)");
}

/* ---- move rejections ------------------------------------------------------- */
ZTEST(slot_store, move_empty_src_rejected) {
    slot_store *s = fresh_store();
    dm_result r = slot_store_move(s, NVS_A, NVS_B);
    zassert_equal(r, DM_REJECTED_EMPTY, "moving an empty source is rejected");
}

ZTEST(slot_store, move_occupied_dst_rejected) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 1);
    seed_slot(s, NVS_B, 1);
    dm_result r = slot_store_move(s, NVS_A, NVS_B);
    zassert_equal(r, DM_REJECTED_OCCUPIED, "moving onto an occupied target is rejected");
    zassert_equal(s->slots[NVS_A].event_count, 1, "src untouched on rejection");
}

/* ---- delete-while-playing skips the RAM zero on completion (fe3689e) ------- */
ZTEST(slot_store, delete_while_playing_skips_zero) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 5);

    dm_result r = slot_store_delete(s, NVS_A);
    zassert_equal(r, DM_OK, "delete enqueues for an NVS slot");
    zassert_true(s->pending_delete[NVS_A], "slot marked pending_delete");

    slot_store_mark_playing(s, NVS_A);
    uint32_t gen = s->slot_generation[NVS_A];
    dm_result c = slot_store_complete_delete(s, NVS_A, gen, true);

    zassert_equal(c, DM_OK, "completion reports OK");
    zassert_equal(s->slots[NVS_A].event_count, 5, "playing slot NOT zeroed (fe3689e)");
    zassert_false(s->pending_delete[NVS_A], "pending_delete cleared even when zero skipped");
}

/* ---- delete completion zeroes a non-playing slot --------------------------- */
ZTEST(slot_store, delete_complete_zeroes_idle_slot) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 5);
    slot_store_delete(s, NVS_A);
    uint32_t gen = s->slot_generation[NVS_A];

    dm_result c = slot_store_complete_delete(s, NVS_A, gen, true);
    zassert_equal(c, DM_OK, "completion ok");
    zassert_equal(s->slots[NVS_A].event_count, 0, "non-playing slot zeroed on completion");
}

/* ---- a stale-generation completion is ignored ------------------------------ */
ZTEST(slot_store, stale_completion_ignored) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 5);
    slot_store_delete(s, NVS_A);
    uint32_t stale = s->slot_generation[NVS_A];

    /* slot is reassigned (generation bumped) before the old delete completes */
    s->slot_generation[NVS_A]++;
    seed_slot(s, NVS_A, 2);

    dm_result c = slot_store_complete_delete(s, NVS_A, stale, true);
    zassert_equal(c, DM_OK, "stale completion is a no-op, reports OK");
    zassert_equal(s->slots[NVS_A].event_count, 2, "reassigned slot NOT clobbered by stale delete");
}

/* ---- delete failure surfaces DM_DELETE_FAILED ------------------------------ */
ZTEST(slot_store, delete_completion_failure_surfaces) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 3);
    slot_store_delete(s, NVS_A);
    uint32_t gen = s->slot_generation[NVS_A];

    dm_result c = slot_store_complete_delete(s, NVS_A, gen, false);
    zassert_equal(c, DM_DELETE_FAILED, "failed storage delete surfaces DM_DELETE_FAILED");
}

/* ---- draft buffer --------------------------------------------------------- */
ZTEST(slot_store, draft_append_until_full) {
    slot_store *s = fresh_store();
    slot_store_draft_reset(s);
    struct dm_event e = {.keycode = 0x04};

    for (uint32_t i = 0; i < MAX_EVENTS; i++) {
        zassert_true(slot_store_draft_append(s, &e), "append fits until MAX_EVENTS");
    }
    zassert_equal(slot_store_draft_count(s), (uint32_t)MAX_EVENTS, "draft filled to capacity");
    zassert_false(slot_store_draft_append(s, &e), "append past MAX_EVENTS reports full");
}

/* Commit is RAM-only: the persist is a separate enqueue the machine fires at
 * typing-finished, so the SAVED message types from a settled state before the
 * enqueue can fail. */
ZTEST(slot_store, draft_commit_copies_to_slot_ram_only) {
    slot_store *s = fresh_store();
    slot_store_draft_reset(s);
    struct dm_event e1 = {.keycode = 0x04}, e2 = {.keycode = 0x05};
    slot_store_draft_append(s, &e1);
    slot_store_draft_append(s, &e2);

    dm_result r = slot_store_draft_commit(s, NVS_A);
    zassert_equal(r, DM_OK, "commit into an empty NVS slot succeeds");
    zassert_equal(s->slots[NVS_A].event_count, 2, "draft copied into the slot");
    zassert_equal(s->slots[NVS_A].events[1].keycode, 0x05, "draft bytes landed in order");
    zassert_equal(g_nvs.save_calls, 0, "commit does NOT touch the sink (persist is deferred)");
}

/* ---- persist: the deferred enqueue step ------------------------------------ */
ZTEST(slot_store, persist_enqueues_committed_slot) {
    slot_store *s = fresh_store();
    slot_store_draft_reset(s);
    struct dm_event e = {.keycode = 0x04};
    slot_store_draft_append(s, &e);
    slot_store_draft_commit(s, NVS_A);

    dm_result r = slot_store_persist(s, NVS_A);
    zassert_equal(r, DM_OK, "persist of a committed NVS slot enqueues");
    zassert_equal(g_nvs.save_calls, 1, "exactly one save reached the sink");
    zassert_equal(g_nvs.last_save_slot, NVS_A, "the committed slot was saved");
    zassert_equal(g_nvs.last_save_gen, s->slot_generation[NVS_A],
                  "save is stamped with the slot's current generation");
}

ZTEST(slot_store, persist_queue_full_surfaces) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 1);

    g_nvs.fail_next_save = true;
    dm_result r = slot_store_persist(s, NVS_A);
    zassert_equal(r, DM_SAVE_QUEUE_FULL, "saturated queue surfaces as DM_SAVE_QUEUE_FULL");
}

ZTEST(slot_store, persist_ram_slot_is_noop) {
    slot_store *s = fresh_store();
    seed_slot(s, RAM_A, 1);

    dm_result r = slot_store_persist(s, RAM_A);
    zassert_equal(r, DM_OK, "RAM slot persist is a successful no-op");
    zassert_equal(g_nvs.save_calls, 0, "RAM slots never reach the sink");
}

/* ---- delete enqueue failure rolls the pending bit back ---------------------- */
ZTEST(slot_store, delete_enqueue_full_rolls_back_pending) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 3);

    g_nvs.fail_next_del = true;
    dm_result r = slot_store_delete(s, NVS_A);

    zassert_equal(r, DM_DELETE_QUEUE_FULL, "saturated queue surfaces as DM_DELETE_QUEUE_FULL");
    zassert_false(s->pending_delete[NVS_A], "pending bit rolled back on enqueue failure");
    zassert_equal(s->slots[NVS_A].event_count, 3, "slot contents untouched");
}

ZTEST(slot_store, draft_commit_rejects_occupied) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 1);
    slot_store_draft_reset(s);
    struct dm_event e = {.keycode = 0x04};
    slot_store_draft_append(s, &e);

    dm_result r = slot_store_draft_commit(s, NVS_A);
    zassert_equal(r, DM_REJECTED_OCCUPIED, "commit onto an occupied slot is rejected");
}

ZTEST(slot_store, draft_chain_empty_rejected) {
    slot_store *s = fresh_store();
    slot_store_draft_reset(s);
    dm_result r = slot_store_draft_chain(s, NVS_A); /* NVS_A is empty */
    zassert_equal(r, DM_REJECTED_EMPTY, "chaining an empty slot is rejected");
}

ZTEST(slot_store, draft_chain_no_room_rejected) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, MAX_EVENTS); /* a full slot */
    slot_store_draft_reset(s);
    struct dm_event e = {.keycode = 0x04};
    slot_store_draft_append(s, &e); /* draft now has 1; MAX_EVENTS more won't fit */

    dm_result r = slot_store_draft_chain(s, NVS_A);
    zassert_equal(r, DM_REJECTED_FULL, "chaining a slot that would overflow the draft is rejected");
    zassert_equal(slot_store_draft_count(s), 1, "draft unchanged on no-room rejection");
}

ZTEST(slot_store, draft_chain_appends_slot_events) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 3);
    slot_store_draft_reset(s);
    struct dm_event e = {.keycode = 0x09};
    slot_store_draft_append(s, &e);

    dm_result r = slot_store_draft_chain(s, NVS_A);
    zassert_equal(r, DM_OK, "chaining a non-empty slot that fits succeeds");
    zassert_equal(slot_store_draft_count(s), 4, "draft grew by the chained slot's events");
}

/* ---- restore surface: boot settings_load + DM_TEST_RELOAD ------------------ */

/* Boot load is a raw populate: no persist echo back into the sink, no generation
 * bump, pending bit cleared — exactly what dm_settings_set does today. */
ZTEST(slot_store, load_populates_without_persist_echo) {
    slot_store *s = fresh_store();
    struct dm_event evs[2] = {{.keycode = 0x04}, {.keycode = 0x05}};
    s->pending_delete[NVS_A] = true; /* stale pending state must not survive a load */

    bool ok = slot_store_load(s, NVS_A, evs, 2);

    zassert_true(ok, "valid load accepted");
    zassert_equal(s->slots[NVS_A].event_count, 2, "loaded events landed");
    zassert_equal(s->slots[NVS_A].events[1].keycode, 0x05, "loaded bytes in order");
    zassert_equal(g_nvs.save_calls, 0, "load never echoes back into the sink");
    zassert_equal(s->slot_generation[NVS_A], 0, "load does not bump the generation");
    zassert_false(s->pending_delete[NVS_A], "load clears a stale pending bit");
}

/* Serialization validation (version/length) is dm_nvs's job; the store only
 * defends its own array bound. */
ZTEST(slot_store, load_rejects_overflow) {
    slot_store *s = fresh_store();
    struct dm_event evs[1] = {{.keycode = 0x04}};

    bool ok = slot_store_load(s, NVS_A, evs, MAX_EVENTS + 1);

    zassert_false(ok, "count past MAX_EVENTS is rejected");
    zassert_equal(s->slots[NVS_A].event_count, 0, "slot untouched on rejected load");
}

/* DM_TEST_RELOAD zeroes slots, pending bits, and generations before re-running
 * settings_load. The draft is NOT touched (reload only dispatches from IDLE;
 * parity with dm_storage_test_reload, which never clears recording_buffer). */
ZTEST(slot_store, reset_clears_slots_pending_generations) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 3);
    seed_slot(s, RAM_A, 2);
    s->pending_delete[NVS_B] = true;
    s->slot_generation[NVS_A] = 7;
    struct dm_event e = {.keycode = 0x09};
    slot_store_draft_append(s, &e);

    slot_store_reset(s);

    zassert_equal(s->slots[NVS_A].event_count, 0, "NVS slot zeroed");
    zassert_equal(s->slots[RAM_A].event_count, 0, "RAM slot zeroed");
    zassert_false(s->pending_delete[NVS_B], "pending bits cleared");
    zassert_equal(s->slot_generation[NVS_A], 0, "generations zeroed");
    zassert_equal(slot_store_draft_count(s), 1, "draft survives a reset (reload is IDLE-only)");
}
