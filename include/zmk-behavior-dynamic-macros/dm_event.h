/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DM_EVENT_H
#define DM_EVENT_H

#include <stdint.h>

/*
 * On Zephyr, __packed is provided by <zephyr/toolchain.h>. This header is also
 * compiled OFF-Zephyr by the pure host unit build (tests/unit), where __packed
 * is undefined — without this fallback gcc parses `} __packed;` as a tentative
 * global definition, causing multiple-definition link errors. Define it to the
 * GCC/Clang attribute only when Zephyr hasn't already. Keeps the header pure and
 * host-testable while staying identical for the firmware build.
 */
#ifndef __packed
#if defined(__GNUC__) || defined(__clang__)
#define __packed __attribute__((packed))
#else
/* MSVC host build: dm_event is naturally 8 bytes (2+2+1+1+1+1), so no packing
 * attribute is needed to satisfy the layout; MSVC uses #pragma pack, not an
 * attribute. The BUILD_ASSERT/sizeof check still verifies the layout. */
#define __packed
#endif
#endif

struct dm_event {
    uint16_t usage_page;
    uint16_t keycode;
    uint8_t implicit_mods;
    uint8_t explicit_mods;
    uint8_t pressed;
    uint8_t _reserved;
} __packed;

/*
 * A read-only window onto a stored macro: a live count plus a pointer to its
 * events. Stored events live in slot_store's shared arena, not in any one struct,
 * so callers receive this view by value (events == NULL when the slot is empty).
 *
 * Defined in this lowest shared header so the storage module, the renderer, and
 * their host tests all name ONE type. The renderer spells it `dm_render_slot_view`
 * (a typedef alias in dm_render.h); slot_store returns `struct dm_slot_view`. Same
 * type, two names — never copy fields between them.
 */
struct dm_slot_view {
    uint32_t               event_count;
    const struct dm_event *events;
};

#endif /* DM_EVENT_H */
