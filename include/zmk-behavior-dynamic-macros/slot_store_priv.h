/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * slot_store private layout.
 *
 * Separated from the public slot_store.h so the struct is opaque to the modules
 * that only hold a slot_store* handle (the machine). Two callers need the full
 * layout: slot_store.c itself, the WHITE-BOX store tests (which assert slots[] /
 * pending_delete / slot_generation directly to pin the dual-write ordering), and
 * the behavior shell, which embeds a slot_store by value in dev->data so no heap
 * allocation is needed — the same caller-owned-storage pattern dm_machine.h and
 * dm_feedback_pump_priv.h already expose. Ordinary modules still include only the
 * public slot_store.h.
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
