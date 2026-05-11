# zmk-dynamic-macros

A [ZMK](https://zmk.dev/) module that adds dynamic macro recording and playback to your keyboard. Record keystrokes on the fly, assign them to slots, and replay them with a single keypress. Macros persist across reboots via NVS flash storage.

## Features

- **Record** keystrokes from either half of a split keyboard
- **Play back** macros with configurable inter-event delay
- **Assign** recordings to any of 16 configurable slots
- **Delete** individual macro slots
- **Status** output showing all filled slots with their contents
- **NVS persistence** across reboots (configurable, enabled by default)
- **Typed feedback** for all operations (configurable, enabled by default)

## Setup

### 1. Add to west.yml

Add the module to your `config/west.yml` under the `projects` section:

```yaml
manifest:
  remotes:
    - name: yourusername
      url-base: https://github.com/yourusername
    # ... other remotes
  projects:
    # ... other projects
    - name: zmk-dynamic-macros
      path: modules/zmk/dynamic-macros
      remote: yourusername
      revision: main
```

### 2. Add includes to your keymap

At the top of your `.keymap` file:

```dts
#include <dt-bindings/zmk/dynamic_macros.h>
#include <behaviors/dynamic_macro.dtsi>
```

### 3. Add a macro layer

Create a layer in your keymap with slot keys and control keys:

```dts
#define MACRO 6  /* adjust layer number as needed */

layer_Macro {
    display-name = "Macro";
    bindings = <
        &dm DM_SLOT 0   &dm DM_SLOT 1   &dm DM_SLOT 2   &dm DM_SLOT 3   &none
        &dm DM_SLOT 4   &dm DM_SLOT 5   &dm DM_SLOT 6   &dm DM_SLOT 7   &none
        &none           &none           &none           &none           &none
                        &dm DM_REC 0    &dm DM_STP 0    &dm DM_DEL 0
        /* right side */
        &none           &dm DM_SLOT 8   &dm DM_SLOT 9   &dm DM_SLOT 10  &dm DM_SLOT 11
        &none           &dm DM_SLOT 12  &dm DM_SLOT 13  &dm DM_SLOT 14  &dm DM_SLOT 15
        &none           &none           &none           &none           &none
                        &dm DM_STATE 0  &tog MACRO      &none
    >;
};
```

### 4. Optional: configure in .conf

Add any overrides to your board's `.conf` file (e.g., `dasbob.conf`):

```ini
# All settings below show their defaults -- only add lines you want to change.
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_SLOTS=16
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS=32
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY=30
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT=10000
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST=y
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK=y
```

## Usage

### Recording a macro

1. Switch to the macro layer
2. Press **REC** -- feedback types `[DM REC]`
3. Switch back to your base layer and type the keystrokes you want to record
4. Switch to the macro layer and press **STOP** -- feedback types `[DM STOP]`
5. Press a **slot key** to store the recording -- feedback types `[DM SAVED N: '...']` showing the macro contents

If the slot is already occupied, you'll see `[DM SLOT N FULL]` and the module stays in assign mode so you can pick another slot. You must delete the existing macro first to reuse that slot.

If you don't press a slot key within the assign timeout (default 10 seconds), the recording is discarded.

### Playing a macro

1. Switch to the macro layer
2. Press a **slot key** that has a recorded macro
3. The macro plays back automatically

### Deleting a macro

1. Switch to the macro layer
2. Press **DEL** to enter delete mode
3. Press the **slot key** you want to clear -- feedback types `[DM DEL N]`

If the slot is already empty, you'll see `[DM SLOT N EMPTY]`.

### Viewing status

1. Switch to the macro layer
2. Press **STATE** -- types a full listing of all slots with their contents

Example output:
```
[DM 2/16]
S0: 'Hello world' (22)
S1: -
S2: '<LCTL+C><LCTL+V>' (8)
...
```

### Re-recording

Pressing **REC** while already recording restarts the recording (discards the current buffer).

## Commands Reference

| Keymap binding     | Action                                      |
|--------------------|---------------------------------------------|
| `&dm DM_REC 0`    | Start recording (param2 is unused, pass 0)  |
| `&dm DM_STP 0`    | Stop recording, enter assign mode            |
| `&dm DM_DEL 0`    | Enter delete mode                            |
| `&dm DM_STATE 0`  | Output status of all slots                   |
| `&dm DM_SLOT N`   | Interact with slot N (play, assign, or delete depending on current state) |

## State Machine

```
                    ┌──────────────┐
          ┌────────>│     IDLE     │<────────────────────┐
          │         └──────┬───┬──┘                      │
          │     REC pressed│   │DEL pressed              │
          │                v   v                         │
          │  ┌───────────────┐ ┌──────────────┐          │
          │  │  RECORDING    │ │DELETE_PENDING │──────────┤
          │  └───────┬───────┘ └──────────────┘  slot    │
          │  STOP    │              or timeout   pressed  │
          │  pressed │                                    │
          │          v                                    │
          │  ┌────────────────┐                           │
          │  │ PENDING_ASSIGN │───────────────────────────┤
          │  └────────────────┘  slot pressed             │
          │       or timeout     (empty = save,           │
          │                       full = reject)          │
          │                                               │
          │  ┌────────────────┐                           │
          └──│    PLAYING     │───────────────────────────┘
             └────────────────┘  playback complete
                slot pressed
               (non-empty, IDLE)
```

## Kconfig Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_SLOTS` | int | 16 | Number of macro storage slots |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS` | int | 32 | Max events per slot. Each key press + release = 2 events, so 32 events = 16 full key taps |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY` | int | 30 | Milliseconds between events during playback and feedback typing |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT` | int | 10000 | Milliseconds before pending assign/delete mode auto-cancels |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST` | bool | y | Enable NVS flash persistence (requires `CONFIG_SETTINGS`) |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK` | bool | y | Enable typed feedback messages for state transitions |

## Constraints and Notes

### NVS Storage

With default settings (16 slots, 32 events each), the module uses approximately **6 KB** of NVS flash storage. The nRF52840 (nice_nano) typically has a 24-32 KB NVS partition shared with BLE bonds and other ZMK settings. This fits comfortably, but be mindful if you increase `MAX_SLOTS` or `MAX_EVENTS` significantly.

Storage per slot: `4 bytes (event count) + MAX_EVENTS * 10 bytes (per event) = ~324 bytes`

Slots are saved individually -- only when assigned or deleted. This minimizes flash wear.

### RAM Usage

The module keeps all slots plus one recording buffer in RAM:
`(MAX_SLOTS + 1) * (4 + MAX_EVENTS * 10) bytes ≈ 5.5 KB` with default settings.

The nRF52840 has 256 KB RAM, so this is not a concern.

### Feedback Output

Typed feedback goes to **whatever application currently has focus** on the host computer. This means:
- `[DM REC]` will appear in your text editor, terminal, etc.
- The STATUS command types multiple lines of text

**Assumption:** The host keyboard layout is **US QWERTY**. Feedback text and macro previews use ASCII symbols (`[`, `]`, `:`, `'`, `+`) that map to specific HID keycodes assuming US QWERTY. If your host uses a different layout, wrapper symbols and printable preview characters may render differently.

Feedback can be disabled entirely by setting `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK=n` in your `.conf` file.

### Split Keyboards

The module runs on the **central** half only. ZMK's event system automatically merges key events from both halves on the central side, so keystrokes from either hand are captured during recording.

### Macro Content Display

When a macro is saved, its contents are displayed as a literal preview:
- **Printable text** (letters, numbers, punctuation, and spaces) is concatenated without extra separators, so `hello world` displays as `hello world`.
- **Shifted printable keys** render as the resulting character, so `LSFT+a` displays as `A` and `LSFT+1` displays as `!`.
- **Command/action keys** render inline as angle-bracket tokens, such as `<LCTL+C>`, `<TAB>`, `<BSPC>`, `<PGUP>`, or `<MEDIA>`.

Only Shift is treated as part of literal text output. Ctrl, Alt, Gui, navigation, editing, media, mouse, and other non-keyboard actions are shown as action tokens instead of being simulated in the preview.

### Slot Overwrite Protection

Assigning a recording to a slot that already contains a macro is **rejected**. You must explicitly delete the slot first. This prevents accidental overwriting.

## License

MIT
