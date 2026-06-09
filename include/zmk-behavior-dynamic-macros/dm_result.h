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
    DM_QUEUE_FULL,        /* storage queue saturated — retry later */
    DM_SAVE_FAILED,       /* NVS write failed (async only) */
    DM_DELETE_FAILED,     /* NVS delete failed (async only) */
    DM_REJECTED_OCCUPIED, /* target slot not empty */
    DM_REJECTED_EMPTY,    /* source/target slot empty */
    DM_REJECTED_FULL,     /* recording draft / chain would overflow MAX_EVENTS */
} dm_result;

#ifdef __cplusplus
}
#endif

#endif /* DM_RESULT_H */
