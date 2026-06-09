/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_nvs — storage backend (see dm_nvs.h).
 *
 * File-scoped/single-instance (ADR-0002): one work queue, one msgq, one buffer.
 * slot_store reaches it only through the dm_nvs_sink; the async outcome flows back
 * UP through the system-queue completion (slot_store_complete_delete +
 * dm_machine_deliver_async). The storage thread never touches slot state.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk-behavior-dynamic-macros/dm_internal.h>
#include <zmk-behavior-dynamic-macros/dm_machine.h>
#include <zmk-behavior-dynamic-macros/dm_nvs.h>
#include <zmk-behavior-dynamic-macros/slot_store.h>
#if DM_TYPING_ENABLED
#include <zmk-behavior-dynamic-macros/dm_feedback_pump.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DM_DRV_COMPAT zmk_behavior_dynamic_macro

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST)

#define DM_STORAGE_VERSION 0xD1

struct dm_slot_header {
    uint8_t  version;
    uint8_t  _reserved[3];
    uint32_t event_count;
} __packed;

BUILD_ASSERT(sizeof(struct dm_slot_header) == 8, "dm_slot_header must be 8 bytes packed");

/* ---- the single instance's wiring (file-scoped per ADR-0002) --------------- */

static slot_store        *dm_store;
static struct dm_machine *dm_machine_ref;
#if DM_TYPING_ENABLED
static struct dm_feedback *dm_feedback_ref;
#endif
static const char        *dm_settings_name;

/* ---- async storage op queue ------------------------------------------------ */

enum dm_storage_op_type {
    DM_STORAGE_OP_SAVE,
    DM_STORAGE_OP_DELETE,
#if DM_TYPING_ENABLED
    DM_STORAGE_OP_SAVE_KNOBS,
#endif
};

struct dm_storage_op {
    enum dm_storage_op_type type;
    int                     slot_idx;
    uint32_t                generation;
    struct dm_slot          slot;
#if DM_TYPING_ENABLED
    uint8_t level;
    uint8_t style;
    uint8_t erase;
#endif
};

#define DM_STORAGE_STACK_SIZE 1024
#define DM_STORAGE_QUEUE_LEN  4
#define DM_STORAGE_PRIORITY   10

K_KERNEL_STACK_DEFINE(dm_storage_work_q_stack, DM_STORAGE_STACK_SIZE);
K_MSGQ_DEFINE(dm_storage_msgq, sizeof(struct dm_storage_op), DM_STORAGE_QUEUE_LEN, 4);

static struct k_work_q dm_storage_work_q;
static struct k_work   dm_storage_work;
static bool            dm_storage_started;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
static K_SEM_DEFINE(dm_storage_flush_sem, 0, 1);
#endif

/* ---- system-queue completion ----------------------------------------------
 * The storage thread submits one completion per finished op; it runs on the
 * SYSTEM work queue and is the ONLY place slot state is touched after enqueue.
 * For a delete it runs slot_store_complete_delete (which honors the playing-slot
 * rule + staleness) then dm_machine_deliver_async with the filtered outcome; for
 * a save failure it just drives deliver_async with DM_SAVE_FAILED.
 */

enum dm_completion_kind {
    DM_COMPLETE_DELETE,
    DM_COMPLETE_SAVE_FAILED,
};

struct dm_completion {
    enum dm_completion_kind kind;
    int                     slot_idx;
    uint32_t                generation;
    bool                    ok; /* storage verdict, for DM_COMPLETE_DELETE */
};

static struct k_work dm_completion_work;
K_MSGQ_DEFINE(dm_completion_msgq, sizeof(struct dm_completion), DM_STORAGE_QUEUE_LEN, 4);

static void dm_completion_handler(struct k_work *work) {
    struct dm_completion c;
    if (k_msgq_get(&dm_completion_msgq, &c, K_NO_WAIT) != 0) {
        return;
    }

    if (c.kind == DM_COMPLETE_DELETE) {
        dm_result outcome = slot_store_complete_delete(dm_store, c.slot_idx, c.generation, c.ok);
        dm_machine_deliver_async(dm_machine_ref, outcome, c.slot_idx);
    } else { /* DM_COMPLETE_SAVE_FAILED */
        dm_machine_deliver_async(dm_machine_ref, DM_SAVE_FAILED, c.slot_idx);
    }

    if (k_msgq_num_used_get(&dm_completion_msgq) > 0) {
        k_work_submit(&dm_completion_work);
    }
}

static void submit_completion(const struct dm_completion *c) {
    if (k_msgq_put(&dm_completion_msgq, c, K_NO_WAIT) != 0) {
        LOG_ERR("Dynamic macro completion queue full, outcome dropped");
        return;
    }
    k_work_submit(&dm_completion_work);
}

/* ---- settings keys --------------------------------------------------------- */

static void slot_key(int slot_idx, char *key, size_t key_len) {
    snprintf(key, key_len, "dm/%s/slot/%d", dm_settings_name, slot_idx);
}

#if DM_TYPING_ENABLED
static void knob_key(const char *leaf, char *key, size_t key_len) {
    snprintf(key, key_len, "dm/%s/%s", dm_settings_name, leaf);
}
#endif

/* ---- the storage work handler (runs on dm_storage_work_q) ------------------- */

static void dm_storage_work_handler(struct k_work *work) {
    static struct dm_storage_op op;
    static uint8_t save_buf[sizeof(struct dm_slot_header) + MAX_EVENTS * sizeof(struct dm_event)];

    while (k_msgq_get(&dm_storage_msgq, &op, K_NO_WAIT) == 0) {
#if DM_TYPING_ENABLED
        if (op.type == DM_STORAGE_OP_SAVE_KNOBS) {
            char key[64];
            knob_key("fblevel", key, sizeof(key));
            int rc = settings_save_one(key, &op.level, sizeof(op.level));
            if (rc) {
                LOG_ERR("Failed to save feedback level: %d", rc);
            }
            knob_key("fbstyle", key, sizeof(key));
            rc = settings_save_one(key, &op.style, sizeof(op.style));
            if (rc) {
                LOG_ERR("Failed to save feedback style: %d", rc);
            }
            knob_key("fberase", key, sizeof(key));
            rc = settings_save_one(key, &op.erase, sizeof(op.erase));
            if (rc) {
                LOG_ERR("Failed to save auto-erase setting: %d", rc);
            }
            continue;
        }
#endif

        char key[64];
        slot_key(op.slot_idx, key, sizeof(key));

        if (op.type == DM_STORAGE_OP_SAVE) {
            /* aligned local header, then copied into the (alignment-1) byte buffer
             * so no packed member is written through a misaligned cast. */
            struct dm_slot_header header = {
                .version = DM_STORAGE_VERSION,
                .event_count = op.slot.event_count,
            };
            memcpy(save_buf, &header, sizeof(header));

            size_t events_size = op.slot.event_count * sizeof(struct dm_event);
            memcpy(save_buf + sizeof(struct dm_slot_header), op.slot.events, events_size);

            size_t data_size = sizeof(struct dm_slot_header) + events_size;
            int rc = settings_save_one(key, save_buf, data_size);
            if (rc) {
                LOG_ERR("Failed to save dynamic macro slot %d: %d", op.slot_idx, rc);
                submit_completion(&(struct dm_completion){
                    .kind = DM_COMPLETE_SAVE_FAILED, .slot_idx = op.slot_idx});
            } else {
                LOG_DBG("Saved dynamic macro slot %d (%u events)", op.slot_idx,
                        (unsigned int)op.slot.event_count);
            }
            continue;
        }

        /* DM_STORAGE_OP_DELETE */
        int rc = settings_delete(key);
        submit_completion(&(struct dm_completion){
            .kind = DM_COMPLETE_DELETE,
            .slot_idx = op.slot_idx,
            .generation = op.generation,
            .ok = (rc == 0),
        });
        if (rc) {
            LOG_ERR("Failed to delete dynamic macro slot %d: %d", op.slot_idx, rc);
        } else {
            LOG_DBG("Deleted dynamic macro slot %d from storage", op.slot_idx);
        }
    }
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
    k_sem_give(&dm_storage_flush_sem);
#endif
}

static dm_result enqueue(enum dm_storage_op_type type, int slot_idx, const struct dm_slot *slot,
                         uint32_t generation) {
    struct dm_storage_op op = {0};
    op.type = type;
    op.slot_idx = slot_idx;
    op.generation = generation;
    if (slot != NULL) {
        memcpy(&op.slot, slot, sizeof(op.slot));
    }

    int rc = k_msgq_put(&dm_storage_msgq, &op, K_NO_WAIT);
    if (rc) {
        LOG_ERR("Dynamic macro storage queue full for slot %d: %d", slot_idx, rc);
        return (type == DM_STORAGE_OP_SAVE) ? DM_SAVE_QUEUE_FULL : DM_DELETE_QUEUE_FULL;
    }
    k_work_submit_to_queue(&dm_storage_work_q, &dm_storage_work);
    return DM_OK;
}

/* ---- the dm_nvs_sink slot_store calls -------------------------------------- */

static dm_result sink_save(void *ctx, int slot, const struct dm_slot *s, uint32_t generation) {
    (void)ctx;
    return enqueue(DM_STORAGE_OP_SAVE, slot, s, generation);
}

static dm_result sink_del(void *ctx, int slot, uint32_t generation) {
    (void)ctx;
    return enqueue(DM_STORAGE_OP_DELETE, slot, NULL, generation);
}

static const dm_nvs_sink dm_nvs_sink_impl = {
    .save = sink_save,
    .del = sink_del,
    .ctx = NULL,
};

const dm_nvs_sink *dm_nvs_sink_get(void) {
    return &dm_nvs_sink_impl;
}

#if DM_TYPING_ENABLED
void dm_nvs_save_knobs(uint8_t level, uint8_t style, bool erase) {
    struct dm_storage_op op = {
        .type = DM_STORAGE_OP_SAVE_KNOBS,
        .level = level,
        .style = style,
        .erase = (uint8_t)erase,
    };
    if (k_msgq_put(&dm_storage_msgq, &op, K_NO_WAIT) != 0) {
        LOG_ERR("Storage queue full, knob save dropped");
        return;
    }
    k_work_submit_to_queue(&dm_storage_work_q, &dm_storage_work);
}
#else
void dm_nvs_save_knobs(uint8_t level, uint8_t style, bool erase) {
    (void)level; (void)style; (void)erase;
}
#endif

/* ---- settings handler (boot restore + export) ------------------------------ */

static const char *match_device(const char *name) {
    if (dm_settings_name == NULL) {
        return NULL;
    }
    size_t n = strlen(dm_settings_name);
    if (strncmp(name, dm_settings_name, n) != 0 || name[n] != '/') {
        return NULL;
    }
    return name + n + 1;
}

static bool parse_slot(const char *suffix, int *slot_idx) {
    if (strncmp(suffix, "slot/", strlen("slot/")) != 0) {
        return false;
    }
    const char *p = suffix + strlen("slot/");
    if (*p == '\0') {
        return false;
    }
    int parsed = 0;
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

    const char *suffix = match_device(name);
    if (suffix == NULL) {
        return -ENOENT;
    }

#if DM_TYPING_ENABLED
    if (strcmp(suffix, "fblevel") == 0) {
        uint8_t level = 0;
        if (read_cb(cb_arg, &level, sizeof(level)) == sizeof(level)) {
            dm_feedback_restore_level(dm_feedback_ref, level);
        }
        return 0;
    }
    if (strcmp(suffix, "fbstyle") == 0) {
        uint8_t style = 0;
        if (read_cb(cb_arg, &style, sizeof(style)) == sizeof(style)) {
            dm_feedback_restore_style(dm_feedback_ref, style);
        }
        return 0;
    }
    if (strcmp(suffix, "fberase") == 0) {
        uint8_t erase = 0;
        if (read_cb(cb_arg, &erase, sizeof(erase)) == sizeof(erase)) {
            dm_feedback_restore_erase(dm_feedback_ref, (bool)erase);
        }
        return 0;
    }
#endif

    int slot_idx = -1;
    if (!parse_slot(suffix, &slot_idx)) {
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

    size_t read_len = len > sizeof(load_buf) ? sizeof(load_buf) : len;
    int rc = read_cb(cb_arg, load_buf, read_len);
    if (rc < (int)sizeof(struct dm_slot_header)) {
        LOG_WRN("Slot %d: read failed: %d", slot_idx, rc);
        return -EINVAL;
    }

    const struct dm_slot_header *header = (const struct dm_slot_header *)load_buf;
    if (header->version != DM_STORAGE_VERSION) {
        LOG_WRN("Slot %d: unknown storage version 0x%02x, clearing", slot_idx, header->version);
        char key[64];
        slot_key(slot_idx, key, sizeof(key));
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
        LOG_WRN("Slot %d: expected %zu bytes for %u events, got %d", slot_idx, expected,
                (unsigned int)header->event_count, rc);
        return -EINVAL;
    }

    /* deliver the decoded slot UP to its owner (slot_store_load is a raw populate:
     * no sink echo, no generation bump). */
    const struct dm_event *events =
        (const struct dm_event *)(load_buf + sizeof(struct dm_slot_header));
    if (!slot_store_load(dm_store, slot_idx, events, header->event_count)) {
        return -EINVAL;
    }
    LOG_DBG("Loaded dynamic macro slot %d with %u events", slot_idx,
            (unsigned int)header->event_count);
    return 0;
}

static int dm_settings_commit(void) {
    return 0;
}

static int dm_settings_export(int (*storage_func)(const char *name, const void *value,
                                                  size_t val_len)) {
    static uint8_t export_buf[sizeof(struct dm_slot_header) + MAX_EVENTS * sizeof(struct dm_event)];

    /* export reads back ONLY through the public query API (the one documented
     * up-read, single-instance-anchored). */
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slot_is_nvs(i)) {
            continue;
        }
        const struct dm_slot *s = slot_store_get(dm_store, i);
        if (s == NULL) {
            continue;
        }

        char key[64];
        slot_key(i, key, sizeof(key));

        struct dm_slot_header header = {
            .version = DM_STORAGE_VERSION,
            .event_count = s->event_count,
        };
        memcpy(export_buf, &header, sizeof(header));
        size_t events_size = s->event_count * sizeof(struct dm_event);
        memcpy(export_buf + sizeof(struct dm_slot_header), s->events, events_size);

        int rc = storage_func(key, export_buf, sizeof(struct dm_slot_header) + events_size);
        if (rc) {
            return rc;
        }
    }

#if DM_TYPING_ENABLED
    if (dm_feedback_level(dm_feedback_ref) != DM_FEEDBACK_LEVEL) {
        char key[64];
        knob_key("fblevel", key, sizeof(key));
        uint8_t level = dm_feedback_level(dm_feedback_ref);
        int rc = storage_func(key, &level, sizeof(level));
        if (rc) {
            return rc;
        }
    }
    if (dm_feedback_style(dm_feedback_ref) != DM_FEEDBACK_STYLE) {
        char key[64];
        knob_key("fbstyle", key, sizeof(key));
        uint8_t style = dm_feedback_style(dm_feedback_ref);
        int rc = storage_func(key, &style, sizeof(style));
        if (rc) {
            return rc;
        }
    }
    {
        bool default_erase = IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE);
        if (dm_feedback_erase(dm_feedback_ref) != default_erase) {
            char key[64];
            knob_key("fberase", key, sizeof(key));
            uint8_t erase = (uint8_t)dm_feedback_erase(dm_feedback_ref);
            int rc = storage_func(key, &erase, sizeof(erase));
            if (rc) {
                return rc;
            }
        }
    }
#endif
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dm, "dm", NULL, dm_settings_set, dm_settings_commit,
                               dm_settings_export);

/* ---- lifecycle ------------------------------------------------------------- */

void dm_nvs_init(slot_store *store, struct dm_machine *machine, struct dm_feedback *feedback,
                 const char *settings_name) {
    dm_store = store;
    dm_machine_ref = machine;
#if DM_TYPING_ENABLED
    dm_feedback_ref = feedback;
#else
    (void)feedback;
#endif
    dm_settings_name = settings_name;

    if (dm_storage_started) {
        return;
    }
    k_work_init(&dm_storage_work, dm_storage_work_handler);
    k_work_init(&dm_completion_work, dm_completion_handler);
    k_work_queue_start(&dm_storage_work_q, dm_storage_work_q_stack,
                       K_KERNEL_STACK_SIZEOF(dm_storage_work_q_stack), DM_STORAGE_PRIORITY, NULL);
    dm_storage_started = true;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TEST_RELOAD)
void dm_nvs_test_reload(void) {
    k_sem_reset(&dm_storage_flush_sem);
    k_work_submit_to_queue(&dm_storage_work_q, &dm_storage_work);
    k_sem_take(&dm_storage_flush_sem, K_FOREVER);

    slot_store_reset(dm_store);

    int rc = settings_load();
    if (rc) {
        LOG_ERR("Test reload: settings_load failed: %d", rc);
    } else {
        LOG_DBG("Test reload: settings reloaded");
    }
}
#endif

#endif /* CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST */
