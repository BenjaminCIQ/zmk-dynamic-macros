/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_query — the pure read-only projection behind the dm_get_* widget API.
 *
 * Widgets ask by slot index, not by device; the projection resolves the one
 * instance internally (the single-instance assumption lives here, in one place,
 * anchored to BUILD_ASSERT(<=1) at the wiring site). This header is the pure
 * core of that: it turns a slot view + counts into the strings and numbers the
 * widgets read, with no Zephyr and no global state.
 *
 * The Zephyr shell over this (dm_events) holds the resolved slot_store pointer,
 * supplies the slot views, and raises zmk_dynamic_macro_state_changed; that part
 * is verified by the native_sim e2e harness. The string/number projection here
 * is host-tested directly.
 */

#ifndef DM_QUERY_H
#define DM_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_event.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Render slot `view` to a preview string in `buf` (length `len`), via the
 * dm_render buffer sink. Truncates honestly (stop-at-first-non-fit) and always
 * NUL-terminates. Returns the string length written (excluding the NUL).
 *
 * `typing_enabled` mirrors DM_TYPING_ENABLED: when false the preview machinery
 * is compiled out in the firmware, so this returns the "(N events)" fallback
 * using `event_count` instead of a rendered preview — the ported quirk widgets
 * rely on.
 */
int dm_query_preview_string(const dm_render_slot_view *view, dm_locale locale, uint32_t event_count,
                            bool typing_enabled, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DM_QUERY_H */
