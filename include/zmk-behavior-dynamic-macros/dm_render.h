/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_render — pure, host-testable macro preview renderer (redesign §2.3).
 *
 * One event-walk over a slot's dm_events, emitting a human-readable preview to
 * an abstract char sink: literal printable keys stay literal, everything else
 * becomes a <TOKEN> (e.g. <LCTL+C>). The replayable-vs-token decision and all
 * token formatting live HERE, used by both sinks, so the live-typing preview
 * and the dm_get_preview_string query API cannot disagree.
 *
 * PURE: this header and its implementation include no Zephyr and do no I/O.
 * That is load-bearing — it is what makes the renderer host-testable and is
 * checked by the standalone unit build (tests/unit). Do not add a Zephyr
 * include here.
 *
 * The sink is char-only by design (no emit_token); see §2.3. A token is emitted
 * as its individual characters via emit_char; dm_render owns the formatting.
 */

#ifndef DM_RENDER_H
#define DM_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include <zmk-behavior-dynamic-macros/dm_event.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host keyboard layout. Locale is link-time-fixed (one layout per firmware),
 * so the renderer selects a static const mapping table by this value. US/UK are
 * full-punctuation locales; DE/FR are plain locales (DM_LOCALE_PLAIN) whose
 * previews emit only letters, digits, and space.
 */
typedef enum {
    DM_LOCALE_US = 0,
    DM_LOCALE_UK = 1,
    DM_LOCALE_DE = 2,
    DM_LOCALE_FR = 3,
} dm_locale;

/*
 * Abstract char sink. Two adapters satisfy it (§2.3):
 *   - ring sink (live typing): space_for returns ring headroom; the walk pauses
 *     when a unit will not fit and resumes after drain.
 *   - buffer sink (dm_get_preview_string): space_for checks remaining buffer.
 *
 * Contract:
 *   - dm_render calls space_for(ctx, n) BEFORE emitting a run of n chars (a
 *     literal char is n=1; a token is its full char length). If space_for
 *     returns false, dm_render emits nothing for that unit and stops — the
 *     caller may re-enter after draining to resume (ring backpressure).
 *   - emit_char(ctx, c) appends one already-resolved preview character.
 */
typedef struct {
    void (*emit_char)(void *ctx, char c);
    bool (*space_for)(void *ctx, uint8_t n);
    void *ctx;
} dm_sink;

/*
 * Slot view the renderer reads. The renderer never owns or mutates a slot; it
 * walks event_count events. Defined here (pure) so the renderer and its host
 * tests need no Zephyr; the behavior's struct dm_slot is layout-compatible with
 * this view for the fields the renderer touches.
 */
typedef struct {
    uint32_t event_count;
    const struct dm_event *events;
} dm_render_slot_view;

/*
 * Resume cursor for the paused walk. The ring sink pauses rendering on
 * backpressure (space_for == false) and re-enters after drain; the cursor is
 * what survives the pause, and it must carry BOTH halves of the walk state:
 * the position AND the modifier state accumulated from events before the pause
 * (a held Ctrl from event i-2 still modifies the token at event i). A plain
 * caller-owned value so the renderer itself stays stateless and re-entrant —
 * the typed equivalent of the old preview_idx/preview_mods fields.
 * Zero-initialize to start a walk from the beginning.
 */
typedef struct {
    uint32_t idx;         /* next event to consider */
    uint8_t  active_mods; /* modifiers held by events before idx */
} dm_render_cursor;

/*
 * Walk `view`'s events for the given `locale`, emitting the preview to `sink`.
 * Pure: no allocation, no I/O, no global state.
 *
 * Returns true when the walk completed. Returns false when the sink refused
 * space for the next unit: nothing was emitted for that unit, and `cursor`
 * holds the resume point — call again with the same cursor after the sink
 * drains. `cursor` may be NULL for a one-shot render from the start (the
 * buffer-sink shape); a NULL-cursor walk that runs out of space just stops.
 *
 * Truncation is stop-at-first-non-fit BY DESIGN (decided 2026-06-09): a
 * truncated preview is always an honest prefix. The old dm_get_preview_string
 * walk instead skipped an oversized token and kept appending later, smaller
 * characters, which could show a sequence with a silently-missing middle
 * token. Observable only when the destination buffer is smaller than the
 * preview; pinned by the truncation tests. Do not "fix" this back.
 */
bool dm_render_slot(const dm_render_slot_view *view, dm_locale locale, dm_sink *sink,
                    dm_render_cursor *cursor);

#ifdef __cplusplus
}
#endif

#endif /* DM_RENDER_H */
