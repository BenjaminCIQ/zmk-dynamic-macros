# Dynamic Macro Tests

Test suite for ZMK dynamic macro behavior using native_sim simulation.

## Test layers

Two test layers run in CI:

- **Host unit tests** (`tests/unit/`) — the pure core (`dm_render`, `slot_store`,
  `dm_machine`, `dm_feedback_build`, `dm_query`) compiled with plain `gcc` and a
  `ztest_shim.h`, run as a sub-second host binary. Test-first; each `ZTEST` pins one
  invariant. See `tests/unit/README.md`.
- **`native_sim` keymap-snapshot cases** (below) — full-firmware integration: a keymap
  of mock keypresses is run and the emitted keycode (and, where enabled, `dm_event`
  notification) stream is diffed against a recorded `keycode_events.snapshot`. The
  parallel `tests/parity/e2e/` tree re-runs each case against the modular stack
  (`NEW_STACK=y`) and asserts the same snapshot (regenerate with
  `tests/parity/e2e/generate.sh`).

## native_sim cases

### Core

| Test | Validates |
|------|-----------|
| `record_play` | Basic record → assign → playback flow |
| `record_timeout` | Pending-assign auto-cancels after the timeout |
| `record_while_playing` | REC is ignored while a slot is playing back |
| `stop_idle` | STP outside RECORDING is a no-op |
| `play_empty` | Playing an unassigned slot is rejected |
| `assign_occupied` | Assign onto an occupied slot is rejected, pending state preserved |
| `assign_cancel` | Pending assign cancelled by the timeout |
| `delete` | Record, save, delete (RAM), verify empty |
| `delete_empty` | Delete on an already-empty slot |
| `delete_cancel` | Delete mode cancelled by the timeout; slot survives |
| `delete_reuse` | Delete then re-assign the same slot |
| `move` | Move a macro to an empty destination slot |
| `move_occupied` | Move onto an occupied destination is rejected |
| `move_cancel` | Same-slot move is a cancel, not a rejection |
| `move_last_empty` | Move source/destination edge case |
| `chain` | Chain an existing slot into a new recording |
| `chain_no_room` | Chain rejected when it would overflow the draft |
| `overflow` | Recording past `MAX_EVENTS` → pending assign |
| `modifier_record` | Modifier chords (Shift/Ctrl + key) record and replay |
| `load_e2e` | Several RAM slots filled to capacity, loaded and played |
| `nvs_roundtrip` | Record → save to NVS → reload → play (save/load fidelity) |
| `multi_slot_nvs` | Three NVS slots survive a reload independently |
| `nvs_load` | NVS slots at max capacity reload with correct keycodes/mods |
| `nvs_delete_reload` | Delete an NVS slot, reload, verify it stays erased (delete durability) |

### Feedback (`FEEDBACK_*` typed output)

| Test | Validates |
|------|-----------|
| `record_play_feedback` | Feedback typing during record/assign/play |
| `feedback_levels` | Output gated by feedback level |
| `feedback_level_toggle` | `DM_FEEDBACK_INC`/`_DEC` adjust and confirm the level |
| `style_toggle` | `DM_STYLE_TOGGLE` switches FULL/ARROW grammar |
| `arrow_record_play` | ARROW-style feedback output |
| `status_accuracy` | STATUS key output (header + slot lines) |
| `erase_basic` | Auto-erase emits the right backspace count |
| `erase_toggle` | `DM_ERASE_TOGGLE` flips auto-erase and confirms |
| `erase_rec_stop` | Auto-erase across a record/stop sequence |

### Events (`EVENTS` notifications)

| Test | Validates |
|------|-----------|
| `record_play_events` | Notification stream for record/assign/play |
| `rec_stop_empty` | Empty recording discarded; `ERROR_NO_RECORDING` fires |
| `nvs_delete_events` | NVS delete raises `DELETED` exactly once, deferred to completion |

## Running Tests

From ZMK workspace with this module:

```bash
west test -T tests/dynamic_macro
```

Or run specific test:

```bash
west test -T tests/dynamic_macro/record_play
```

## Test Structure

Each test directory contains:
- `native_sim.keymap` - Test keymap with simulated key events
- `native_sim.conf` - Test configuration

## Notes

- Tests use `ZMK_MOCK_PRESS/RELEASE` macros to simulate key events at specific timestamps
- Feedback typing (if enabled) adds extra keycodes between operations
- Timer-based playback may require generous timing gaps in test sequences
