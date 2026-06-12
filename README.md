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

### 1. Add to config/west.yml

Add this module's remote and project alongside ZMK's so that your manifest looks like:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: benjaminciq
      url-base: https://github.com/BenjaminCIQ
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: v0.3 # Set to desired ZMK release.
      import: app/west.yml
    - name: zmk-dynamic-macros
      remote: benjaminciq
      revision: v0.3 # Match this to your ZMK major.minor
    # - name: <other-module> ...   # add other modules here
  self:
    path: config
```

This module is tested against ZMK via urob's
[zmk-actions](https://github.com/urob/zmk-actions). Its version is a **hybrid**:
the **major.minor** matches the ZMK line it targets (so `v0.3.x` works with ZMK
`0.3`), while the **patch** is the module's own — fixes ship on the module's
schedule, independent of ZMK's release cadence. **Pin the module to the same
major.minor as your ZMK:**

- `revision: v0.3` — floating minor tag; tracks the module's latest patch for the
  ZMK 0.3 line (recommended).
- `revision: v0.3.0` — an exact, immutable release.
- a **full 40-char commit SHA** — fully immutable (west cannot resolve abbreviated
  hashes); loses the version-matching convenience and auto-fixes.
- `revision: main` — latest unreleased changes; may be unstable.

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

See [docs/keycodes.md](docs/keycodes.md) for the full binding reference including command descriptions, param2 rules, feedback output, and per-command requirements.

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
| `&dm DM_FEEDBACK_INC 0` | Increase feedback verbosity (persisted) |
| `&dm DM_FEEDBACK_DEC 0` | Decrease feedback verbosity (persisted) |
| `&dm DM_STYLE_TOGGLE 0` | Toggle FULL / ARROW style (US/UK only, persisted) |
| `&dm DM_ERASE_TOGGLE 0` | Toggle auto-erase on / off (persisted)     |

## Kconfig Options

### Core

| Option                | Default | Description                          |
| --------------------- | ------- | ------------------------------------ |
| `MAX_EVENTS`          | 64      | Largest single macro (press+release = 2 events) |
| `AVG_EVENTS_PER_SLOT` | 32      | Per-slot average the shared RAM pool is sized for (1-64) — see [The shared event pool](#the-shared-event-pool-ram-sizing) |
| `TAP_DELAY`           | 30      | ms between events during playback    |
| `ASSIGN_TIMEOUT`      | 10000   | ms before pending mode auto-cancels  |
| `PERSIST`             | y       | Enable NVS persistence               |
| `NVS_SLOTS`           | 8       | Persistent slots (0-16)              |
| `RAM_SLOTS`           | 8       | Temporary slots (0-48)               |

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

`FEEDBACK_AUTO_ERASE` is the **build-time gate and the default** — it must be `y` for the feature to be compiled in. Once compiled in, `DM_ERASE_TOGGLE` flips auto-erase on/off at runtime (persisted across reboots), starting from this Kconfig default. With `FEEDBACK_AUTO_ERASE=n` the erase code is left out entirely and the toggle has no effect.

### Locale

| Option      | Description                        |
| ----------- | ---------------------------------- |
| `LOCALE_US` | US QWERTY, full punctuation (default) |
| `LOCALE_UK` | UK QWERTY, full punctuation          |
| `LOCALE_DE` | German QWERTZ, plain mode          |
| `LOCALE_FR` | French AZERTY, plain mode          |

Feedback works by typing HID keycodes into the focused application. Punctuation characters like `[`, `]`, `:`, `'` occupy different physical keys on each keyboard layout — a US bracket keycode produces a different character on a German or French host. DE and FR locales therefore use plain mode (letters, digits, spaces only) to avoid garbled output. UK uses full punctuation with correct mappings for the 6 keys that differ from US (`"`, `@`, `#`, `~`, `\`, `|`). Letter and digit mappings are adjusted per locale (e.g. Y/Z swap for German QWERTZ, AZERTY positions for French).

#### Locale Feature Matrix

| Feature | US | UK | DE | FR |
| ------- | -- | -- | -- | -- |
| FULL style punctuation (`[DM SAVED N3]`) | Yes | Yes | No (plain) | No (plain) |
| ARROW style | Yes | Yes | No | No |
| Preview rendering (printable chars) | Accurate | US layout assumed | US layout assumed | US layout assumed |
| Feedback level adjustment | Yes | Yes | Yes | Yes |
| Auto-erase | Yes | Yes | Yes | Yes |
| Status output | Full | Full | Plain | Plain |

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

## Firmware Size

> **🚧 Placeholder — numbers to be measured.** The flash/RAM figures below are not yet
> filled in. They will be measured per configuration on a representative target
> (e.g. nice!nano v2 / nRF52840) and reported here so you can estimate the module's
> cost before building. The table structure and the switches that drive it are final;
> only the measured values are pending.

The module's footprint is driven almost entirely by a handful of **compile-time**
switches that include or exclude whole subsystems. Runtime knobs (feedback level,
style, locale selection at runtime) do not change firmware size — only the build-time
selections below do.

### What each switch adds

| Switch | Default | Code it includes when on | Relative flash impact |
| --- | --- | --- | --- |
| **Typed feedback** (`FEEDBACK_*` > OFF, or any `STATUS_*` > OFF) | Verbose | The feedback pump, message builder, `dm_render`, the active locale table, ring + preview streaming | _TBD_ (largest single lever) |
| `EVENTS` | n | Event notifications, the query API, preview mode | _TBD_ |
| `PERSIST` | y | The full NVS storage backend (`dm_nvs`): async work queue, serialization, settings handlers | _TBD_ |
| `FEEDBACK_AUTO_ERASE` | n | The auto-erase scheduler + backspace batching. Also the build-time **default** for the runtime `DM_ERASE_TOGGLE` — when this is off the erase code is compiled out entirely, so the toggle has nothing to switch. | _TBD_ (small) |
| `TEST_RELOAD` | n | Test-only reload command — **do not enable in production** | _TBD_ (test only) |

Notes for when the figures land:
- **Typed feedback is the dominant lever.** Turning it fully off (`FEEDBACK_OFF` *and*
  `STATUS_OFF`) compiles out the pump, renderer, and locale tables together — the
  single biggest saving. With it off, the module is just the state machine + slot
  storage (+ NVS if `PERSIST`).
- **Locale choice does not change size** meaningfully — exactly one `static const`
  locale table is linked regardless of which locale you pick; selecting a different
  locale swaps the table, it doesn't add one.
- `MAX_EVENTS` / `AVG_EVENTS_PER_SLOT` / slot counts affect **RAM**, not flash (they
  size the pool and buffers — see [The shared event pool](#the-shared-event-pool-ram-sizing)).

### Representative combinations

The combinations worth quoting (each a row once measured: flash Δ vs. baseline, RAM Δ):

| Configuration | Feedback | EVENTS | PERSIST | Flash | RAM |
| --- | --- | --- | --- | --- | --- |
| **Minimal** (record/play only, RAM slots) | off | n | n | _TBD_ | _TBD_ |
| **Persistent, no feedback** | off | n | y | _TBD_ | _TBD_ |
| **Feedback, no persistence** | verbose | n | n | _TBD_ | _TBD_ |
| **Default** | verbose | n | y | _TBD_ | _TBD_ |
| **Full** (widgets + feedback + persistence) | verbose | y | y | _TBD_ | _TBD_ |

Baseline = ZMK firmware without the module. Figures will be stated as the delta the
module adds, so they remain comparable across ZMK versions.

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

- **RAM:** all slots share **one event pool** sized `AVG_EVENTS_PER_SLOT × (NVS_SLOTS + RAM_SLOTS)` events — not a full worst-case buffer per slot. A slot's events live in the pool only while it holds a macro; empty slots cost almost nothing (just a small descriptor). See [The shared event pool](#the-shared-event-pool-ram-sizing) below for how to size it. RAM slot contents are lost on reboot; NVS slots are loaded back from flash into the pool on boot.
- **NVS (flash):** only written when you save to an NVS slot — `8` byte header + `8` bytes × event count (not padded to `MAX_EVENTS`). Each slot is its own flash record (no pool, no compaction); empty slots use no flash entry. A 2-event macro writes 24 bytes; a full `MAX_EVENTS` macro writes `8 + MAX_EVENTS × 8` bytes (~520 B at the default 64). Shares ZMK settings partition; format version in header — incompatible upgrades clear saved macros.

### The shared event pool (RAM sizing)

All macro slots — NVS-backed and RAM-only alike — draw their event storage from **one shared pool in RAM**, rather than each slot reserving its own worst-case buffer. This is the key knob for trading RAM against capacity.

**How it's sized.** The pool holds:

```
AVG_EVENTS_PER_SLOT × (NVS_SLOTS + RAM_SLOTS)   events
```

Each event is 8 bytes, so the pool is `AVG_EVENTS_PER_SLOT × total_slots × 8` bytes. At the defaults (`AVG=32`, 8 NVS + 8 RAM = 16 slots) that's `32 × 16 × 8 ≈ 4 KB` — versus ~8.8 KB if every slot reserved a full 64-event buffer.

**How it behaves.** A macro's events occupy a contiguous run in the pool. A single macro may be **longer than the average** — up to `MAX_EVENTS` — as long as the *total* recorded across all slots stays within the pool. So a few long macros simply leave less room for the rest; you are budgeting a shared total, not a per-slot cap. Deleting or moving a macro frees its space back to the pool (reclaimed lazily — the freed space is compacted back in the next time you save a macro), so space is never permanently lost to fragmentation.

**The two knobs and what each does:**

| Setting | Default | What it controls | Effect of raising it |
| --- | --- | --- | --- |
| `MAX_EVENTS` | 64 | The largest a **single** macro can be, and the size of the in-progress recording buffer. | A longer individual macro is allowed; the recording buffer grows by `MAX_EVENTS × 8` bytes. Does **not** change the pool size. |
| `AVG_EVENTS_PER_SLOT` | 32 | The **per-slot average** the shared pool is budgeted for — i.e. the pool's total size. | The pool grows, so more total events can be stored across all slots at once. Costs `total_slots × 8` bytes of RAM per unit. |

**The rule that ties them together:** `MAX_EVENTS ≤ AVG_EVENTS_PER_SLOT × total_slots` (a single macro can never be allowed to exceed the whole pool — this is a build-time assertion).

**How to choose:**

- **Most macros similar length:** set `AVG_EVENTS_PER_SLOT` near your typical macro length and `MAX_EVENTS` to your longest. The pool comfortably holds everything; you save RAM versus worst-case.
- **A few long macros, many short ones:** keep `AVG` modest (the average you actually expect) and `MAX_EVENTS` high for the outliers. The long ones borrow space the short ones don't use. Watch for `[DM ... FULL]` (pool exhausted) — if you hit it often, raise `AVG`.
- **Guarantee every slot can be full at once (no sharing):** set `AVG_EVENTS_PER_SLOT = MAX_EVENTS`. Now the pool equals the old per-slot worst case — every slot can independently hold a maximum macro, at the cost of the extra RAM (no pool savings).
- **Tight on RAM:** lower `AVG_EVENTS_PER_SLOT` and/or reduce slot counts. The pool shrinks linearly; you trade total capacity for memory.

> **Pool exhausted vs. recording buffer full — two different limits, two different messages.** `MAX_EVENTS` is hit *while recording* (the in-progress buffer fills) — `[DM FULL]`, the overflow message. The pool is exhausted *at save/assign time* (no room for the finished macro across all slots) — the assign is rejected with the slot-full message (`[DM SLOT N0 FULL]`), the slot stays empty, and the macros already stored are untouched. Raising `MAX_EVENTS` fixes the first; raising `AVG_EVENTS_PER_SLOT` fixes the second.

### Flash Wear

NVS slots write to flash on every save and delete. ZMK's NVS implementation includes wear leveling, but flash has finite write endurance (typically 10,000+ cycles). For macros you change frequently, prefer RAM slots and promote to NVS only once stable.

For perspective, reflashing firmware writes the entire firmware partition (hundreds of KB) which also wears flash. Saving a dynamic macro to NVS writes only ~520 bytes — far less wear than reflashing the firmware just to add a single static macro.

### Feedback

Typed feedback goes to the focused application. Use a text editor when testing status output.

### Split Keyboards

Runs on central half only. Both halves' keystrokes are captured during recording.

### Troubleshooting

| Symptom | Cause | Fix |
| ------- | ----- | --- |
| `[DM FULL]` during recording | Recording buffer reached MAX_EVENTS | Increase `MAX_EVENTS` or record a shorter sequence. The partial recording can still be saved. |
| `[DM SLOT N0 FULL]` when assigning | Shared event pool exhausted — no room for this macro across all slots | Increase `AVG_EVENTS_PER_SLOT` (grows the pool), or free space by deleting a stored macro. Slots already saved are unaffected. See [The shared event pool](#the-shared-event-pool-ram-sizing). |
| `[DM SAVE FAILED N0]` | NVS write error | Check flash health. Settings partition may be full — reduce NVS_SLOTS or MAX_EVENTS. |
| `[DM SAVE QUEUE FULL N0]` | Too many storage operations queued | Wait a moment and retry. Occurs when rapidly saving/deleting multiple NVS slots. |
| Slot shows occupied but was deleted | NVS delete still in progress | The slot is marked pending-delete and will clear shortly. It cannot be played or assigned during this time. |
| Feedback not appearing | Feedback level set to OFF | Press `DM_FEEDBACK_INC` to raise the level, or set `FEEDBACK_VERBOSE` in .conf. |
| Wrong characters in feedback | Non-US locale with US punctuation | Non-US locales use plain mode. Set `LOCALE_US` for full punctuation output. |
| Macro plays wrong keys | Recorded on different layer/layout | Macros record HID keycodes, not physical positions. Replay on the same layout used during recording. |

### Compatibility

The module version tracks ZMK's: the **major.minor** matches the ZMK release
line it targets (so `v0.3.x` works with ZMK `0.3`), while the **patch** is the
module's own. Pin both to the same major.minor (see [Setup](#setup)). The `main`
branch tracks ZMK `main` and may be unstable — prefer a tagged release. Tested
with Zephyr 4.1+.

### Stability

These form the module's compatibility contract and are treated as append-only —
existing values are never renumbered or reordered:

- **Binding command IDs** (`dt-bindings/zmk/dynamic_macros.h`) — compiled into
  your keymap.
- **Event types** (`zmk_dynamic_macro_event_type`) — compiled into display
  widgets; new events are only appended.
- **On-flash format** — versioned in each saved slot's header. An incompatible
  format change is a deliberate, breaking event that clears saved macros: the
  module detects the old version and discards it rather than misreading it.

## License

This module is released under the [MIT License](LICENSE).
