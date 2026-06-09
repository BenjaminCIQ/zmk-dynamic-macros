/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_config — compile-time sizing for the pure core.
 *
 * slot_store and dm_slot are sized by MAX_EVENTS / NVS_SLOTS / RAM_SLOTS. On
 * Zephyr those come from Kconfig (via dm_internal.h, included BEFORE this header
 * in the firmware build, so the guards below are no-ops there). In the pure host
 * unit build there is no Kconfig, so this header supplies test-representative
 * defaults — letting slot_store.c compile and link with nothing but a C compiler.
 * The values mirror the Kconfig defaults so host tests exercise the same array
 * shapes the firmware uses.
 *
 * PURE: no Zephyr include. Do not add one.
 */

#ifndef DM_CONFIG_H
#define DM_CONFIG_H

#ifndef MAX_EVENTS
#define MAX_EVENTS 64
#endif

#ifndef NVS_SLOTS
#define NVS_SLOTS 8
#endif

#ifndef RAM_SLOTS
#define RAM_SLOTS 8
#endif

#define MAX_SLOTS     (NVS_SLOTS + RAM_SLOTS)
#define SLOT_CAPACITY (MAX_SLOTS > 0 ? MAX_SLOTS : 1)

#endif /* DM_CONFIG_H */
