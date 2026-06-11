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
 *   - the shared arena: commit rejects a full pool, delete+commit reclaims a
 *     hole by compaction, move reuses a region with no extra space, load rejects
 *     arena overflow, and compaction never runs (so never moves bytes) while a
 *     slot is playing
 *
 * White-box: the store tests read events_arena[]/meta[]/pending_delete/
 * slot_generation via the private layout to assert the dual-write outcome and the
 * arena packing directly.
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
#include <zmk-behavior-dynamic-macros/slot_store_priv.h>

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

static dm_result fake_save(void *ctx, int slot, const struct dm_event *events, uint32_t count,
                           uint32_t generation) {
    struct fake_nvs *f = ctx;
    (void)events;
    (void)count;
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

/* Sum of all live meta counts — the arena's first free offset (white-box helper,
 * mirrors slot_store.c's arena_live). */
static uint16_t arena_live_test(const slot_store *s) {
    uint16_t n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        n = (uint16_t)(n + s->meta[i].count);
    }
    return n;
}

/* Bump-allocate `n` synthetic events for a slot directly in the arena (test
 * helper; bypasses the draft). Appends at the current high-water mark so seeded
 * slots never overlap, regardless of seed order. */
static void seed_slot(slot_store *s, int idx, uint32_t n) {
    uint16_t start = arena_live_test(s);
    for (uint32_t i = 0; i < n && (start + i) < ARENA_EVENTS; i++) {
        s->events_arena[start + i].keycode = (uint16_t)(0x04 + i);
    }
    s->meta[idx].start = start;
    s->meta[idx].count = (uint16_t)n;
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
    zassert_equal(s->meta[NVS_A].count, 3, "src kept its events");
    zassert_equal(s->slot_generation[NVS_A], src_gen_before, "src generation unchanged");
    /* dst rolled back to empty */
    zassert_equal(s->meta[NVS_B].count, 0, "dst rolled back to empty");
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
    zassert_equal(s->meta[NVS_B].count, 2, "dst persisted and kept");
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
    zassert_equal(s->meta[NVS_B].count, 4, "dst holds the macro");
    zassert_equal(s->meta[NVS_A].count, 0, "src zeroed after delete enqueued");
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
    zassert_equal(s->meta[NVS_A].count, 1, "src untouched on rejection");
}

/* ---- delete-while-playing skips the RAM zero on completion (fe3689e) ------- */
ZTEST(slot_store, delete_while_playing_skips_zero) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 5);

    dm_result r = slot_store_delete(s, NVS_A);
    zassert_equal(r, DM_DELETE_DEFERRED, "an NVS delete enqueues and defers to completion");
    zassert_true(s->pending_delete[NVS_A], "slot marked pending_delete");

    slot_store_mark_playing(s, NVS_A);
    uint32_t gen = s->slot_generation[NVS_A];
    dm_result c = slot_store_complete_delete(s, NVS_A, gen, true);

    zassert_equal(c, DM_OK, "completion reports OK");
    zassert_equal(s->meta[NVS_A].count, 5, "playing slot NOT zeroed (fe3689e)");
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
    zassert_equal(s->meta[NVS_A].count, 0, "non-playing slot zeroed on completion");
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
    zassert_equal(c, DM_DELETE_STALE, "stale completion is a no-op, reports DM_DELETE_STALE");
    zassert_equal(s->meta[NVS_A].count, 2, "reassigned slot NOT clobbered by stale delete");
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
    zassert_equal(s->meta[NVS_A].count, 2, "draft copied into the slot");
    zassert_equal(s->events_arena[s->meta[NVS_A].start + 1].keycode, 0x05,
                  "draft bytes landed in order");
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
    zassert_equal(s->meta[NVS_A].count, 3, "slot contents untouched");
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
    zassert_equal(s->meta[NVS_A].count, 2, "loaded events landed");
    zassert_equal(s->events_arena[s->meta[NVS_A].start + 1].keycode, 0x05,
                  "loaded bytes in order");
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
    zassert_equal(s->meta[NVS_A].count, 0, "slot untouched on rejected load");
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

    zassert_equal(s->meta[NVS_A].count, 0, "NVS slot zeroed");
    zassert_equal(s->meta[RAM_A].count, 0, "RAM slot zeroed");
    zassert_false(s->pending_delete[NVS_B], "pending bits cleared");
    zassert_equal(s->slot_generation[NVS_A], 0, "generations zeroed");
    zassert_equal(slot_store_draft_count(s), 1, "draft survives a reset (reload is IDLE-only)");
}

/* ---- shared arena --------------------------------------------------------- */

/* Stage `n` events in the draft so a following commit allocates them. */
static void fill_draft(slot_store *s, uint32_t n) {
    slot_store_draft_reset(s);
    for (uint32_t i = 0; i < n; i++) {
        struct dm_event e = {.keycode = (uint16_t)(0x04 + i)};
        slot_store_draft_append(s, &e);
    }
}

/* Fill the whole arena with MAX_EVENTS-sized slots; returns how many slots used.
 * ARENA_EVENTS is a multiple of MAX_EVENTS in the host config (256 / 64 = 4). */
static int fill_arena(slot_store *s) {
    int used = 0;
    for (int i = 0; arena_live_test(s) + MAX_EVENTS <= ARENA_EVENTS; i++) {
        seed_slot(s, i, MAX_EVENTS);
        used++;
    }
    return used;
}

/* A non-empty commit into an exhausted pool is rejected; the target stays empty. */
ZTEST(slot_store, commit_full_pool_rejected) {
    slot_store *s = fresh_store();
    int filled = fill_arena(s);
    zassert_equal(arena_live_test(s), ARENA_EVENTS, "arena is exactly full");

    fill_draft(s, 1);
    dm_result r = slot_store_draft_commit(s, filled); /* first untouched slot */
    zassert_equal(r, DM_REJECTED_FULL, "commit into a full pool is rejected");
    zassert_equal(s->meta[filled].count, 0, "rejected target stays empty");
}

/* Deleting a slot frees its bytes; the next commit reclaims them by compaction —
 * a commit that ONLY fits because the hole was repacked proves lazy compaction. */
ZTEST(slot_store, delete_then_commit_reclaims_space) {
    slot_store *s = fresh_store();
    int filled = fill_arena(s);
    zassert_true(filled >= 2, "host arena holds at least two full slots");

    /* Free a low RAM-class hole so no NVS async is involved. Use slot 0 (NVS) via
     * the immediate completion path instead: delete + complete. */
    slot_store_delete(s, 0);
    uint32_t gen = s->slot_generation[0];
    slot_store_complete_delete(s, 0, gen, true);
    zassert_equal(s->meta[0].count, 0, "deleted slot freed");

    /* Now MAX_EVENTS of free space exists, but as a hole at offset 0 with live
     * slots above it. A commit must compact to place the new macro. */
    fill_draft(s, MAX_EVENTS);
    dm_result r = slot_store_draft_commit(s, 0);
    zassert_equal(r, DM_OK, "commit reclaims the freed hole via compaction");
    zassert_equal(s->meta[0].count, MAX_EVENTS, "new macro stored");
    zassert_equal(arena_live_test(s), ARENA_EVENTS, "pool full again, no bytes leaked");
}

/* Move is a descriptor reassignment: it needs no free arena space even when the
 * pool is exactly full, because dst aliases src's region until src is freed. */
ZTEST(slot_store, move_reuses_region_no_extra_space) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, MAX_EVENTS);
    /* fill the rest of the pool so there is zero free space */
    int next = NVS_B;
    while (arena_live_test(s) + MAX_EVENTS <= ARENA_EVENTS) {
        seed_slot(s, next++, MAX_EVENTS);
    }
    uint16_t live_before = arena_live_test(s);

    /* free a non-NVS destination to move into (RAM_A is empty here only if the
     * fill didn't reach it; pick a high RAM slot guaranteed empty) */
    int dst = MAX_SLOTS - 1;
    zassert_equal(s->meta[dst].count, 0, "chosen dst is empty");

    dm_result r = slot_store_move(s, NVS_A, dst);
    zassert_equal(r, DM_OK, "move succeeds with no free arena space");
    zassert_equal(s->meta[dst].count, MAX_EVENTS, "dst holds the macro");
    zassert_equal(s->meta[NVS_A].count, 0, "src freed");
    zassert_equal(arena_live_test(s), live_before, "total occupancy unchanged by move");
}

/* Load defends the arena bound: a load that would overflow the pool is rejected
 * and leaves the slot untouched. */
ZTEST(slot_store, load_rejects_arena_overflow) {
    slot_store *s = fresh_store();
    int filled = fill_arena(s); /* pool exactly full */
    (void)filled;

    static struct dm_event evs[1] = {{.keycode = 0x04}};
    bool ok = slot_store_load(s, MAX_SLOTS - 1, evs, 1);
    zassert_false(ok, "load past arena capacity is rejected");
    zassert_equal(s->meta[MAX_SLOTS - 1].count, 0, "slot untouched on arena-overflow load");
}

/* After a hole is repacked, live slots sit contiguously from offset 0 — no
 * permanent gap survives a compaction. */
ZTEST(slot_store, compaction_repacks_after_hole) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 10); /* A at [0,10)  */
    seed_slot(s, NVS_B, 10); /* B at [10,20) */
    seed_slot(s, RAM_A, 10); /* C at [20,30) */

    /* delete B (RAM-immediate path needs NVS async; B is NVS, so complete it) */
    slot_store_delete(s, NVS_B);
    slot_store_complete_delete(s, NVS_B, s->slot_generation[NVS_B], true);

    fill_draft(s, 5);
    dm_result r = slot_store_draft_commit(s, RAM_A + 1);
    zassert_equal(r, DM_OK, "commit after the hole succeeds");

    /* A stayed at 0; C repacked down to follow A; D after C — all contiguous. */
    zassert_equal(s->meta[NVS_A].start, 0, "A anchored at 0");
    zassert_equal(s->meta[RAM_A].start, 10, "C repacked to follow A (hole closed)");
    zassert_equal(s->meta[RAM_A + 1].start, 20, "D placed right after C");
}

/* Seed a slot whose every event keycode == tag, so a slot's identity survives a
 * relocation and a cross-slot mixup is detectable. */
static void seed_tagged(slot_store *s, int idx, uint32_t n, uint16_t tag) {
    uint16_t start = arena_live_test(s);
    for (uint32_t i = 0; i < n && (start + i) < ARENA_EVENTS; i++) {
        s->events_arena[start + i].keycode = tag;
    }
    s->meta[idx].start = start;
    s->meta[idx].count = (uint16_t)n;
}

/* Regression (load_e2e parity failure): after a move, a higher-indexed slot can
 * sit at a LOWER arena offset than a lower-indexed one, so index order != memory
 * order. A repack that packed in index order issued a forward memmove that
 * clobbered the not-yet-moved region, swapping two slots' contents. arena_repack
 * must pack in ascending start-offset order. */
ZTEST(slot_store, repack_after_move_preserves_slot_identity) {
    slot_store *s = fresh_store();
    seed_tagged(s, NVS_A, 10, 0xAA); /* [0,10)  index 0 */
    seed_tagged(s, NVS_B, 10, 0xBB); /* [10,20) index 1 */
    seed_tagged(s, RAM_A, 10, 0xCC); /* [20,30) index 8 */

    /* move B (index 1) -> a HIGH index; dst now aliases [10,20) but at index 9,
     * so memory order (0:idx0, 10:idx9, 20:idx8) diverges from index order. */
    int moved = RAM_A + 1; /* index 9 */
    dm_result mr = slot_store_move(s, NVS_B, moved);
    zassert_equal(mr, DM_OK, "move B -> high slot");
    zassert_equal(s->meta[moved].start, 10, "moved slot still at offset 10");
    zassert_equal(s->meta[RAM_A].start, 20, "C above moved slot: memory order != index order");

    /* a commit triggers arena_repack while index order != memory order */
    fill_draft(s, 5);
    dm_result r = slot_store_draft_commit(s, NVS_B); /* slot 1 is now empty */
    zassert_equal(r, DM_OK, "commit after move compacts");

    /* every slot must still read its OWN tag — no cross-slot clobber */
    struct dm_slot_view va = slot_store_get(s, NVS_A);
    struct dm_slot_view vc = slot_store_get(s, RAM_A);
    struct dm_slot_view vm = slot_store_get(s, moved);
    zassert_equal(va.events[0].keycode, 0xAA, "slot A kept its data");
    zassert_equal(vc.events[0].keycode, 0xCC, "slot C kept its data (not clobbered)");
    zassert_equal(vm.events[0].keycode, 0xBB, "moved slot kept B's data (not C's)");
    zassert_equal(va.event_count, 10, "A count intact");
    zassert_equal(vc.event_count, 10, "C count intact");
    zassert_equal(vm.event_count, 10, "moved count intact");
}

/* R1: while a slot plays, an allocating commit that needs compaction is REJECTED
 * and moves no bytes — the guard, not the arithmetic, blocks it. Clearing playback
 * then lets the same commit succeed. */
ZTEST(slot_store, commit_while_playing_rejected_no_move) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 10); /* [0,10)  */
    seed_slot(s, NVS_B, 10); /* [10,20) */
    /* fill the rest so only a compactable hole could satisfy a new commit */
    int next = RAM_A;
    while (arena_live_test(s) + MAX_EVENTS <= ARENA_EVENTS) {
        seed_slot(s, next++, MAX_EVENTS);
    }
    /* free A, leaving a 10-event hole at offset 0 below live slots */
    slot_store_delete(s, NVS_A);
    slot_store_complete_delete(s, NVS_A, s->slot_generation[NVS_A], true);
    uint16_t b_start_before = s->meta[NVS_B].start;

    slot_store_mark_playing(s, NVS_B);
    fill_draft(s, 10);
    dm_result r = slot_store_draft_commit(s, NVS_A);
    zassert_equal(r, DM_REJECTED_FULL, "commit needing compaction is rejected while playing");
    zassert_equal(s->meta[NVS_B].start, b_start_before, "no bytes moved: B's offset unchanged");

    slot_store_clear_playing(s);
    r = slot_store_draft_commit(s, NVS_A);
    zassert_equal(r, DM_OK, "same commit succeeds once playback clears (guard, not math)");
}

/* R3: a pending-delete slot that is also playing keeps its bytes pinned — nothing
 * relocates them, because the only mover (compaction) is blocked while playing. */
ZTEST(slot_store, play_pending_delete_bytes_pinned) {
    slot_store *s = fresh_store();
    seed_slot(s, NVS_A, 10); /* the playing + pending-delete slot */
    seed_slot(s, NVS_B, 10);
    int next = RAM_A;
    while (arena_live_test(s) + MAX_EVENTS <= ARENA_EVENTS) {
        seed_slot(s, next++, MAX_EVENTS);
    }
    uint16_t a_start = s->meta[NVS_A].start;
    uint16_t a_count = s->meta[NVS_A].count;

    slot_store_mark_playing(s, NVS_A);
    slot_store_delete(s, NVS_A); /* NVS: marks pending, bytes stay parked */
    zassert_true(s->pending_delete[NVS_A], "slot pending delete");

    /* an allocating commit into an empty high slot would need compaction to fit —
     * blocked while playing */
    int dst = MAX_SLOTS - 1;
    zassert_equal(s->meta[dst].count, 0, "chosen dst is empty");
    fill_draft(s, 4);
    dm_result r = slot_store_draft_commit(s, dst);
    zassert_equal(r, DM_REJECTED_FULL, "compaction blocked while playing");
    zassert_equal(s->meta[NVS_A].start, a_start, "playing+pending slot's bytes not relocated");
    zassert_equal(s->meta[NVS_A].count, a_count, "playing+pending slot's count intact");
}
