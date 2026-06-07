# Changelog

All notable changes to this module are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows the module's hybrid scheme: the **major.minor** tracks the ZMK
release line it targets, while the **patch** is the module's own — see
[docs/versioning-and-ci.md](docs/versioning-and-ci.md).

## [Unreleased]

## [0.3.1] - 2026-06-07

### Fixed

- Moving a macro between slots no longer risks resurrecting or duplicating it
  after a reboot. The move's two NVS operations (write destination, delete
  source) are now ordered so a full storage queue can only leave a benign
  transient duplicate, never lost data: the destination is persisted and
  checked first (rolled back with the source left intact on failure), and only
  then is the source cleared and deleted.
- A macro slot that is mid-playback is no longer zeroed in RAM when a queued NVS
  delete for that slot completes on the storage thread, avoiding a race with the
  playback pump.
- Slot previews from the query API (`dm_get_preview_string`) now render a
  printable key held with a non-shift modifier (e.g. Ctrl+A) as a `<LCTL+A>`
  token instead of a bare character, matching the live preview-typing path.
- Slot previews on UK-locale builds now map layout-specific punctuation
  correctly (e.g. `@`, `"`, `#`, `\`) instead of using US assumptions; keys that
  produce a non-ASCII glyph on the layout render as a token.

### Changed

- Pressing record while a finished-but-unassigned recording is awaiting slot
  assignment now logs the discard of that recording (behaviour unchanged; the
  recording was already discarded silently).
- NVS slot headers are built in an aligned local before serialization rather
  than written through a packed-struct pointer cast (no on-flash format change).

## [0.3.0] - 2026-06-06

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

[Unreleased]: https://github.com/BenjaminCIQ/zmk-dynamic-macros/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/BenjaminCIQ/zmk-dynamic-macros/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/BenjaminCIQ/zmk-dynamic-macros/releases/tag/v0.3.0
