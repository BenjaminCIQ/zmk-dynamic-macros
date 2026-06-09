/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * slot_store — the deep storage module (redesign §2.1, rewrite step 3).
 *
 * Owns slots[], pending_delete, slot_generation, and the recording draft buffer.
 * The RAM+NVS dual-write ordering and rollback are INTERNAL; callers never see
 * "dst-before-src" — they see only a dm_result. The machine asks for counts and
 * commits; it never touches slot bytes (§2.7.4).
 *
 * PURE: this header and slot_store.c include no Zephyr and do no I/O. Persistence
 * is reached through an injected dm_nvs_sink vtable (below), so the dual-write
 * ordering + rollback are host-testable against a fake sink (§4). The firmware
 * wires a thin adapter over the file-scoped dm_nvs; the host tests wire a fake
 * that can inject DM_QUEUE_FULL and drive async completion synchronously.
 *
 * Cross-thread contract (§3): pending_delete is atomic bits and slot_generation
 * is generation-stamped so a stale async completion is ignored. In the pure
 * build these are plain words (single-threaded host); the firmware adapter keeps
 * the atomic semantics. The ordering logic under test does not depend on the
 * atomicity — only on the generation check, which is pure arithmetic.
 */

#ifndef SLOT_STORE_H
#define SLOT_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_config.h>
#include <zmk-behavior-dynamic-macros/dm_event.h>
#include <zmk-behavior-dynamic-macros/dm_result.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A recorded macro: a fixed-capacity event array with a live count. Layout is
 * shared with the firmware's struct dm_slot (dm_internal.h) for the fields the
 * store touches; the new stack includes THIS definition, the old path keeps its
 * own, and they are merged at the step-8 cut-over.
 */
struct dm_slot {
    uint32_t event_count;
    struct dm_event events[MAX_EVENTS];
};

/* Slot class for slot_store_count (§2.1). */
typedef enum {
    DM_SLOT_CLASS_NVS = 0, /* persisted slots [0, NVS_SLOTS)        */
    DM_SLOT_CLASS_RAM,     /* RAM-only slots  [NVS_SLOTS, MAX_SLOTS) */
    DM_SLOT_CLASS_ALL,     /* every slot                             */
} slot_class;

/*
 * The NVS sink — slot_store's only downward dependency for persistence.
 *
 * save/del are the ENQUEUE step: they return DM_OK if the async op was accepted,
 * DM_QUEUE_FULL if the storage queue is saturated (the only synchronous failure
 * the dual-write ordering must roll back on). The async OUTCOME of a save/delete
 * (success memset, save_failed, delete_failed) is reported back later via
 * slot_store_complete_delete() / the machine's deliver_async — not here.
 *
 * RAM-only slots never reach the sink; slot_store calls it only for NVS slots.
 */
typedef struct {
    dm_result (*save)(void *ctx, int slot, const struct dm_slot *s, uint32_t generation);
    dm_result (*del)(void *ctx, int slot, uint32_t generation);
    void *ctx;
} dm_nvs_sink;

typedef struct slot_store slot_store;

/*
 * Construct over caller-owned storage. `sink` may be NULL only in a build with no
 * NVS slots (RAM-only); otherwise it persists NVS slots. The store does not own
 * the sink.
 */
void slot_store_init(slot_store *s, const dm_nvs_sink *sink);

/* ---- Queries -------------------------------------------------------------- */

bool                  slot_store_is_empty(const slot_store *s, int idx);
const struct dm_slot *slot_store_get(const slot_store *s, int idx); /* NULL if empty */
int                   slot_store_count(const slot_store *s, slot_class cls);

/* ---- Mutations — each handles its own persistence for NVS slots ----------- */

/* Move src -> dst. Ordering hidden: dst is written+persisted first; only on its
 * success is src zeroed+deleted. dst-enqueue failure rolls dst back, src intact
 * (returns DM_QUEUE_FULL). src delete-enqueue failure leaves dst safe and
 * surfaces DM_QUEUE_FULL. Returns DM_REJECTED_EMPTY if src empty,
 * DM_REJECTED_OCCUPIED if dst occupied. (ports a2865b3) */
dm_result slot_store_move(slot_store *s, int src, int dst);

/* Delete idx. NVS slots are marked pending and enqueued; the RAM zero happens on
 * async completion (slot_store_complete_delete), honoring the playing-slot rule.
 * RAM slots are zeroed immediately. DM_REJECTED_EMPTY if already empty. */
dm_result slot_store_delete(slot_store *s, int idx);

/* ---- Draft buffer (the recording buffer) — §2.7.4 ------------------------- */

void      slot_store_draft_reset(slot_store *s);                     /* REC start */
bool      slot_store_draft_append(slot_store *s, const struct dm_event *e); /* false = full */
uint32_t  slot_store_draft_count(const slot_store *s);              /* guard input */
dm_result slot_store_draft_chain(slot_store *s, int src);          /* chain src into draft */
dm_result slot_store_draft_commit(slot_store *s, int dst);         /* assign: draft -> dst */

/* ---- Playback ownership — lets delete-completion skip a playing slot ------ */

void slot_store_mark_playing(slot_store *s, int idx);
void slot_store_clear_playing(slot_store *s);

/*
 * Async delete completion (called by the nvs sink driver once settings_delete
 * returns). `ok` is the storage verdict. On success the RAM slot is zeroed
 * UNLESS it is the playing slot (ports fe3689e) or the generation is stale (the
 * op was superseded). Returns the outcome to surface (DM_OK / DM_DELETE_FAILED),
 * already filtered for staleness so the caller need not re-check.
 */
dm_result slot_store_complete_delete(slot_store *s, int idx, uint32_t generation, bool ok);

#ifdef __cplusplus
}
#endif

#endif /* SLOT_STORE_H */
