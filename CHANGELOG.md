# Changelog

All notable changes to this module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows the module's hybrid scheme: the **major.minor** tracks the ZMK
release line it targets, while the **patch** is the module's own — see
[docs/versioning-and-ci.md](docs/versioning-and-ci.md).

## [Unreleased]

Initial release, targeting ZMK 0.3.

### Added

- Dynamic macro recording and playback with NVS-persistent slots, RAM-only
  slots, and move / chain / delete / status operations.
- Typed feedback: US/UK/DE/FR locales, FULL and ARROW styles, runtime
  feedback-level and style toggles, and optional auto-erase.
- Optional event and query API (`CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS`) for
  display widgets, including preview mode.
- `ERROR_NO_RECORDING` event, emitted when STOP is pressed with nothing recorded
  (previously declared but never raised).
- `tests/events/rec_stop_empty` regression test.

### Fixed

- Pressing STOP with an empty recording no longer enters the assign state, so an
  empty macro can no longer be saved — it reports `NO REC` and returns to idle.
  This avoids misleading `SAVED` feedback and a wasted NVS write.
- Deferred NVS delete / delete-failed feedback can no longer hijack an unrelated
  in-progress operation; the storage feedback callbacks are now state-guarded and
  symmetric.
- `feedback_levels` test now actually records its keystroke (it was typed during
  feedback suppression and silently dropped), so it validates real non-empty
  save previews instead of empty ones.

[Unreleased]: https://github.com/BenjaminCIQ/zmk-dynamic-macros/commits/main
