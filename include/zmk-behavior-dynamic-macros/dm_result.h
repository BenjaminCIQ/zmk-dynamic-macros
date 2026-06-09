/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_result — the shared transition-outcome type (redesign §2.0).
 *
 * One enum names every transition outcome, whether returned synchronously from
 * a slot_store/dm_machine call or delivered late through the deferred-feedback
 * path. PURE: no Zephyr, so the pure-core modules (slot_store, dm_machine) and
 * their host tests share it without pulling in the kernel.
 *
 * Deliberate timing-flattening (§2.0): a synchronous return MAY type-hold an
 * async-only value (DM_SAVE_FAILED). Accepted on purpose — one outcome type is
 * worth more than encoding sync-vs-async in the type. Do NOT re-split this.
 */

#ifndef DM_RESULT_H
#define DM_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DM_OK = 0,
    DM_SAVE_QUEUE_FULL,   /* save enqueue refused — storage queue saturated */
    DM_DELETE_QUEUE_FULL, /* delete enqueue refused — storage queue saturated */
    DM_SAVE_FAILED,       /* NVS write failed (async only) */
    DM_DELETE_FAILED,     /* NVS delete failed (async only) */
    DM_REJECTED_OCCUPIED, /* target slot not empty */
    DM_REJECTED_EMPTY,    /* source/target slot empty */
    DM_REJECTED_FULL,     /* recording draft / chain would overflow MAX_EVENTS */
} dm_result;

/*
 * Queue-full is split per op (2026-06-09; replaced a single DM_QUEUE_FULL) for
 * the same reason DM_REJECTED_FULL exists: each code drives a DISTINCT
 * user-facing message ("[DM SAVE QUEUE FULL <dst>]" vs "[DM DEL QUEUE FULL
 * <src>]"). A move can fail either way, so one shared code would force feedback
 * to recover which phase failed — and which slot to name — from out-of-band
 * knowledge, the exact thing this enum exists to prevent.
 */

#ifdef __cplusplus
}
#endif

#endif /* DM_RESULT_H */
