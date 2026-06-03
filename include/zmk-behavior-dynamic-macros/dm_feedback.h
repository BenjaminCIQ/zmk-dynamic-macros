/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef DM_FEEDBACK_H
#define DM_FEEDBACK_H

#include <zmk-behavior-dynamic-macros/dm_internal.h>

/* Feedback functions called from command handlers */
void feedback_rec(struct behavior_dynamic_macro_data *data);
void feedback_stop(struct behavior_dynamic_macro_data *data);
void feedback_saved(struct behavior_dynamic_macro_data *data, int slot_idx,
                    const struct dm_slot *slot);
void feedback_slot_full(struct behavior_dynamic_macro_data *data, int slot_idx);
void feedback_slot_empty(struct behavior_dynamic_macro_data *data, int slot_idx);
void feedback_overflow(struct behavior_dynamic_macro_data *data);
void feedback_status(struct behavior_dynamic_macro_data *data);
void feedback_move_prompt(struct behavior_dynamic_macro_data *data);
void feedback_move_source_selected(struct behavior_dynamic_macro_data *data, int slot_idx);
void feedback_moved(struct behavior_dynamic_macro_data *data, int src, int dst);
void feedback_move_cancelled(struct behavior_dynamic_macro_data *data);
void feedback_chain_insert(struct behavior_dynamic_macro_data *data, int slot_idx,
                           const struct dm_slot *slot);
void feedback_chain_empty(struct behavior_dynamic_macro_data *data, int slot_idx);
void feedback_chain_no_room(struct behavior_dynamic_macro_data *data, int slot_idx);

/* Storage callback feedback (called from dm_storage.c and cmd_slot) */
void dm_feedback_deleted(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_delete_failed(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_save_failed(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_save_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_delete_queue_full(struct behavior_dynamic_macro_data *data, int slot_idx);

#if DM_TYPING_ENABLED
void cmd_feedback_adjust(struct behavior_dynamic_macro_data *data, int direction);
void cmd_style_toggle(struct behavior_dynamic_macro_data *data);
void cmd_erase_toggle(struct behavior_dynamic_macro_data *data);
#endif

/* Emit handler support: called from emit_work_handler in core */
void feedback_complete(struct behavior_dynamic_macro_data *data);

#if DM_TYPING_ENABLED
bool ring_empty(struct behavior_dynamic_macro_data *data);
bool render_slot_contents_stream(struct behavior_dynamic_macro_data *data);
void status_slot_suffix(struct behavior_dynamic_macro_data *data, int slot_idx);
void dm_feedback_preview_suffix(struct behavior_dynamic_macro_data *data);
#endif

/* Preview helpers shared with query API (dm_get_preview_string) */
#if DM_TYPING_ENABLED

#define MOD_SHIFT_MASK (0x02 | 0x20)
#define MOD_NON_SHIFT_MASK (~MOD_SHIFT_MASK & 0xFF)

bool printable_char_for_keycode(uint32_t keycode, bool shifted, char *out);
bool is_modifier_key(uint16_t usage_page, uint32_t keycode);
const char *action_name(uint16_t usage_page, uint32_t keycode);
uint8_t token_size(uint8_t mods, uint16_t usage_page, uint32_t keycode);
size_t render_token_to_buf(char *buf, size_t pos, size_t len,
                           uint8_t mods, uint16_t usage_page, uint32_t keycode);

extern const char *mod_names[];

/* Slot counting helpers shared with query API */
int filled_slot_count(struct behavior_dynamic_macro_data *data);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS)
int filled_nvs_slot_count(struct behavior_dynamic_macro_data *data);
int filled_ram_slot_count(struct behavior_dynamic_macro_data *data);
#endif

#endif /* DM_TYPING_ENABLED */

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_AUTO_ERASE)
void dm_feedback_erase_init(struct behavior_dynamic_macro_data *data);
void dm_feedback_cancel_erase(struct behavior_dynamic_macro_data *data);
#endif

#endif /* DM_FEEDBACK_H */
