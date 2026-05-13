/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DM_INTERNAL_H
#define DM_INTERNAL_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define MAX_EVENTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
#define NVS_SLOTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_NVS_SLOTS
#else
#define NVS_SLOTS 0
#endif
#define RAM_SLOTS     CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_RAM_SLOTS
#define MAX_SLOTS     (NVS_SLOTS + RAM_SLOTS)
#define SLOT_CAPACITY (MAX_SLOTS > 0 ? MAX_SLOTS : 1)

#define DM_FEEDBACK_OFF     0
#define DM_FEEDBACK_ERROR   1
#define DM_FEEDBACK_BASIC   2
#define DM_FEEDBACK_VERBOSE 3
#define DM_FEEDBACK_LEVEL   CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_LEVEL

enum dm_state {
    DM_STATE_IDLE = 0,
    DM_STATE_RECORDING,
    DM_STATE_PENDING_ASSIGN,
    DM_STATE_DELETE_PENDING,
    DM_STATE_MOVE_PENDING,
    DM_STATE_PLAYING,
    DM_STATE_TYPING_FEEDBACK,
};

struct dm_event {
    uint16_t usage_page;
    uint16_t keycode;
    uint8_t implicit_mods;
    uint8_t explicit_mods;
    uint8_t pressed;
    uint8_t _reserved;
} __packed;

struct dm_slot {
    uint32_t event_count;
    struct dm_event events[MAX_EVENTS];
};

#if DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF
#define FEEDBACK_BUF_LEN 512

struct fb_event {
    uint16_t keycode;
    uint8_t mods;
};
#endif

struct behavior_dynamic_macro_config {
    const char *settings_name;
};

struct behavior_dynamic_macro_data {
    const struct device *dev;
    enum dm_state state;
    struct dm_slot slots[SLOT_CAPACITY];
    ATOMIC_DEFINE(pending_delete, SLOT_CAPACITY);
    uint32_t slot_generation[SLOT_CAPACITY];
    struct dm_slot recording_buffer;
    struct k_work_delayable assign_timeout_work;
    int move_source_slot;
    int playback_slot;
    uint32_t playback_event;
    struct k_timer playback_timer;
    struct k_work playback_work;
    bool suppress_recording;
#if DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF
    struct fb_event feedback_buf[FEEDBACK_BUF_LEN];
    int feedback_len;
    int feedback_pos;
    bool feedback_press_phase;
    enum dm_state feedback_return_state;
    int feedback_post_save_slot;
    bool status_mode;
    int status_next_slot;
    struct k_timer feedback_timer;
    struct k_work feedback_work;
#endif
};

static inline bool slot_is_nvs(int slot_idx) {
    return IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST) && slot_idx < NVS_SLOTS;
}

static inline bool slot_is_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    return data->slots[slot_idx].event_count == 0 || atomic_test_bit(data->pending_delete, slot_idx);
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
void dm_storage_init(void);
void dm_storage_save_slot(struct behavior_dynamic_macro_data *data, int slot_idx);
int dm_storage_delete_slot(struct behavior_dynamic_macro_data *data, int slot_idx);
#endif

void dm_feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx);

#endif /* DM_INTERNAL_H */
