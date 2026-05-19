/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <zmk-dynamic-macros/dm_internal.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define DM_STORAGE_VERSION 0xD1

struct dm_slot_header {
    uint8_t version;
    uint8_t _reserved[3];
    uint32_t event_count;
} __packed;

BUILD_ASSERT(sizeof(struct dm_slot_header) == 8, "dm_slot_header must be 8 bytes packed");

enum dm_storage_op_type {
    DM_STORAGE_OP_SAVE,
    DM_STORAGE_OP_DELETE,
};

struct dm_storage_op {
    enum dm_storage_op_type type;
    struct behavior_dynamic_macro_data *data;
    int slot_idx;
    uint32_t generation;
    struct dm_slot slot;
};

#define DM_STORAGE_STACK_SIZE  1024
#define DM_STORAGE_QUEUE_LEN   8
#define DM_STORAGE_PRIORITY    10

K_KERNEL_STACK_DEFINE(dm_storage_work_q_stack, DM_STORAGE_STACK_SIZE);
K_MSGQ_DEFINE(dm_storage_msgq, sizeof(struct dm_storage_op), DM_STORAGE_QUEUE_LEN, 4);

static struct k_work_q dm_storage_work_q;
static struct k_work dm_storage_work;
static bool dm_storage_work_q_started;


static void settings_slot_key(struct behavior_dynamic_macro_data *data, int slot_idx, char *key,
                              size_t key_len) {
    const struct behavior_dynamic_macro_config *config = data->dev->config;

    snprintf(key, key_len, "dm/%s/slot/%d", config->settings_name, slot_idx);
}

/*
 * Threading model:
 *
 * All behavior handlers and the event listener run on the system work queue
 * (cooperative, single-threaded). This storage work handler runs on
 * dm_storage_work_q at priority 10.
 *
 * Thread safety:
 * - pending_delete uses atomic bit operations (safe across threads)
 * - slot_generation is only written by main thread, read here for staleness check
 * - slots[] writes: main thread does memcpy on assign; this handler does memset
 *   on delete completion. These don't race because pending_delete is set during
 *   delete (main thread treats slot as empty and won't access it), and the
 *   atomic_clear_bit happens after memset completes.
 * - Save operations read slot data via op.slot copy in message queue, not
 *   directly from data->slots[]
 */
static void dm_storage_work_handler(struct k_work *work) {
    static struct dm_storage_op op;
    static uint8_t save_buf[sizeof(struct dm_slot_header) + MAX_EVENTS * sizeof(struct dm_event)];

    while (k_msgq_get(&dm_storage_msgq, &op, K_NO_WAIT) == 0) {
        char key[64];
        settings_slot_key(op.data, op.slot_idx, key, sizeof(key));

        if (op.type == DM_STORAGE_OP_SAVE) {
            struct dm_slot_header *header = (struct dm_slot_header *)save_buf;
            header->version = DM_STORAGE_VERSION;
            header->_reserved[0] = 0;
            header->_reserved[1] = 0;
            header->_reserved[2] = 0;
            header->event_count = op.slot.event_count;

            size_t events_size = op.slot.event_count * sizeof(struct dm_event);
            memcpy(save_buf + sizeof(struct dm_slot_header), op.slot.events, events_size);

            size_t data_size = sizeof(struct dm_slot_header) + events_size;
            int rc = settings_save_one(key, save_buf, data_size);
            if (rc) {
                LOG_ERR("Failed to save dynamic macro slot %d: %d", op.slot_idx, rc);
                dm_feedback_save_failed(op.data, op.slot_idx);
            } else {
                LOG_DBG("Saved dynamic macro slot %d (%u events)", op.slot_idx,
                        (unsigned int)op.slot.event_count);
            }
            continue;
        }

        int rc = settings_delete(key);
        if (rc) {
            LOG_ERR("Failed to delete dynamic macro slot %d from storage: %d", op.slot_idx, rc);
            if (atomic_test_bit(op.data->pending_delete, op.slot_idx) &&
                op.data->slot_generation[op.slot_idx] == op.generation) {
                atomic_clear_bit(op.data->pending_delete, op.slot_idx);
            }
            if (op.data->state == DM_STATE_IDLE) {
                dm_feedback_delete_failed(op.data, op.slot_idx);
            }
            continue;
        }

        if (atomic_test_bit(op.data->pending_delete, op.slot_idx) &&
            op.data->slot_generation[op.slot_idx] == op.generation) {
            memset(&op.data->slots[op.slot_idx], 0, sizeof(struct dm_slot));
            atomic_clear_bit(op.data->pending_delete, op.slot_idx);
            if (op.data->state == DM_STATE_IDLE) {
                dm_feedback_deleted(op.data, op.slot_idx);
            }
        }

        LOG_DBG("Deleted dynamic macro slot %d from storage", op.slot_idx);
    }
}

void dm_storage_init(void) {
    if (dm_storage_work_q_started) {
        return;
    }

    k_work_init(&dm_storage_work, dm_storage_work_handler);
    k_work_queue_start(&dm_storage_work_q, dm_storage_work_q_stack,
                       K_KERNEL_STACK_SIZEOF(dm_storage_work_q_stack),
                       DM_STORAGE_PRIORITY, NULL);
    dm_storage_work_q_started = true;
}

static int enqueue_storage_op(struct behavior_dynamic_macro_data *data, enum dm_storage_op_type type,
                              int slot_idx, const struct dm_slot *slot) {
    struct dm_storage_op op = {
        .type = type,
        .data = data,
        .slot_idx = slot_idx,
        .generation = data->slot_generation[slot_idx],
    };

    if (slot != NULL) {
        memcpy(&op.slot, slot, sizeof(op.slot));
    }

    int rc = k_msgq_put(&dm_storage_msgq, &op, K_NO_WAIT);
    if (rc) {
        LOG_ERR("Dynamic macro storage queue full for slot %d: %d", slot_idx, rc);
        if (type == DM_STORAGE_OP_SAVE) {
            dm_feedback_save_queue_full(data, slot_idx);
        } else {
            dm_feedback_delete_queue_full(data, slot_idx);
        }
        return rc;
    }

    k_work_submit_to_queue(&dm_storage_work_q, &dm_storage_work);
    return 0;
}

static bool parse_settings_slot_name(const char *name, struct behavior_dynamic_macro_data **data,
                                     int *slot_idx) {
    const char *p = name;
    int parsed = 0;

    for (size_t i = 0; i < dm_devices_len; i++) {
        const struct device *dev = dm_devices[i];
        const struct behavior_dynamic_macro_config *config = dev->config;
        size_t settings_name_len = strlen(config->settings_name);

        if (strncmp(name, config->settings_name, settings_name_len) != 0 ||
            name[settings_name_len] != '/') {
            continue;
        }

        *data = dev->data;
        p = name + settings_name_len + 1;
        break;
    }

    if (*data == NULL || strncmp(p, "slot/", strlen("slot/")) != 0) {
        return false;
    }
    p += strlen("slot/");

    if (*p == '\0') {
        return false;
    }
    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return false;
        }

        parsed = parsed * 10 + (*p - '0');
        if (parsed >= MAX_SLOTS) {
            return false;
        }

        p++;
    }

    *slot_idx = parsed;
    return true;
}

static int dm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    static uint8_t load_buf[sizeof(struct dm_slot_header) + MAX_EVENTS * sizeof(struct dm_event)];
    struct behavior_dynamic_macro_data *data = NULL;
    int slot_idx = -1;

    if (!parse_settings_slot_name(name, &data, &slot_idx)) {
        return -ENOENT;
    }

    if (!slot_is_nvs(slot_idx)) {
        LOG_DBG("Ignoring stored dynamic macro RAM slot %d", slot_idx);
        return 0;
    }

    if (len < sizeof(struct dm_slot_header)) {
        LOG_WRN("Slot %d: stored length %zu too small for header", slot_idx, len);
        return -EINVAL;
    }

    size_t read_len = len;
    if (read_len > sizeof(load_buf)) {
        read_len = sizeof(load_buf);
    }

    /* NVS read_cb does not track offset — read everything in one call */
    int rc = read_cb(cb_arg, load_buf, read_len);
    if (rc < (int)sizeof(struct dm_slot_header)) {
        LOG_WRN("Slot %d: read failed: %d", slot_idx, rc);
        return -EINVAL;
    }

    const struct dm_slot_header *header = (const struct dm_slot_header *)load_buf;

    if (header->version != DM_STORAGE_VERSION) {
        LOG_WRN("Slot %d: unknown storage version 0x%02x, clearing", slot_idx, header->version);
        char key[64];
        settings_slot_key(data, slot_idx, key, sizeof(key));
        settings_delete(key);
        return 0;
    }

    if (header->event_count > MAX_EVENTS) {
        LOG_WRN("Slot %d: event_count %u exceeds MAX_EVENTS", slot_idx,
                (unsigned int)header->event_count);
        return -EINVAL;
    }

    size_t events_size = header->event_count * sizeof(struct dm_event);
    size_t expected = sizeof(struct dm_slot_header) + events_size;
    if ((size_t)rc < expected) {
        LOG_WRN("Slot %d: expected %zu bytes for %u events, got %d",
                slot_idx, expected, (unsigned int)header->event_count, rc);
        return -EINVAL;
    }

    memset(&data->slots[slot_idx], 0, sizeof(struct dm_slot));
    data->slots[slot_idx].event_count = header->event_count;

    if (header->event_count > 0) {
        memcpy(data->slots[slot_idx].events, load_buf + sizeof(struct dm_slot_header), events_size);
    }

    LOG_DBG("Loaded dynamic macro slot %d with %u events", slot_idx,
            (unsigned int)data->slots[slot_idx].event_count);
    return 0;
}

static int dm_settings_commit(void) {
    return 0;
}

static int dm_settings_export(int (*storage_func)(const char *name, const void *value,
                                                   size_t val_len)) {
    static uint8_t export_buf[sizeof(struct dm_slot_header) + MAX_EVENTS * sizeof(struct dm_event)];

    for (size_t dev_idx = 0; dev_idx < dm_devices_len; dev_idx++) {
        const struct device *dev = dm_devices[dev_idx];
        struct behavior_dynamic_macro_data *data = dev->data;

        for (int i = 0; i < MAX_SLOTS; i++) {
            if (!slot_is_nvs(i) || slot_is_empty(data, i)) {
                continue;
            }

            char key[64];
            settings_slot_key(data, i, key, sizeof(key));

            struct dm_slot_header *header = (struct dm_slot_header *)export_buf;
            header->version = DM_STORAGE_VERSION;
            header->_reserved[0] = 0;
            header->_reserved[1] = 0;
            header->_reserved[2] = 0;
            header->event_count = data->slots[i].event_count;

            size_t events_size = data->slots[i].event_count * sizeof(struct dm_event);
            memcpy(export_buf + sizeof(struct dm_slot_header), data->slots[i].events, events_size);

            size_t data_size = sizeof(struct dm_slot_header) + events_size;
            int rc = storage_func(key, export_buf, data_size);
            if (rc) {
                return rc;
            }
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dm, "dm", NULL, dm_settings_set, dm_settings_commit,
                               dm_settings_export);

void dm_storage_save_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!slot_is_nvs(slot_idx)) {
        return;
    }

    enqueue_storage_op(data, DM_STORAGE_OP_SAVE, slot_idx, &data->slots[slot_idx]);
}

int dm_storage_delete_slot(struct behavior_dynamic_macro_data *data, int slot_idx) {
    if (!slot_is_nvs(slot_idx)) {
        return 0;
    }

    return enqueue_storage_op(data, DM_STORAGE_OP_DELETE, slot_idx, NULL);
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
void dm_storage_flush(void) {
    k_work_submit_to_queue(&dm_storage_work_q, &dm_storage_work);
    k_sleep(K_MSEC(100));
}

void dm_storage_test_reload(void) {
    for (size_t i = 0; i < dm_devices_len; i++) {
        struct behavior_dynamic_macro_data *data = dm_devices[i]->data;
        for (int j = 0; j < MAX_SLOTS; j++) {
            memset(&data->slots[j], 0, sizeof(struct dm_slot));
            atomic_clear_bit(data->pending_delete, j);
            data->slot_generation[j] = 0;
        }
    }

    int rc = settings_load();
    if (rc) {
        LOG_ERR("Test reload: settings_load failed: %d", rc);
    } else {
        LOG_DBG("Test reload: settings reloaded");
    }
}
#endif

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
