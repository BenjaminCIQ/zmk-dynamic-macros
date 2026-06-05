# Robustness & Pre-Publication Review

**Date:** 2026-06-05
**Scope:** Full module review ahead of public release — correctness, concurrency,
public-contract stability, and packaging. Goal: surface anything that is
cheaper to fix *now* than after users depend on it.

## Overall assessment

The module is in good shape and close to publication-ready. The architecture is
sound: a clear state machine, a documented two-thread model (system work queue +
dedicated storage work queue) synchronized through atomics and a generation
counter, a streaming ring-buffer feedback pump, careful locale/punctuation
handling, and a strong snapshot-based `native_sim` test suite with CI.

Nothing here is an emergency. The findings below are mostly about **public
contracts** (binding IDs, the event enum, the on-flash format, the release tag)
that become expensive to change once people have saved macros or written display
widgets, plus one genuine behavior gap and one concurrency inconsistency worth
tightening. Severity is rated for a *to-be-publicized* module.

---

## Findings

### 1. Empty recording can be "saved"; documented `ERROR_NO_RECORDING` event is never emitted — **Medium / ✅ DONE**

> **Resolved:** `cmd_stop` now discards a zero-event recording, emits
> `ERROR_NO_RECORDING`, types a `[DM NO REC]` / `?*` message, and returns to
> `IDLE` (never `PENDING_ASSIGN`). Regression test: `tests/events/rec_stop_empty`.


`ZMK_DYNAMIC_MACRO_ERROR_NO_RECORDING` is declared in the public event header
(`dynamic_macro_state_changed.h:33`) and documented in `docs/event-api.md:67`
as *"Stop pressed with no recording"*, but it is **never raised** anywhere in the
code, and there is no guard against assigning an empty buffer:

- `cmd_stop` (`behavior_dynamic_macro.c:471`) only checks `state == RECORDING`. A
  `REC` → `STOP` with zero keystrokes transitions straight to `PENDING_ASSIGN`.
- `cmd_slot` / `PENDING_ASSIGN` (`behavior_dynamic_macro.c:542`) then `memcpy`s a
  zero-event `recording_buffer` into the slot, types `[DM SAVED N0: '']`, and
  `dm_save_slot()` writes a header-only entry to NVS.

So the user gets a misleading "SAVED" confirmation and a wasted flash write for a
macro that `slot_is_empty()` will immediately report as empty. Because the event
is part of the **public API surface** (header + docs), this is a
contract mismatch worth closing before release.

**Fix:** guard in `cmd_stop`: if `recording_buffer.event_count == 0`, emit the
`ZMK_DYNAMIC_MACRO_ERROR_NO_RECORDING` event, give an error-level feedback
message, and return to `IDLE` instead of `PENDING_ASSIGN`. Add a `core` test
(`rec_stop_empty`) pinning the new behavior. (Alternative/defense-in-depth: also
reject an empty buffer in the `PENDING_ASSIGN` branch.)

---

### 2. Deferred delete/delete-failed feedback is not state-guarded at execution time — **Low / ✅ DONE**

> **Resolved:** `dm_feedback_deleted` now proceeds only when `IDLE` (deferred
> path) or `DELETE_PENDING` (RAM path); `dm_feedback_delete_failed` mirrors
> `dm_feedback_save_failed`'s `state == IDLE` guard. The feedback-OFF stubs were
> hardened the same way. The four storage callbacks are now symmetric.


The save-side deferred callbacks self-guard:
`dm_feedback_save_failed` and `dm_feedback_save_queue_full` both bail on
`data->state != DM_STATE_IDLE` (`dm_feedback.c:1026, 1043`). The delete-side
callbacks (`dm_feedback_deleted`, `dm_feedback_delete_failed`) do **not** — they
rely solely on the storage thread's pre-check of `op.data->state == DM_STATE_IDLE`
before `schedule_deferred_fb(...)` (`dm_storage.c:219, 229`).

That pre-check is a cross-thread TOCTOU: the storage thread reads `data->state`
without synchronization, and the deferred handler re-enters the feedback state
machine (`fb_reset` + `start_feedback`) when it runs on the system work queue. In
practice this is safe today because ZMK's system work queue is cooperative and
single-core, so a late `[DM DEL N0]` is serialized ahead of any subsequent
command rather than clobbering, e.g., an in-progress `RECORDING`. But the
behavior is **fragile and inconsistent** with the save side, and would become a
real race under a preemptible system work queue or SMP.

**Fix:** make `dm_feedback_deleted` / `dm_feedback_delete_failed` re-validate
state at entry the same way the save callbacks do (allow `IDLE`; the synchronous
RAM-delete path calls `dm_feedback_deleted` while still in `DELETE_PENDING`, so
the guard must permit `IDLE || DELETE_PENDING`). This is small, removes the
reliance on cooperative scheduling, and makes the four storage callbacks
symmetric.

---

### 3. No release tag; README pins `revision: main` — **Medium / before publicizing**

`VERSION` reads `v0.3.0` but there are **no git tags** in the repo, and the
README setup snippet pins downstream users to `revision: main`
(`README.md:36`). For a module you're about to publicize, this is the single
biggest "hard to change later" item: every downstream `west.yml` tracking `main`
will pick up breaking changes (storage-format bumps, binding-ID changes)
silently on their next firmware build.

**Fix:**
- Cut an annotated git tag (`v0.3.0`) matching `VERSION`.
- Update the README to recommend pinning `revision: v0.3.0` (keep a note that
  `main` is the development branch).
- Adopt a short, documented versioning policy (below).

---

### 4. Public-contract stability policy — **Medium / before publicizing**

Three things become compatibility contracts the moment users depend on the
module. Worth an explicit, documented policy so future changes are deliberate:

- **Binding command IDs** (`include/dt-bindings/zmk/dynamic_macros.h`,
  `DM_REC=0 … DM_TEST_RELOAD=12`) are compiled into user keymaps. Treat as
  append-only; never renumber.
- **Event enum order** (`zmk_dynamic_macro_event_type`) is compiled into widget
  code. Treat as append-only. (Note finding #1 affects `ERROR_NO_RECORDING`,
  which currently sits mid-enum but unused — wire it up *before* release so its
  ordinal is locked in by real users rather than changed later.)
- **On-flash format** — `DM_STORAGE_VERSION 0xD1` plus the 8-byte header and
  8-byte `dm_event` layout. The version byte + "unknown version clears the slot"
  path (`dm_storage.c:402`) is good forward-thinking; just document that bumping
  the version wipes saved macros so it's a conscious decision.

**Fix:** add a short "Compatibility & Versioning" section to the README (or a
`COMPATIBILITY.md`) stating these three surfaces are stable/append-only and that
SemVer-minor = additive, SemVer-major = format/ID break.

---

### 5. Copyright attribution is inconsistent — **Low / before publicizing**

`LICENSE` is `Copyright (c) 2026 Benjamin H`, but every source/header file
carries `Copyright (c) 2026 The ZMK Contributors`. That header is the
conventional ZMK-core boilerplate and is fine to keep, but the mismatch looks
accidental for a personally-authored, publicized module.

**Fix:** decide on one attribution and make the file headers and `LICENSE`
consistent (either keep "The ZMK Contributors" in both, or use your name/handle
in both).

---

### 6. Missing repo hygiene files for a public project — **Low / optional**

No `CHANGELOG.md` or `CONTRIBUTING.md`. For a project about to attract users and
PRs, a minimal CHANGELOG (anchored to the new tag) and a short CONTRIBUTING note
(how to run `west test`, snapshot-update workflow, coding style) materially
reduce maintenance friction. The existing `tests/README.md` already covers much
of the testing story and can be linked.

---

### 7. Storage thread sizing & queue depth — **Low / verify, optional**

- `DM_STORAGE_STACK_SIZE = 1024` (`dm_storage.c:55`). The handler keeps the large
  buffers static (good), but `settings_save_one`/NVS write paths can be
  stack-hungry depending on the flash backend. Worth a one-time
  `CONFIG_THREAD_ANALYZER` / high-water-mark check on real hardware before
  declaring it safe across boards.
- `DM_STORAGE_QUEUE_LEN = 4` (`dm_storage.c:56`). Rapid multi-slot save/delete
  bursts surface `[DM SAVE QUEUE FULL]` (already documented in the README
  troubleshooting table). Acceptable, but consider exposing the depth as a
  Kconfig if users report hitting it.

---

### 8. Multi-instance behavior is implicitly single-instance — **Low / document**

The `dtsi` ships a single `dm` node, and the design assumes one:
`get_first_dm_data()` (query API) always uses `dm_devices[0]`
(`behavior_dynamic_macro.c:872`), and the recording listener's
`suppress_recording` check is **global** across all instances
(`behavior_dynamic_macro.c:769-775`) — a second instance playing back would
suppress recording on the first. This is fine as a documented constraint, but
should be stated so nobody instantiates two nodes and hits surprising behavior.

**Fix:** one line in the README/bindings doc: "A single dynamic-macro instance is
supported; do not define multiple `zmk,behavior-dynamic-macro` nodes."

---

### 9. Minor / informational

- **Keycode truncation:** recording narrows `ev->keycode` (u32) to `uint16_t`
  (`behavior_dynamic_macro.c:799`). Safe for keyboard/consumer/button usage IDs
  (all ≤ 0xFFFF), but a one-line comment or `__ASSERT` documenting the assumption
  would prevent a future surprise if a wider usage page is ever recorded.
- **`status_mode` excluded from auto-erase** is intentional and documented; no
  action.

---

## Suggested implementation order

1. **#1** empty-recording guard + emit `ERROR_NO_RECORDING` + new test
   *(locks the public event ordinal before release — do first).*
2. **#2** symmetric state guards on delete-side deferred feedback.
3. **#5** copyright alignment.
4. **#3 / #4** tag `v0.3.0`, switch README to pinned revision, add
   Compatibility/Versioning section.
5. **#6** CHANGELOG + CONTRIBUTING.
6. **#7** on-hardware stack/queue sanity check.
7. **#8 / #9** doc notes and a clarifying comment.

Items 1–4 are the ones genuinely worth doing **before** wide adoption. 5–9 are
quality-of-life and can follow.

## What I deliberately did *not* flag as needing change

- The two-thread storage model, generation/`pending_delete` reconciliation, and
  the delete-then-save FIFO ordering on slot reuse — reviewed and sound.
- The streaming ring-buffer feedback pump and overflow/drain handling.
- Bounds handling in `dm_get_preview_string` and the settings parse/validate path
  (version, `event_count > MAX_EVENTS`, short-read checks).
- Locale/punctuation matrix and the plain-mode fallback for DE/FR.
- Build-time keymap validation (`BUILD_ASSERT` command/param2 checks).
