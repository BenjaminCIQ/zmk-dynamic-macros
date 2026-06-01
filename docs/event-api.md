# Event System API Reference

The dynamic macro event system provides notifications and query functions for display widgets and custom integrations.

## Configuration

Enable in your `.conf`:

```ini
CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS=y
```

When disabled, no events are raised and query functions are not compiled.

## Events

### Event Structure

```c
struct zmk_dynamic_macro_state_changed {
    enum zmk_dynamic_macro_state state;
    enum zmk_dynamic_macro_event_type event;
    int slot;
    bool slot_is_nvs;
};
```

| Field        | Description                                    |
| ------------ | ---------------------------------------------- |
| `state`      | Current macro system state after the event     |
| `event`      | What triggered this notification               |
| `slot`       | Affected slot index, or -1 if not applicable   |
| `slot_is_nvs`| True if slot is NVS, false if RAM              |

### States

| State                            | Meaning                    |
| -------------------------------- | -------------------------- |
| `ZMK_DYNAMIC_MACRO_STATE_IDLE`   | No operation in progress   |
| `ZMK_DYNAMIC_MACRO_STATE_RECORDING` | Currently recording     |
| `ZMK_DYNAMIC_MACRO_STATE_PLAYING`   | Playing back a macro    |

### Event Types

#### Normal Events

| Event                               | When raised                          |
| ----------------------------------- | ------------------------------------ |
| `ZMK_DYNAMIC_MACRO_RECORDING_STARTED` | Recording began                    |
| `ZMK_DYNAMIC_MACRO_RECORDING_STOPPED` | Recording stopped (before save)    |
| `ZMK_DYNAMIC_MACRO_SAVED`             | Macro saved to slot                |
| `ZMK_DYNAMIC_MACRO_DELETED`           | Slot cleared                       |
| `ZMK_DYNAMIC_MACRO_MOVED`             | Macro moved between slots          |
| `ZMK_DYNAMIC_MACRO_PLAY_STARTED`      | Playback began                     |
| `ZMK_DYNAMIC_MACRO_PLAY_FINISHED`     | Playback completed                 |
| `ZMK_DYNAMIC_MACRO_PREVIEW_READY`     | Preview data available for query   |

#### Error Events

| Event                                  | Meaning                           |
| -------------------------------------- | --------------------------------- |
| `ZMK_DYNAMIC_MACRO_ERROR_OVERFLOW`     | Recording buffer full             |
| `ZMK_DYNAMIC_MACRO_ERROR_SAVE_FAILED`  | NVS write failed                  |
| `ZMK_DYNAMIC_MACRO_ERROR_DELETE_FAILED`| NVS delete failed                 |
| `ZMK_DYNAMIC_MACRO_ERROR_QUEUE_FULL`   | Storage queue full, retry later   |
| `ZMK_DYNAMIC_MACRO_ERROR_SLOT_EMPTY`   | Attempted operation on empty slot |
| `ZMK_DYNAMIC_MACRO_ERROR_NO_RECORDING` | Stop pressed with no recording    |

## Subscribing to Events

```c
#include <zmk-behaviour-dynamic-macros/events/dynamic_macro_state_changed.h>

static int my_macro_listener(const zmk_event_t *eh) {
    const struct zmk_dynamic_macro_state_changed *ev = as_zmk_dynamic_macro_state_changed(eh);
    
    switch (ev->event) {
    case ZMK_DYNAMIC_MACRO_RECORDING_STARTED:
        // Update display to show recording indicator
        break;
    case ZMK_DYNAMIC_MACRO_SAVED:
        // Refresh slot display, ev->slot has the saved slot
        break;
    case ZMK_DYNAMIC_MACRO_PREVIEW_READY:
        // Fetch preview data for ev->slot
        char buf[64];
        dm_get_preview_string(ev->slot, buf, sizeof(buf));
        break;
    // ... handle other events
    }
    
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(my_macro_widget, my_macro_listener);
ZMK_SUBSCRIPTION(my_macro_widget, zmk_dynamic_macro_state_changed);
```

## Query API

Events notify that something changed. Query functions retrieve current state. Call these from your event handler or at any time.

### State Queries

#### `dm_get_state()`

```c
enum zmk_dynamic_macro_state dm_get_state(void);
```

Returns current state: `IDLE`, `RECORDING`, or `PLAYING`.

#### `dm_get_recording_event_count()`

```c
uint32_t dm_get_recording_event_count(void);
```

Returns number of events captured in current recording. Returns 0 if not recording.

### Slot Queries

#### `dm_is_slot_empty()`

```c
bool dm_is_slot_empty(int slot_idx);
```

Returns true if slot has no macro stored.

**Parameters:**
- `slot_idx`: Slot index (0 to NVS_SLOTS-1 for NVS, NVS_SLOTS to MAX_SLOTS-1 for RAM)

#### `dm_get_used_nvs_slots()`

```c
int dm_get_used_nvs_slots(void);
```

Returns count of non-empty NVS slots.

#### `dm_get_used_ram_slots()`

```c
int dm_get_used_ram_slots(void);
```

Returns count of non-empty RAM slots.

#### `dm_get_total_nvs_slots()`

```c
int dm_get_total_nvs_slots(void);
```

Returns total configured NVS slots (`CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_NVS_SLOTS`).

#### `dm_get_total_ram_slots()`

```c
int dm_get_total_ram_slots(void);
```

Returns total configured RAM slots (`CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_RAM_SLOTS`).

### Content Queries

#### `dm_get_preview_string()`

```c
int dm_get_preview_string(int slot_idx, char *buf, size_t len);
```

Writes a human-readable preview of slot contents to buffer.

**Parameters:**
- `slot_idx`: Slot index
- `buf`: Output buffer
- `len`: Buffer size

**Returns:** Number of characters written, or negative on error.

**Example output:** `"Hello World"` or `"Ctrl+C Ctrl+V"`

#### `dm_get_slot_events()`

```c
const struct dm_event *dm_get_slot_events(int slot_idx, uint32_t *count);
```

Returns pointer to raw event array for a slot.

**Parameters:**
- `slot_idx`: Slot index
- `count`: Output parameter, receives event count

**Returns:** Pointer to event array, or NULL if slot empty/invalid.

**Event structure:**
```c
struct dm_event {
    uint16_t usage_page;
    uint16_t keycode;
    uint8_t implicit_mods;
    uint8_t explicit_mods;
    uint8_t pressed;
    uint8_t _reserved;
};
```

## Preview Mode

To inspect a slot's contents:

1. User presses `&dm DM_PREVIEW 0`
2. User presses a slot key
3. System raises `ZMK_DYNAMIC_MACRO_PREVIEW_READY` with the slot index
4. Widget calls `dm_get_preview_string()` or `dm_get_slot_events()`

This allows widgets to show slot contents on demand without constantly polling.

## Thread Safety

Query functions read shared state. In ZMK's cooperative threading model, this is safe when called from event handlers or work queue items. The event tells you state changed; the query reads current state at call time.

## Example: Display Widget

```c
#include <zmk-behaviour-dynamic-macros/events/dynamic_macro_state_changed.h>

static void update_macro_display(void) {
    enum zmk_dynamic_macro_state state = dm_get_state();
    int nvs_used = dm_get_used_nvs_slots();
    int ram_used = dm_get_used_ram_slots();
    
    // Update your display with current state
    display_set_macro_state(state);
    display_set_slot_counts(nvs_used, ram_used);
}

static int macro_display_listener(const zmk_event_t *eh) {
    const struct zmk_dynamic_macro_state_changed *ev = as_zmk_dynamic_macro_state_changed(eh);
    
    if (ev->event == ZMK_DYNAMIC_MACRO_PREVIEW_READY) {
        char preview[64];
        dm_get_preview_string(ev->slot, preview, sizeof(preview));
        display_show_preview(ev->slot, preview);
    } else {
        update_macro_display();
    }
    
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(macro_display, macro_display_listener);
ZMK_SUBSCRIPTION(macro_display, zmk_dynamic_macro_state_changed);
```
