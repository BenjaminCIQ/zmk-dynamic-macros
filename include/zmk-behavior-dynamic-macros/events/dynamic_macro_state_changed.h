/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk-behavior-dynamic-macros/dm_event.h>

enum zmk_dynamic_macro_state {
    ZMK_DYNAMIC_MACRO_STATE_IDLE,
    ZMK_DYNAMIC_MACRO_STATE_RECORDING,
    ZMK_DYNAMIC_MACRO_STATE_PLAYING,
};

enum zmk_dynamic_macro_event_type {
    ZMK_DYNAMIC_MACRO_RECORDING_STARTED,
    ZMK_DYNAMIC_MACRO_RECORDING_STOPPED,
    ZMK_DYNAMIC_MACRO_SAVED,
    ZMK_DYNAMIC_MACRO_DELETED,
    ZMK_DYNAMIC_MACRO_MOVED,
    ZMK_DYNAMIC_MACRO_PLAY_STARTED,
    ZMK_DYNAMIC_MACRO_PLAY_FINISHED,
    ZMK_DYNAMIC_MACRO_PREVIEW_READY,

    ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW,
    ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED,
    ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED,
    ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL,
    ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY,
    ZMK_DYNAMIC_MACRO_ERROR_NO_RECORDING,
};

struct zmk_dynamic_macro_state_changed {
    enum zmk_dynamic_macro_state state;
    enum zmk_dynamic_macro_event_type event;
    int slot;
    bool slot_is_nvs;
};

ZMK_EVENT_DECLARE(zmk_dynamic_macro_state_changed);

int dm_get_preview_string(int slot_idx, char *buf, size_t len);
const struct dm_event *dm_get_slot_events(int slot_idx, uint32_t *count);
bool dm_is_slot_empty(int slot_idx);
int dm_get_used_nvs_slots(void);
int dm_get_used_ram_slots(void);
int dm_get_total_nvs_slots(void);
int dm_get_total_ram_slots(void);
enum zmk_dynamic_macro_state dm_get_state(void);
uint32_t dm_get_recording_event_count(void);
