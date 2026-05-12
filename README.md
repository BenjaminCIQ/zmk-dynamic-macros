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

## Use Cases

Dynamic macros record raw HID events, so a single macro can mix typed text with modifier combos, navigation, and editing commands. Each key press + release costs 2 events; the default limit is 64 events per slot (32 full key taps).

### Text shortcuts

| Record | Status preview | Events | Use |
|--------|----------------|--------|-----|
| `user@example.com` | `S0: 'user@example.com' (36)` | 36 | Fill in your email on forms |
| `Best regards,` + Enter + `John` | `S1: 'Best regards,<RET>John' (38)` | 38 | Email sign-off |
| `Thanks, will do!` | `S2: 'Thanks, will do!' (34)` | 34 | Quick acknowledgement |
| `:-)` | `S3: ':-) (6)` | 6 | Emoticons

### Code snippets

| Record | Status preview | Events | Use |
|--------|----------------|--------|-----|
| `console.log();` + Left + Left | `S4: 'console.log();<LEFT><LEFT>' (34)` | 34 | JS debug print -- cursor lands between the parens |
| `() => {}` + Left | `S5: '() => {}<LEFT>' (18)` | 18 | Arrow function skeleton |
| `[]()` + Left + Left + Left | `S6: '[]()<LEFT><LEFT><LEFT>' (14)` | 14 | Markdown link template -- cursor in the brackets |

### Editing operations

| Record | Status preview | Events | Use |
|--------|----------------|--------|-----|
| Ctrl+A, Ctrl+C | `S7: '<LCTL+A><LCTL+C>' (8)` | 8 | Select all and copy |
| Ctrl+S, Ctrl+W | `S8: '<LCTL+S><LCTL+W>' (8)` | 8 | Save and close tab |
| Home, Shift+End, Ctrl+C | `S9: '<HOME><LSFT+END><LCTL+C>' (10)` | 10 | Select current line and copy |

### Mixed text and actions

These combine typed text with modifier combos and navigation in one recording.

| Record | Status preview | Events | Use |
|--------|----------------|--------|-----|
| `git commit -m ""` + Left | `S10: 'git commit -m ""<LEFT>' (36)` | 36 | Git commit -- cursor between the quotes, ready to type your message |
| Ctrl+H, then type `TODO` | `S11: '<LCTL+H>TODO' (12)` | 12 | Open find/replace pre-filled with a search term |
| Ctrl+T, type `github.com`, Enter | `S12: '<LCTL+T>github.com<RET>' (26)` | 26 | New browser tab straight to a URL |

### Creative and CAD workflows

Hotkey-heavy applications like Photoshop, Blender, and CAD tools benefit from chaining multiple shortcuts into a single key.

| Record | Status preview | Events | Use |
|--------|----------------|--------|-----|
| Ctrl+J, Ctrl+T | `S13: '<LCTL+J><LCTL+T>' (8)` | 8 | Photoshop -- duplicate layer and enter Free Transform in one tap |
| Ctrl+Shift+N, type `Sketch1`, Enter | `S14: '<LCTL+LSFT+N>Sketch1<RET>' (20)` | 20 | CAD / Photoshop -- new layer/sketch with a preset name, skipping the dialog |
| Ctrl+Alt+Shift+E | `S15: '<LCTL+LALT+LSFT+E>' (8)` | 8 | Photoshop -- stamp visible (flatten all layers into a new layer) |

## Setup

### 1. Add to west.yml

Add the module to your `config/west.yml` under the `projects` section:

```yaml
manifest:
  projects:
    # ... other projects
    - name: zmk-dynamic-macros
      path: modules/zmk/dynamic-macros
      url: https://github.com/BenjaminCIQ/dynamic_macros
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
# CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS=64
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
          ┌────────>│     IDLE     │<─────────────────────┐
          │         └──────┬───┬──┘                       │
          │     REC pressed│   │DEL pressed               │
          │                v   v                          │
          │  ┌───────────────┐ ┌──────────────┐           │
          │  │  RECORDING    │ │DELETE_PENDING │──────────┤
          │  └───────┬───────┘ └──────────────┘  slot     │
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
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS` | int | 64 | Max events per slot. Each key press + release = 2 events, so 64 events = 32 full key taps |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY` | int | 30 | Milliseconds between events during playback and feedback typing |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_ASSIGN_TIMEOUT` | int | 10000 | Milliseconds before pending assign/delete mode auto-cancels |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST` | bool | y | Enable NVS flash persistence (requires `CONFIG_SETTINGS`) |
| `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK` | bool | y | Enable typed feedback messages for state transitions |

## Constraints and Notes

### NVS Storage

With default settings (16 slots, 64 events each), a completely full set of persisted slots uses roughly **12 KB** of NVS flash storage before backend overhead. The nRF52840 (nice_nano) typically has a 24-32 KB NVS partition shared with BLE bonds and other ZMK settings. This fits comfortably, but be mindful if you increase `MAX_SLOTS` or `MAX_EVENTS` significantly.

Storage per slot is compact and only writes recorded events: `4 bytes (event count) + event_count * sizeof(struct dm_event)`. On typical ZMK targets, `struct dm_event` is padded to about 12 bytes, so a full 64-event slot is about 772 bytes before storage backend overhead.

Slots are saved individually -- only when assigned or deleted. This minimizes flash wear.

### RAM Usage

The module keeps all slots plus one recording buffer in RAM:
`(MAX_SLOTS + 1) * (4 + MAX_EVENTS * sizeof(struct dm_event))`, or roughly **13 KB** with default settings on typical ZMK targets.

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
