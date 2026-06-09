/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * slot_store — the deep storage module.
 *
 * Owns slots[], pending_delete, slot_generation, and the recording draft buffer.
 * The RAM+NVS dual-write ordering and rollback are INTERNAL; callers never see
 * "dst-before-src" — they see only a dm_result. The machine asks for counts and
 * commits; it never touches slot bytes.
 *
 * PURE: this header and slot_store.c include no Zephyr and do no I/O. Persistence
 * is reached through an injected dm_nvs_sink vtable (below), so the dual-write
 * ordering + rollback are host-testable against a fake sink. The firmware wires a
 * thin adapter over the file-scoped dm_nvs; the host tests wire a fake that can
 * inject queue-full and drive async completion synchronously.
 *
 * Cross-thread contract: pending_delete is atomic bits and slot_generation
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
 * A recorded macro: a fixed-capacity event array with a live count.
 */
struct dm_slot {
    uint32_t event_count;
    struct dm_event events[MAX_EVENTS];
};

/* Slot class for slot_store_count. */
typedef enum {
    DM_SLOT_CLASS_NVS = 0, /* persisted slots [0, NVS_SLOTS)        */
    DM_SLOT_CLASS_RAM,     /* RAM-only slots  [NVS_SLOTS, MAX_SLOTS) */
    DM_SLOT_CLASS_ALL,     /* every slot                             */
} slot_class;

/*
 * The NVS sink — slot_store's only downward dependency for persistence.
 *
 * save/del are the ENQUEUE step: they return DM_OK if the async op was accepted,
 * or the op's queue-full code if the storage queue is saturated (save returns
 * DM_SAVE_QUEUE_FULL, del returns DM_DELETE_QUEUE_FULL — split so a move's two
 * failure phases stay distinguishable; see dm_result.h). Queue-full is the only
 * synchronous failure the dual-write ordering must roll back on. The async
 * OUTCOME of a save/delete (success memset, save_failed, delete_failed) is
 * reported back later via slot_store_complete_delete() / the machine's
 * deliver_async — not here.
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
 * (returns DM_SAVE_QUEUE_FULL). src delete-enqueue failure leaves dst safe and
 * surfaces DM_DELETE_QUEUE_FULL. Returns DM_REJECTED_EMPTY if src empty,
 * DM_REJECTED_OCCUPIED if dst occupied.
 * NOTE: src == dst never reaches the store — the machine's guard turns a
 * same-slot move into a CANCEL, not a rejection. */
dm_result slot_store_move(slot_store *s, int src, int dst);

/* Delete idx. NVS slots are marked pending and enqueued; the RAM zero happens on
 * async completion (slot_store_complete_delete), honoring the playing-slot rule.
 * RAM slots are zeroed immediately. DM_REJECTED_EMPTY if already empty;
 * DM_DELETE_QUEUE_FULL rolls the pending bit back. */
dm_result slot_store_delete(slot_store *s, int idx);

/* Enqueue the persist of slot idx (NVS slots; a RAM slot is a successful no-op).
 * This is assign's deferred persistence step: draft_commit is RAM-only, and the
 * machine calls persist from dm_machine_typing_finished() — after the SAVED
 * feedback has typed from a settled state. At feedback levels that type
 * nothing, typing-finished fires synchronously, so the persist is immediate.
 * Returns DM_OK | DM_SAVE_QUEUE_FULL. */
dm_result slot_store_persist(slot_store *s, int idx);

/* ---- Draft buffer (the recording buffer) ---------------------------------- */

void      slot_store_draft_reset(slot_store *s);                     /* REC start */
bool      slot_store_draft_append(slot_store *s, const struct dm_event *e); /* false = full */
uint32_t  slot_store_draft_count(const slot_store *s);              /* guard input */
dm_result slot_store_draft_chain(slot_store *s, int src);          /* chain src into draft */
/* Assign: draft -> dst, RAM ONLY (DM_OK | DM_REJECTED_OCCUPIED). Persistence is
 * the separate slot_store_persist() above, deferred to typing-finished. */
dm_result slot_store_draft_commit(slot_store *s, int dst);

/* ---- Restore surface — dm_nvs only (boot settings_load + DM_TEST_RELOAD) --- */

/* Raw populate of slot idx from decoded storage: no sink echo, no generation
 * bump, clears a stale pending bit. Serialization validation (version, length)
 * is dm_nvs's job; the store only defends count <= MAX_EVENTS (false = reject).
 * Never called by the machine. */
bool slot_store_load(slot_store *s, int idx, const struct dm_event *events, uint32_t count);

/* Zero all slots, pending bits, and generations ahead of a settings_load re-run
 * (DM_TEST_RELOAD). The draft and the sink wiring are untouched — reload only
 * dispatches from IDLE. */
void slot_store_reset(slot_store *s);

/* ---- Playback ownership — lets delete-completion skip a playing slot ------ */

void slot_store_mark_playing(slot_store *s, int idx);
void slot_store_clear_playing(slot_store *s);

/*
 * Async delete completion (called by the nvs sink driver once settings_delete
 * returns). `ok` is the storage verdict. On success the RAM slot is zeroed
 * UNLESS it is the playing slot or the generation is stale (the
 * op was superseded). Returns the outcome to surface (DM_OK / DM_DELETE_FAILED),
 * already filtered for staleness so the caller need not re-check.
 */
dm_result slot_store_complete_delete(slot_store *s, int idx, uint32_t generation, bool ok);

#ifdef __cplusplus
}
#endif

#endif /* SLOT_STORE_H */
