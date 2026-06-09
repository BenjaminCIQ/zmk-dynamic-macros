/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_nvs — the storage backend (single-instance, file-scoped).
 *
 * Owns the async work queue, the storage msgq, the save/load buffer, and the
 * settings handler. File-scoped per ADR-0002: one work queue / msgq / buffer
 * shared, no instance handle threaded — the per-instance slot_store calls in
 * through the injected dm_nvs_sink, whose adapter (dm_nvs_sink_save/del) lands
 * here.
 *
 * The dual-write ORDERING lives in slot_store; dm_nvs only serializes and reports
 * outcomes. A delete/save completion is delivered on the SYSTEM work queue (§3):
 * the storage thread finishes the settings op and submits one completion that
 * runs slot_store_complete_delete + dm_machine_deliver_async back-to-back, so the
 * storage thread never touches slot state.
 *
 * Compiled only under PERSIST.
 */

#ifndef DM_NVS_H
#define DM_NVS_H

#include <stdbool.h>
#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_result.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dm_machine;
struct dm_feedback;

/*
 * Wire the single instance's modules into the file-scoped backend, and start the
 * storage work queue. settings_name keys the per-slot settings entries (matches
 * the old config->settings_name). machine/feedback may be NULL in a build without
 * them; store must be non-NULL. Boot restore (slot_store_load /
 * dm_feedback_restore_*) and export read-back reach these through the saved refs.
 */
void dm_nvs_init(slot_store *store, struct dm_machine *machine, struct dm_feedback *feedback,
                 const char *settings_name);

/*
 * The dm_nvs_sink slot_store calls for NVS persistence. save/del are the enqueue
 * step: DM_OK if accepted, DM_SAVE_QUEUE_FULL / DM_DELETE_QUEUE_FULL if the msgq
 * is saturated (the only synchronous failure the dual-write ordering rolls back
 * on). The firmware passes &dm_nvs_sink_get() to slot_store_init.
 */
const dm_nvs_sink *dm_nvs_sink_get(void);

/* Persist the runtime knobs (level/style/erase). Queue-full is logged, not spoken
 * (matches the old DM_STORAGE_OP_SAVE_FEEDBACK). The dm_feedback save_knobs hook
 * is wired to this. */
void dm_nvs_save_knobs(uint8_t level, uint8_t style, bool erase);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
/* Test-only: drain the work queue, zero RAM via slot_store_reset, re-run
 * settings_load. IDLE-only; dispatched by the shell, never a machine transition. */
void dm_nvs_test_reload(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DM_NVS_H */
