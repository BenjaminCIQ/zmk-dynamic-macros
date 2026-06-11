/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * slot_store — deep storage module.
 *
 * Owns the shared event arena + per-slot meta, pending_delete, slot_generation,
 * and the recording draft. Stored slots share one events_arena pool (sized for the
 * average, not the per-slot worst case); a slot's events live contiguously at
 * meta[i].start. Free space is reclaimed by lazy compaction (arena_repack) inside
 * the allocators only, and never while a slot is playing. The RAM+NVS dual-write
 * ordering and rollback live HERE and surface only as a dm_result; no caller sees
 * "dst-before-src".
 *
 * PURE: no Zephyr, no I/O. Persistence is reached through the injected
 * dm_nvs_sink; the generation-stamp staleness check is plain arithmetic.
 */

#include <assert.h>
#include <string.h>

#include <zmk-behavior-dynamic-macros/slot_store.h>
#include <zmk-behavior-dynamic-macros/slot_store_priv.h>

static bool slot_is_nvs(int idx) {
    return idx >= 0 && idx < NVS_SLOTS;
}

static bool idx_valid(int idx) {
    return idx >= 0 && idx < MAX_SLOTS;
}

/* Raw RAM-occupancy: a slot is empty if it has no events OR is pending delete
 * (the NVS copy is on its way out and the RAM copy is stale). */
static bool slot_empty(const slot_store *s, int idx) {
    return s->meta[idx].count == 0 || s->pending_delete[idx];
}

/* Return a slot's events to the shared pool. The bytes are not wiped — they are
 * simply no longer reachable, and the next arena_repack reclaims the hole. */
static void free_slot(slot_store *s, int idx) {
    s->meta[idx].count = 0;
}

/* ---- shared arena --------------------------------------------------------- */

/* Sum of live (count>0) events across all slots, INCLUDING pending-delete slots
 * (their bytes stay parked in the arena until completion frees them). This is the
 * arena's true occupancy, the capacity all allocation checks measure against. */
static uint16_t arena_live(const slot_store *s) {
    uint16_t n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        n += s->meta[i].count;
    }
    return n;
}

/*
 * Repack all live slots to the low end of the arena, writing the first free
 * offset (== arena_live) to *out_free. Returns false WITHOUT moving a byte if a
 * slot is currently playing: relocating events would dangle the raw pointer the
 * playback handler holds into the arena. This is slot_store's own enforcement of
 * the playing-slot rule (fe3689e) ported to byte movement -- the safety is a
 * runtime guard here, not an assumption about which caller invoked.
 *
 * Slots are packed in ASCENDING start-offset order, NOT index order. After a
 * move (meta[dst]=meta[src]) a higher-indexed slot can sit at a lower arena
 * offset than a lower-indexed one, so index order != memory order. Packing left
 * in source-offset order guarantees each memmove's destination (w) never lands on
 * a region not yet relocated, so a forward copy can't clobber a pending slot.
 * memmove (not memcpy): a left-shifted slot may overlap its old location.
 */
static bool arena_repack(slot_store *s, uint16_t *out_free) {
    if (s->playing_slot != -1) {
        return false; /* compaction is unsafe while a slot plays */
    }

    /* order live slot indices by current start (insertion sort; MAX_SLOTS<=64) */
    int order[MAX_SLOTS];
    int n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (s->meta[i].count == 0) {
            continue;
        }
        int j = n++;
        while (j > 0 && s->meta[order[j - 1]].start > s->meta[i].start) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    uint16_t w = 0;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        if (s->meta[i].start != w) {
            memmove(&s->events_arena[w], &s->events_arena[s->meta[i].start],
                    (size_t)s->meta[i].count * sizeof(struct dm_event));
            s->meta[i].start = w;
        }
        w = (uint16_t)(w + s->meta[i].count);
    }
    *out_free = w;
    return true;
}

void slot_store_init(slot_store *s, const dm_nvs_sink *sink) {
    memset(s, 0, sizeof(*s));
    s->playing_slot = -1;
    s->sink = sink;
}

/* ---- queries -------------------------------------------------------------- */

bool slot_store_is_empty(const slot_store *s, int idx) {
    if (!idx_valid(idx)) {
        return true;
    }
    return slot_empty(s, idx);
}

struct dm_slot_view slot_store_get(const slot_store *s, int idx) {
    if (!idx_valid(idx) || slot_empty(s, idx)) {
        return (struct dm_slot_view){.event_count = 0, .events = NULL};
    }
    return (struct dm_slot_view){
        .event_count = s->meta[idx].count,
        .events = &s->events_arena[s->meta[idx].start],
    };
}

int slot_store_count(const slot_store *s, slot_class cls) {
    int lo = (cls == DM_SLOT_CLASS_RAM) ? NVS_SLOTS : 0;
    int hi = (cls == DM_SLOT_CLASS_NVS) ? NVS_SLOTS : MAX_SLOTS;
    int n = 0;
    for (int i = lo; i < hi; i++) {
        if (!slot_empty(s, i)) {
            n++;
        }
    }
    return n;
}

/* ---- persistence helpers (NVS slots only) --------------------------------- */

static dm_result nvs_save(slot_store *s, int idx) {
    if (!slot_is_nvs(idx) || s->sink == NULL) {
        return DM_OK; /* RAM slot: nothing to persist */
    }
    return s->sink->save(s->sink->ctx, idx, &s->events_arena[s->meta[idx].start],
                         s->meta[idx].count, s->slot_generation[idx]);
}

static dm_result nvs_delete(slot_store *s, int idx) {
    if (!slot_is_nvs(idx) || s->sink == NULL) {
        return DM_OK;
    }
    return s->sink->del(s->sink->ctx, idx, s->slot_generation[idx]);
}

/* ---- move ------------------------------------------------------------------ */

dm_result slot_store_move(slot_store *s, int src, int dst) {
    if (!idx_valid(src) || !idx_valid(dst)) {
        return DM_REJECTED_EMPTY;
    }
    if (slot_empty(s, src)) {
        return DM_REJECTED_EMPTY;
    }
    if (!slot_empty(s, dst)) {
        return DM_REJECTED_OCCUPIED;
    }

    /*
     * Two independent NVS ops (write dst, delete src) with no atomicity. Order
     * them so the only reachable failure is a benign transient duplicate, never
     * data loss:
     *   1. Commit dst in RAM and persist it. If the save can't even be enqueued,
     *      roll dst back and leave src fully intact -- nothing lost.
     *   2. Only once dst's save is queued, zero src in RAM and delete it from
     *      storage. If THAT delete can't be enqueued, dst is already safe; src
     *      may resurrect on reboot, so surface the failure.
     */
    /* Descriptor reassignment, no event copy: dst's meta aliases src's arena
     * region. The arena makes move free in space and bytes. The aliased window
     * (here until free_slot(src) below) must contain NO allocator call -- an
     * arena_repack inside it would double-pack the shared region. It contains
     * none; the assert at the end catches a future edit that breaks that. */
    s->pending_delete[dst] = false;
    s->slot_generation[dst]++;
    s->meta[dst] = s->meta[src];

    dm_result rc = nvs_save(s, dst);
    if (rc != DM_OK) {
        /* roll back dst; src untouched. The sink's code (DM_SAVE_QUEUE_FULL)
         * names the failing op so feedback can name the right slot. */
        s->slot_generation[dst]++;
        free_slot(s, dst);
        return rc;
    }

    s->pending_delete[src] = false;
    s->slot_generation[src]++;
    free_slot(s, src);

    /* Aliasing window closed: src freed, exactly dst now owns the region, so total
     * occupancy is unchanged and within the pool. Trips if a future allocator was
     * slipped into the window above (it would have double-counted the region). */
    assert(arena_live(s) <= ARENA_EVENTS);

    rc = nvs_delete(s, src);
    if (rc != DM_OK) {
        /* dst is safe; src's NVS copy lingers and may reappear on reboot */
        return rc;
    }

    return DM_OK;
}

dm_result slot_store_persist(slot_store *s, int idx) {
    if (!idx_valid(idx)) {
        return DM_REJECTED_EMPTY;
    }
    return nvs_save(s, idx);
}

/* ---- delete --------------------------------------------------------------- */

dm_result slot_store_delete(slot_store *s, int idx) {
    if (!idx_valid(idx) || slot_empty(s, idx)) {
        return DM_REJECTED_EMPTY;
    }

    if (slot_is_nvs(idx) && s->sink != NULL) {
        s->pending_delete[idx] = true;
        dm_result rc = nvs_delete(s, idx);
        if (rc != DM_OK) {
            s->pending_delete[idx] = false;
            return rc;
        }
        /* enqueued: the RAM zero, the DELETED notification, and the spoken
         * confirmation all wait for slot_store_complete_delete. The deferral is
         * explicit in the return so the machine stays silent here. */
        return DM_DELETE_DEFERRED;
    }

    /* RAM slot: free immediately (bytes return to the pool, reclaimed on the next
     * allocating compaction). */
    free_slot(s, idx);
    return DM_OK;
}

dm_result slot_store_complete_delete(slot_store *s, int idx, uint32_t generation, bool ok) {
    if (!idx_valid(idx)) {
        return DM_DELETE_STALE;
    }
    /* Ignore a completion the slot has moved past (reassigned/redeleted). The
     * STALE outcome is distinct from DM_OK so the machine does not speak a
     * spurious DELETED for an op the slot already moved past. */
    if (!s->pending_delete[idx] || s->slot_generation[idx] != generation) {
        return DM_DELETE_STALE;
    }

    if (!ok) {
        s->pending_delete[idx] = false;
        return DM_DELETE_FAILED;
    }

    /* Don't zero a slot that is currently being played back: the NVS copy is
     * already gone, the RAM copy lingers harmlessly until the next op
     * overwrites it. */
    if (s->playing_slot != idx) {
        free_slot(s, idx);
    }
    s->pending_delete[idx] = false;
    return DM_OK;
}

/* ---- draft buffer --------------------------------------------------------- */

void slot_store_draft_reset(slot_store *s) {
    s->draft.event_count = 0;
}

bool slot_store_draft_append(slot_store *s, const struct dm_event *e) {
    if (s->draft.event_count >= MAX_EVENTS) {
        return false;
    }
    s->draft.events[s->draft.event_count++] = *e;
    return true;
}

uint32_t slot_store_draft_count(const slot_store *s) {
    return s->draft.event_count;
}

dm_result slot_store_draft_chain(slot_store *s, int src) {
    if (!idx_valid(src) || slot_empty(s, src)) {
        return DM_REJECTED_EMPTY;
    }
    uint32_t remaining = MAX_EVENTS - s->draft.event_count;
    uint16_t src_count = s->meta[src].count;
    if (src_count > remaining) {
        return DM_REJECTED_FULL;
    }
    memcpy(&s->draft.events[s->draft.event_count], &s->events_arena[s->meta[src].start],
           (size_t)src_count * sizeof(struct dm_event));
    s->draft.event_count += src_count;
    return DM_OK;
}

dm_result slot_store_draft_commit(slot_store *s, int dst) {
    if (!idx_valid(dst)) {
        return DM_REJECTED_OCCUPIED;
    }
    if (!slot_empty(s, dst)) {
        return DM_REJECTED_OCCUPIED;
    }

    uint32_t n = s->draft.event_count; /* n <= MAX_EVENTS by construction */
    uint16_t used;
    if (!arena_repack(s, &used)) {
        return DM_REJECTED_FULL; /* a slot is playing: cannot compact safely */
    }
    if (n > (uint32_t)(ARENA_EVENTS - used)) {
        return DM_REJECTED_FULL; /* draft does not fit the free pool */
    }

    if (n > 0) {
        memcpy(&s->events_arena[used], s->draft.events, (size_t)n * sizeof(struct dm_event));
    }
    s->pending_delete[dst] = false;
    s->slot_generation[dst]++;
    s->meta[dst] = (struct dm_slot_meta){.start = used, .count = (uint16_t)n};
    /* RAM only — the persist is slot_store_persist(), fired by the machine at
     * typing-finished. */
    return DM_OK;
}

/* ---- restore surface (dm_nvs boot load + DM_TEST_RELOAD) -------------------- */

bool slot_store_load(slot_store *s, int idx, const struct dm_event *events, uint32_t count) {
    if (!idx_valid(idx) || count > MAX_EVENTS) {
        return false;
    }
    free_slot(s, idx); /* drop any prior occupant before measuring free space */
    uint16_t used;
    if (!arena_repack(s, &used)) {
        return false; /* boot/reload is not playing; guard anyway */
    }
    if (count > (uint32_t)(ARENA_EVENTS - used)) {
        return false; /* arena overflow */
    }
    if (count > 0) {
        memcpy(&s->events_arena[used], events, (size_t)count * sizeof(struct dm_event));
    }
    s->meta[idx] = (struct dm_slot_meta){.start = used, .count = (uint16_t)count};
    s->pending_delete[idx] = false;
    return true;
}

void slot_store_reset(slot_store *s) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        free_slot(s, i);
        s->pending_delete[i] = false;
        s->slot_generation[i] = 0;
    }
}

/* ---- playback ownership --------------------------------------------------- */

void slot_store_mark_playing(slot_store *s, int idx) {
    s->playing_slot = idx;
}

void slot_store_clear_playing(slot_store *s) {
    s->playing_slot = -1;
}
