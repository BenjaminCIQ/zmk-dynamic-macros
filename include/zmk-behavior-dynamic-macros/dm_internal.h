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
#define DM_FEEDBACK_COMMAND 2
#define DM_FEEDBACK_BASIC   3
#define DM_FEEDBACK_VERBOSE 4
#define DM_FEEDBACK_LEVEL   CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_LEVEL

#define DM_STATUS_OFF          0
#define DM_STATUS_COUNT        1
#define DM_STATUS_USED         2
#define DM_STATUS_USED_PREVIEW 3
#define DM_STATUS_FULL         4
#define DM_STATUS_DETAIL       CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_STATUS_DETAIL

#define DM_TYPING_ENABLED (DM_FEEDBACK_LEVEL > DM_FEEDBACK_OFF || DM_STATUS_DETAIL > DM_STATUS_OFF)

#define DM_LOCALE_US 0
#define DM_LOCALE_UK 1
#define DM_LOCALE_DE 2
#define DM_LOCALE_FR 3
#if DM_TYPING_ENABLED
#define DM_LOCALE CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_LOCALE
#else
#define DM_LOCALE DM_LOCALE_US
#endif
#define DM_LOCALE_PLAIN (DM_LOCALE != DM_LOCALE_US)

#define DM_STYLE_FULL  0
#define DM_STYLE_ARROW 1
#if DM_TYPING_ENABLED
#define DM_FEEDBACK_STYLE CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_STYLE
#else
#define DM_FEEDBACK_STYLE DM_STYLE_FULL
#endif
#define DM_STYLE_IS_ARROW (DM_FEEDBACK_STYLE == DM_STYLE_ARROW)

enum dm_state {
    DM_STATE_IDLE = 0,
    DM_STATE_RECORDING,
    DM_STATE_PENDING_ASSIGN,
    DM_STATE_DELETE_PENDING,
    DM_STATE_MOVE_PENDING,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
    DM_STATE_PREVIEW_PENDING,
#endif
    DM_STATE_PLAYING,
    DM_STATE_TYPING_FEEDBACK,
};

#include <zmk-behavior-dynamic-macros/dm_event.h>

struct dm_slot {
    uint32_t event_count;
    struct dm_event events[MAX_EVENTS];
};

#if DM_TYPING_ENABLED
#define FB_RING_SIZE 64

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
    struct k_timer emit_timer;
    struct k_work emit_work;
    bool suppress_recording;
#if DM_TYPING_ENABLED
    uint8_t current_feedback_level;
    struct fb_event ring[FB_RING_SIZE];
    uint8_t ring_head;
    uint8_t ring_tail;
    bool feedback_press_phase;
    enum dm_state feedback_return_state;
    int feedback_post_save_slot;
    bool status_mode;
    int status_next_slot;
    int status_current_slot;
    const struct dm_slot *preview_slot;
    uint32_t preview_idx;
    uint8_t preview_mods;
    bool preview_done;
    bool needs_suffix;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE)
    uint16_t erase_char_count;
    struct k_work_delayable erase_work;
    bool erase_pending;
#endif
#endif
};

static inline bool slot_is_nvs(int slot_idx) {
    return IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST) && slot_idx < NVS_SLOTS;
}

static inline bool slot_is_empty(struct behavior_dynamic_macro_data *data, int slot_idx) {
    return data->slots[slot_idx].event_count == 0 || atomic_test_bit(data->pending_delete, slot_idx);
}

static inline char slot_storage_prefix(int slot_idx) {
    return slot_is_nvs(slot_idx) ? 'N' : 'R';
}

extern const struct device *dm_devices[];
extern const size_t dm_devices_len;

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)
void dm_storage_init(void);
void dm_storage_save_slot(struct behavior_dynamic_macro_data *data, int slot_idx);
int dm_storage_delete_slot(struct behavior_dynamic_macro_data *data, int slot_idx);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
void dm_storage_flush(void);
void dm_storage_test_reload(void);
#endif
#if DM_TYPING_ENABLED
void dm_storage_save_feedback_level(struct behavior_dynamic_macro_data *data);
#endif
#else
#if DM_TYPING_ENABLED
static inline void dm_storage_save_feedback_level(struct behavior_dynamic_macro_data *data) {}
#endif
#endif

void dm_save_slot(struct behavior_dynamic_macro_data *data, int slot_idx);
int dm_delete_slot_from_storage(struct behavior_dynamic_macro_data *data, int slot_idx);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
void dm_raise_state_changed(struct behavior_dynamic_macro_data *data,
                            int event, int slot);
#endif

#endif /* DM_INTERNAL_H */
