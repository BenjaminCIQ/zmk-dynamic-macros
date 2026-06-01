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

## Use Cases

| Use Case | Description |
| --- | --- |
| **Text snippets** | Store email addresses, sign-offs, code snippets and other boilerplate in NVS slots for instant single-keypress replay — no companion app or OS text expander needed |
| **On-the-fly macro layers** | Dedicate a layer as a macro bank or scatter slot keys across existing layers. Record, replace and reorganise macros live, promoting RAM slots to NVS once keepers are identified |
| **Shortcut-heavy apps** | Map a layer as a macro pad for CAD, video editing, or similar workflows. Use RAM slots for session-only sequences and NVS slots for frequent shortcuts — no reflash when switching contexts, or new frequent shortcuts identified |
| **Layout prototyping** | Test key placement without reflashing by recording sequences into RAM slots. Especially useful given that macro creation and editing is not currently supported in ZMK Studio |

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

While recording, press a non-empty slot key to inline its contents. The chained events are copied into the recording buffer, not referenced — the original slot is unchanged and can be independently deleted or overwritten afterwards.

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
| `&dm DM_PREVIEW 0`  | Enter preview mode (requires EVENTS) |
| `&dm DM_FEEDBACK_INC 0` | Increase feedback level        |
| `&dm DM_FEEDBACK_DEC 0` | Decrease feedback level        |

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

### Feedback Style

| Option             | Description                      |
| ------------------ | -------------------------------- |
| `FEEDBACK_STYLE_FULL`  | Full text (default): `[DM SAVED N3]` |
| `FEEDBACK_STYLE_ARROW` | Compact ASCII: `>N3` (US locale only) |

ARROW uses single punctuation marks with fixed roles: `>` success, `-` delete, `!` error, `?` empty, `%` full, `<>` move, `>>` transfer, `*` record, `.` stop. See [docs/arrow-cheatsheet.md](docs/arrow-cheatsheet.md) for the full message reference.

| Operation | FULL | ARROW |
| --------- | ---- | ----- |
| Record | `[DM REC]` | `>*` |
| Stop | `[DM STOP]` | `>.` |
| Saved to N0 | `[DM SAVED N0]` | `>N0` |
| Saved (verbose) | `[DM SAVED N0: 'Hello']` | `>N0:'Hello'` |
| Deleted N0 | `[DM DEL N0]` | `-N0` |
| Slot empty | `[DM SLOT N0: -]` | `?N0` |
| Slot full | `[DM SLOT N0 FULL]` | `>N0%` |
| Buffer overflow | `[DM FULL]` | `!%` |
| Save failed | `[DM SAVE FAILED N0]` | `!>N0` |
| Move prompt | `[DM MOV]` | `<>` |
| Move done | `[DM MOV N0->N1]` | `>N0>>N1` |
| Status | `[DM 2/8 NVS:0-7 RAM:8-15]` | `==2/8 N0-7 R8-15` |

### Auto-Erase

| Option             | Default | Description                      |
| ------------------ | ------- | -------------------------------- |
| `FEEDBACK_AUTO_ERASE` | n    | Delete feedback after display    |
| `FEEDBACK_ERASE_DELAY`| 1500 | ms before erasing (500-10000)    |

When enabled, feedback text is automatically erased by emitting backspace keycodes after the configured delay. If you type before the delay expires, the erase is cancelled. Multi-line status output is excluded. Pairs well with ARROW style's short output.

### Locale

| Option      | Description                        |
| ----------- | ---------------------------------- |
| `LOCALE_US` | US QWERTY, full punctuation (default) |
| `LOCALE_UK` | UK QWERTY, plain mode              |
| `LOCALE_DE` | German QWERTZ, plain mode          |
| `LOCALE_FR` | French AZERTY, plain mode          |

Feedback works by typing HID keycodes into the focused application. Punctuation characters like `[`, `]`, `:`, `'` occupy different physical keys on each keyboard layout — a US bracket keycode produces a different character on a German or French host. Non-US locales therefore use plain mode (letters, digits, spaces only) to avoid garbled output. Letter and digit mappings are adjusted per locale (e.g. Y/Z swap for German QWERTZ, AZERTY positions for French).

#### Locale Feature Matrix

| Feature | US | UK | DE | FR |
| ------- | -- | -- | -- | -- |
| FULL style punctuation (`[DM SAVED N3]`) | Yes | No (plain) | No (plain) | No (plain) |
| ARROW style | Yes | No | No | No |
| Preview rendering (printable chars) | Accurate | US layout assumed | US layout assumed | US layout assumed |
| Feedback level adjustment | Yes | Yes | Yes | Yes |
| Auto-erase | Yes | Yes | Yes | Yes |
| Status output | Full | Plain | Plain | Plain |

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

### Slot Numbering

NVS and RAM slots use separate index spaces in bindings but share a single internal array:

| Binding | Internal Index | Label |
| ------- | -------------- | ----- |
| `&dm DM_SLOT_NVS 0` | 0 | N0 |
| `&dm DM_SLOT_NVS 7` | 7 | N7 |
| `&dm DM_SLOT_RAM 0` | 8 (= NVS_SLOTS) | R8 |
| `&dm DM_SLOT_RAM 7` | 15 | R15 |

Feedback messages and status output use the `N`/`R` prefix with the internal index (e.g. `N0`, `R8`). The binding index is always relative to the slot type.

### Feedback Examples

Sample output per feedback level (US locale). VERBOSE adds previews to save messages; lower levels show the same text but for fewer events.

| Operation | FULL | ARROW | VERBOSE | BASIC | COMMAND | ERROR |
| --------- | ---- | ----- | :-----: | :---: | :-----: | :---: |
| Record | `[DM REC]` | `>*` | x | x | x | |
| Stop | `[DM STOP]` | `>.` | x | x | x | |
| Save to N0 | `[DM SAVED N0]` | `>N0` | x | x | x | |
| Save (verbose) | `[DM SAVED N0: 'Hello']` | `>N0:'Hello'` | x | | | |
| Delete N0 | `[DM DEL N0]` | `-N0` | x | x | x | |
| Play empty | `[DM SLOT N0: -]` | `?N0` | x | x | | |
| Slot full | `[DM SLOT N0 FULL]` | `>N0%` | x | x | x | |
| Buffer overflow | `[DM FULL]` | `!%` | x | x | x | x |
| Save failed | `[DM SAVE FAILED N0]` | `!>N0` | x | x | x | x |
| Queue full | `[DM SAVE QUEUE FULL N0]` | `!>%N0` | x | x | x | x |

Feedback level can be adjusted at runtime with `DM_FEEDBACK_INC` / `DM_FEEDBACK_DEC` (persisted across reboots). The minimum runtime level is ERROR — OFF is only available at build time.

### Storage

- **RAM:** each slot index (`NVS_SLOTS + RAM_SLOTS`) reserves a full in-memory buffer — `4 + MAX_EVENTS × 8` bytes (~520 B at default 64 events), whether empty or not, plus one recording buffer. Example: 8 NVS + 8 RAM → (16 + 1) × ~520 B ≈ 8.8 KB. RAM slot contents are lost on reboot; NVS slots are loaded back from flash into the same buffers.
- **NVS (flash):** only written when you save to an NVS slot — `8` byte header + `8` bytes × event count (not padded to `MAX_EVENTS`). Empty slots use no flash entry. Max ~520 B per saved slot at default settings. Shares ZMK settings partition; format version in header — incompatible upgrades clear saved macros.

### Flash Wear

NVS slots write to flash on every save and delete. ZMK's NVS implementation includes wear leveling, but flash has finite write endurance (typically 10,000+ cycles). For macros you change frequently, prefer RAM slots and promote to NVS only once stable.

### Feedback

Typed feedback goes to the focused application. Use a text editor when testing status output.

### Split Keyboards

Runs on central half only. Both halves' keystrokes are captured during recording.

### Troubleshooting

| Symptom | Cause | Fix |
| ------- | ----- | --- |
| `[DM FULL]` during recording | Recording buffer reached MAX_EVENTS | Increase `MAX_EVENTS` or record a shorter sequence. The partial recording can still be saved. |
| `[DM SAVE FAILED N0]` | NVS write error | Check flash health. Settings partition may be full — reduce NVS_SLOTS or MAX_EVENTS. |
| `[DM SAVE QUEUE FULL N0]` | Too many storage operations queued | Wait a moment and retry. Occurs when rapidly saving/deleting multiple NVS slots. |
| Slot shows occupied but was deleted | NVS delete still in progress | The slot is marked pending-delete and will clear shortly. It cannot be played or assigned during this time. |
| Feedback not appearing | Feedback level set to OFF | Press `DM_FEEDBACK_INC` to raise the level, or set `FEEDBACK_VERBOSE` in .conf. |
| Wrong characters in feedback | Non-US locale with US punctuation | Non-US locales use plain mode. Set `LOCALE_US` for full punctuation output. |
| Macro plays wrong keys | Recorded on different layer/layout | Macros record HID keycodes, not physical positions. Replay on the same layout used during recording. |

### Compatibility

Requires ZMK main branch. Tested with Zephyr 4.1+.

## License

This module is released under the [MIT License](LICENSE).
