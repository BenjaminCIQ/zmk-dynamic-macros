# Dynamic Macro Tests

Test suite for ZMK dynamic macro behavior using native_sim simulation.

## Test Cases

### Core

| Test | Description | Validates |
|------|-------------|-----------|
| `record_play` | Record A and play back | Basic record/assign/playback flow |
| `record_timeout` | Record then timeout without assign | Assign timeout cancellation |
| `play_empty` | Play unassigned slot | Empty slot handling |
| `delete` | Record, save, delete, verify empty | Delete functionality |
| `move` | Move macro between slots | Move mode functionality |
| `overflow` | Record >MAX_EVENTS | Buffer overflow handling |
| `chain` | Chain existing slot into new recording | Chain insert during recording |
| `nvs_roundtrip` | Record, save to NVS, simulate reboot, play back | NVS save/load data integrity |

### Feedback

| Test | Description | Validates |
|------|-------------|-----------|
| `record_play_feedback` | Record and play with typed feedback enabled | Feedback typing during record/assign/play |

### Events

| Test | Description | Validates |
|------|-------------|-----------|
| `record_play_events` | Record and play with event notifications enabled | Event API broadcasting |

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
