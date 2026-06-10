/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_kconfig — the new stack's Kconfig-derived constants.
 *
 * The new deep-module shells need the firmware's compile-time knobs (locale,
 * feedback defaults, status detail, slot geometry, slot_is_nvs) but must NOT pull
 * in the legacy dm_internal.h, because that header *also* defines names the new
 * module headers own — `enum dm_state` (vs dm_machine.h) and the `DM_LOCALE_*`
 * value macros (vs dm_render.h's `dm_locale` enum) — which collide when both are
 * in one translation unit during the parallel-stack phase. This header carries
 * only the scalar Kconfig values and the one tiny inline, so it composes cleanly
 * with dm_machine.h / dm_render.h.
 *
 * At the step-8 cut-over (legacy path deleted) this can fold back into a single
 * config header.
 */

#ifndef DM_KCONFIG_H
#define DM_KCONFIG_H

#include <stdbool.h>

/* Slot geometry: MAX_EVENTS / NVS_SLOTS / RAM_SLOTS / MAX_SLOTS / SLOT_CAPACITY.
 * dm_config.h sources these from Kconfig under __ZEPHYR__ (host defaults
 * otherwise) — the single source of truth, shared with the pure cores. */
#include <zmk-behavior-dynamic-macros/dm_config.h>

/* Feedback levels (numeric ladder; mirrors the Kconfig choice values). */
#define DM_FEEDBACK_OFF     0
#define DM_FEEDBACK_ERROR   1
#define DM_FEEDBACK_COMMAND 2
#define DM_FEEDBACK_BASIC   3
#define DM_FEEDBACK_VERBOSE 4
#define DM_FEEDBACK_LEVEL   CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_LEVEL

/* Status detail (compile-time, independent of runtime feedback level). */
#define DM_STATUS_OFF          0
#define DM_STATUS_COUNT        1
#define DM_STATUS_USED         2
#define DM_STATUS_USED_PREVIEW 3
#define DM_STATUS_FULL         4
#define DM_STATUS_DETAIL       CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_STATUS_DETAIL

/* Whether any typed output is compiled in at all. */
#define DM_TYPING_ENABLED (DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF || DM_STATUS_DETAIL > DM_STATUS_OFF)

/* Locale as the Kconfig integer (0..3). The dm_locale ENUM and its DM_LOCALE_US/
 * UK/DE/FR names live in dm_render.h; this is only the selected value, cast to
 * dm_locale at the boundary. */
#if DM_TYPING_ENABLED
#define DM_LOCALE CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_LOCALE
#else
#define DM_LOCALE 0
#endif

/* Feedback style default (0 = FULL, 1 = ARROW), matching dm_feedback_pump.h's
 * DM_FB_STYLE_FULL/ARROW. */
#if DM_TYPING_ENABLED
#define DM_FEEDBACK_STYLE CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_STYLE
#else
#define DM_FEEDBACK_STYLE 0
#endif

static inline bool slot_is_nvs(int slot_idx) {
    return IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST) && slot_idx < NVS_SLOTS;
}

#endif /* DM_KCONFIG_H */
