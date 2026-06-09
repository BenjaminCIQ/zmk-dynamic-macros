/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * slot_store private layout.
 *
 * Separated from the public slot_store.h so the struct is opaque to ordinary
 * callers (the machine sees only the slot_store* handle), while slot_store.c and
 * the WHITE-BOX store tests can both see the fields. The store tests are
 * deliberately white-box: they assert slots[] / pending_delete / slot_generation
 * outcomes directly, which is the whole point of pinning the dual-write
 * ordering. No other module includes this header.
 *
 * PURE: no Zephyr. pending_delete/playing are plain words here (single-threaded
 * host + the cooperative firmware behavior thread); the cross-thread atomicity
 * the firmware needs lives in the nvs adapter, not in this ordering logic.
 */

#ifndef SLOT_STORE_PRIV_H
#define SLOT_STORE_PRIV_H

#include <zmk-behavior-dynamic-macros/slot_store.h>

struct slot_store {
    struct dm_slot slots[SLOT_CAPACITY];
    bool           pending_delete[SLOT_CAPACITY];
    uint32_t       slot_generation[SLOT_CAPACITY];
    struct dm_slot draft; /* the recording buffer */
    int            playing_slot; /* -1 when nothing is playing */
    const dm_nvs_sink *sink;     /* not owned; NULL in RAM-only builds */
};

#endif /* SLOT_STORE_PRIV_H */
