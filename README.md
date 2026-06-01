# zmk-dynamic-macros

A [ZMK](https://zmk.dev/) module for dynamic macro recording and playback. Record keystrokes on the fly, save to persistent or temporary slots, and replay with a single key.

## Features

- **Record** keystrokes from either half of a split keyboard
- **Play back** macros with configurable delay
- **NVS slots** persist across reboots (`N0`, `N1`, etc.)
- **RAM slots** are temporary, never touch flash (`R8`, `R9`, etc.)
- **Move** macros between slots (promote RAM to NVS, reorganize, etc.)
- **Chain** existing macros into new recordings
- **Status** output showing slot contents
- **Locale support** for US, UK, German, and French keyboards
- **Event system** for display widgets and custom integrations

## Setup

### 1. Add to west.yml

```yaml
manifest:
  projects:
    - name: zmk-dynamic-macros
      path: modules/zmk/dynamic-macros
      url: https://github.com/BenjaminCIQ/zmk-dynamic-macros
      revision: main
```

### 2. Add includes to your keymap

```dts
#include <dt-bindings/zmk/dynamic_macros.h>
#include <behaviors/dynamic_macro.dtsi>
```

### 3. Add bindings to your keymap

Place slot and control keys anywhere in your keymap. Example dedicated layer:

```dts
layer_Macro {
    bindings = <
        &dm DM_SLOT_NVS 0  &dm DM_SLOT_NVS 1  &dm DM_SLOT_NVS 2  &dm DM_SLOT_NVS 3
        &dm DM_SLOT_RAM 0  &dm DM_SLOT_RAM 1  &dm DM_SLOT_RAM 2  &dm DM_SLOT_RAM 3
        &dm DM_REC 0       &dm DM_STP 0       &dm DM_DEL 0       &dm DM_STATE 0
    >;
};
```

Or scatter them across layers as needed.

### 4. Optional: configure in .conf

```ini
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS=64
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_NVS_SLOTS=8
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_RAM_SLOTS=8
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_VERBOSE=y
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_LOCALE_US=y
```

## Usage

### Recording

1. Press **REC** to start recording
2. Type your keystrokes
3. Press **STOP** to finish
4. Press a **slot key** to save

Slots must be empty. Delete first to overwrite.

### Playback

Press a non-empty **slot key** to play.

### Deleting

1. Press **DEL** to enter delete mode
2. Press the **slot key** to clear

### Moving

1. Press **MOV**
2. Press **source slot**
3. Press **destination slot** (must be empty)

### Chaining

While recording, press a non-empty slot key to inline its contents. The chained events are copied, not referenced.

### Status

Press **STATE** to output slot info to the focused window.

## Bindings Reference

| Binding             | Action                           |
| ------------------- | -------------------------------- |
| `&dm DM_REC 0`      | Start recording                  |
| `&dm DM_STP 0`      | Stop recording                   |
| `&dm DM_DEL 0`      | Enter delete mode                |
| `&dm DM_MOV 0`      | Enter move mode                  |
| `&dm DM_STATE 0`    | Output status                    |
| `&dm DM_SLOT_NVS N` | NVS slot N (0 to NVS_SLOTS-1)    |
| `&dm DM_SLOT_RAM N` | RAM slot N (0 to RAM_SLOTS-1)    |
| `&dm DM_PREVIEW 0`  | Enter preview mode               |

## Kconfig Options

### Core

| Option           | Default | Description                          |
| ---------------- | ------- | ------------------------------------ |
| `MAX_EVENTS`     | 64      | Events per slot (press+release = 2)  |
| `TAP_DELAY`      | 30      | ms between events during playback    |
| `ASSIGN_TIMEOUT` | 10000   | ms before pending mode auto-cancels  |
| `PERSIST`        | y       | Enable NVS persistence               |
| `NVS_SLOTS`      | 8       | Persistent slots (0-16)              |
| `RAM_SLOTS`      | 8       | Temporary slots (0-48)               |

All options prefixed with `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_`.

### Feedback

| Option             | Description                      |
| ------------------ | -------------------------------- |
| `FEEDBACK_OFF`     | No typed feedback                |
| `FEEDBACK_ERROR`   | Errors only                      |
| `FEEDBACK_COMMAND` | Confirmations, no empty slot msgs|
| `FEEDBACK_BASIC`   | Short confirmations              |
| `FEEDBACK_VERBOSE` | Full previews (default)          |

### Locale

| Option      | Description                        |
| ----------- | ---------------------------------- |
| `LOCALE_US` | US QWERTY, full punctuation (default) |
| `LOCALE_UK` | UK QWERTY, plain mode              |
| `LOCALE_DE` | German QWERTZ, plain mode          |
| `LOCALE_FR` | French AZERTY, plain mode          |

Non-US locales use plain messages (letters, digits, spaces only) with correct key mappings.

### Status Detail

| Option                | Description                    |
| --------------------- | ------------------------------ |
| `STATUS_OFF`          | No output                      |
| `STATUS_COUNT`        | Count only                     |
| `STATUS_USED`         | Used slots, no preview         |
| `STATUS_USED_PREVIEW` | Used slots with preview        |
| `STATUS_FULL`         | All slots (default for VERBOSE)|

### Events

| Option   | Default | Description                        |
| -------- | ------- | ---------------------------------- |
| `EVENTS` | n       | Enable event system for widgets    |

## Event System

For display widgets or custom integrations, enable the event system to receive notifications when macro state changes.

Enable in `.conf`:

```ini
CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS=y
```

The event system provides:
- **Events** for state changes (recording started/stopped, saved, deleted, playback, errors)
- **Query API** to get current state, slot counts, and slot contents
- **Preview mode** (`DM_PREVIEW`) to inspect slot contents on demand

See [docs/event-api.md](docs/event-api.md) for full API reference, code examples, and integration guide.

## Notes

### Storage

- NVS slots persist; RAM slots are lost on reboot
- Storage uses ~520 bytes per full slot
- Format includes version header; incompatible upgrades clear saved macros

### Feedback

Typed feedback goes to the focused application. Use a text editor when testing status output.

### Split Keyboards

Runs on central half only. Both halves' keystrokes are captured during recording.

## License

This module is released under the [MIT License](LICENSE).
