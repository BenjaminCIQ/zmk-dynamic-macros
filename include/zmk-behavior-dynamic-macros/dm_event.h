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

#endif /* DM_EVENT_H */
