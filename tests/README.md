# Dynamic Macro Tests

Test suite for ZMK dynamic macro behavior using native_posix simulation.

## Test Cases

| Test | Description | Validates |
|------|-------------|-----------|
| `record_play` | Record A,B,C and play back | Basic record/assign/playback flow |
| `record_timeout` | Record then timeout without assign | Assign timeout cancellation |
| `play_empty` | Play unassigned slot | Empty slot handling |
| `delete` | Record, save, delete, verify empty | Delete functionality |
| `move` | Move macro between slots | Move mode functionality |
| `overflow` | Record >MAX_EVENTS | Buffer overflow handling |
| `nvs_roundtrip` | Record, save to NVS, simulate reboot, play back | NVS save/load data integrity |

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
- `native_posix_64.keymap` - Test keymap with simulated key events
- `events.patterns` - Expected log patterns to match
- `keycode_events.snapshot` - Expected HID keycode output

## Notes

- Tests use `ZMK_MOCK_PRESS/RELEASE` macros to simulate key events at specific timestamps
- Feedback typing (if enabled) adds extra keycodes between operations
- Timer-based playback may require generous timing gaps in test sequences
- Snapshots need to be captured from actual test runs and updated

## TODO

- [ ] Capture actual snapshots from test runs
- [ ] Add RAM slot tests (currently only NVS)
- [ ] Add modifier key recording tests
- [ ] Add multi-key sequence tests
- [ ] Test feedback typing output
- [x] Test NVS persistence across simulated reboot
