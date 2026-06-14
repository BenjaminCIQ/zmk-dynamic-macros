# Architecture-redesign review — verified findings & implementation plan

Reviewed branch: `docs/architecture-redesign` (HEAD `17250d6`), against `main`.
Method: read-through of the full modular stack + a "pseudo-fix" trace of each
finding (execution simulated by hand, cross-checked against the host tests).
Host suite at review time: **114/114 unit tests pass**.

Each item below records: the verdict (CONFIRMED / INTENTIONAL / DOC), the exact
trace that confirms it, and the fix **as it should actually be implemented**
(adjusted from the first-pass suggestions where the trace proved them wrong).

---

## 1. Phantom `dm_machine_typing_finished` after an auto-erase cancel — CONFIRMED (med)

### What happens
The auto-erase emitter drives `emit_timer` as a self-rescheduling one-shot.
`emit_iteration` re-arms the timer at its tail (`dm_feedback_pump.c:269`) after
every emitted half-keystroke, including the last one; the *terminal* drain is
reached on the next fire, where `ring_empty` is true and `emit_iteration` calls
`on_typing_drained` and returns **before** re-arming. So during any active
emission there is always exactly one pending `emit_timer` fire in flight.

`cancel_erase` (`dm_feedback_pump.c:458`) aborts a mid-emission erase by draining
the ring (`ring_head = ring_tail`), clearing `erase_in_progress`, dropping
suppression, and calling `dm_machine_erase_cancel`. It does **not** stop
`emit_timer` (there is no `k_timer_stop` anywhere in the module). That pending
fire still lands:

- `emit_iteration` → `ring_empty` && `!preview_pending` → `on_typing_drained` (`:187`)
- `erase_in_progress` is now false, `status_mode` false →
  it unconditionally calls `dm_machine_typing_finished` (`:222`).

`typing_finished` writes `state = return_state`. That is harmless **unless** a
command ran between the cancel and the stale fire that changed `state` without
parking a `return_state` and without starting a new emission (which would
reschedule the timer and override the stale one).

### Vulnerable commands (traced)
`on_keymap_binding_pressed` calls `dm_feedback_pump_cancel_erase` unconditionally
at its top (`behavior_dynamic_macro.c:493`) for **every** DM binding, then
dispatches. Commands whose transition neither `speak`s (→ `start_timer`) nor
otherwise re-arms `emit_timer` are left exposed to the stale fire:

- `do_delete_mode` (`dm_machine.c:161`) → sets `DELETE_PENDING`, no speak.
  → phantom reverts to `return_state` (typically IDLE): **delete mode silently lost.**
- `do_preview` (`dm_machine.c:185`) → sets `PREVIEW_PENDING`, no speak.
  → **preview mode silently lost.**
- `slot_play` (`dm_machine.c:358`) → sets `PLAYING`, `store_mark_playing` (which
  starts the shell's *playback_timer*, a different timer), no speak.
  → phantom reverts `PLAYING`→IDLE while playback is still emitting:
  **machine/playback desync** (machine reads IDLE mid-playback; a subsequent
  command can be accepted while keystrokes are still being replayed).

All other ALLOWED transitions end in `speak()` (→ `start_timer`, which reschedules
`emit_timer` and overrides the stale one) or in an inline `typing_finished` from
an already-settled IDLE (harmless self-assignment), so they are safe.

### Reproduction (preconditions)
- `FEEDBACK_AUTO_ERASE` enabled (off by default — limits exposure).
- After a feedback message that triggers auto-erase (e.g. SAVED/DELETED/MOVED,
  return-state IDLE), press `DM_DEL`, `DM_PREVIEW`, or a slot key **while the
  backspaces are still emitting**. Window ≈ `char_count × 2 × TAP_DELAY` ms.

### Why the first-pass fix ("just `k_timer_stop`") is INSUFFICIENT
`on_keymap_binding_pressed` and `emit_work` both run on the system workqueue, so
they serialize — but the timer ISR can submit `emit_work` *before* the cancel
runs. In that ordering the work item is already queued; `k_timer_stop` in
`cancel_erase` cannot un-queue it, so the stale `emit_iteration` still runs.
The robust fix must guard the **iteration**, not just the timer.

### Fix to implement (adjusted)
Add an "actively emitting" guard so a stale iteration after a cancel is inert.

1. `dm_feedback_pump_priv.h` — add to `struct dm_feedback`:
   ```c
   bool emit_active;   /* true while a message/erase is draining through emit_iteration */
   ```
2. `dm_feedback_pump.c`:
   - Top of `emit_iteration` (`:233`): `if (!f->emit_active) return;`
   - Set `f->emit_active = true;` immediately before `start_timer(f)` in
     `dm_feedback_speak` (`:330`) and in `erase_work_handler` (before `start_timer`, ~`:453`).
   - Clear `f->emit_active = false;` in `cancel_erase`'s in-progress branch (`:463`).
   - Clear `f->emit_active = false;` at the two terminal exits of
     `on_typing_drained` (the `erase_finished` return `:201` and the final
     `typing_finished` `:222`). Leave it **true** on the continuation paths
     (status `status_advance` success, erase batch reschedule) so streaming
     continues. (These terminal clears are hygiene — no timer is pending there —
     but keep the flag honest.)
   - Optional: also `k_timer_stop(&f->emit_timer)` in `cancel_erase` to avoid a
     needless wakeup. Not required for correctness once the guard is in place.

### Test to add (regression)
The pump is currently host-untested (Zephyr-coupled). Minimum viable coverage:
a native_sim case that enables auto-erase, types a feedback message, then issues
`DM_DEL` during the erase window and asserts the machine ends in `DELETE_PENDING`
(not IDLE). If pump-level host testing is out of scope, document the manual
repro in `tests/README.md`.

---

## 2. Dead `store_clear_playing` machine-callback — CONFIRMED (cleanup)

### Trace
`store_clear_playing` is declared in the vtable (`dm_machine.h:89`), wired
(`behavior_dynamic_macro.c:401` `cb_store_clear_playing`, assigned `:467`) and set
in the test fake (`test_machine.c:117,208`), but `dm_machine.c` **never calls
`cb->store_clear_playing`** (grep-confirmed). Playback completion is owned by the
shell: `playback_finish` calls `slot_store_clear_playing(&inst->store)` directly
(`behavior_dynamic_macro.c:279`). The store-level `slot_store_clear_playing` and
the machine-driven `store_mark_playing` (called by `slot_play`) stay; only the
machine *clear* indirection is vestigial. `test_machine.c:775` asserts
`mark_playing` only — nothing asserts `clear_playing`, so removal is test-safe.

### Fix to implement
- Remove `void (*store_clear_playing)(void *ctx);` from `dm_machine_callbacks`
  (`dm_machine.h:89`).
- Remove `cb_store_clear_playing` (`behavior_dynamic_macro.c:401-404`) and its
  assignment (`:467`).
- Remove `cb_clear_playing` (`test_machine.c:117`) and `.store_clear_playing`
  (`test_machine.c:208`).
- Keep `slot_store_clear_playing` (still used by `playback_finish` and
  `test_slot_store.c`).

---

## 3. Dead observe-call in `do_rec` — CONFIRMED (cleanup)

### Trace
`dm_machine.c:126`: `(void)m->cb->store_draft_count(m->cb->ctx);` — the result is
discarded; the very next line is `store_draft_reset`. `store_draft_count` maps to
`slot_store_draft_count`, a pure getter with no side effect, and the test fake
`cb_draft_count` (`test_machine.c:113`) is an un-logged getter, so no test asserts
it is called. Removing the line is behaviourally and test-neutral.

### Fix to implement
Delete `dm_machine.c:126` (and fold the now-redundant comment into the
`store_draft_reset` comment).

---

## 4. `DM_CMD_TEST_RELOAD` machine path "unreachable" — INTENTIONAL, do not change

### Re-evaluation (reversed from first pass)
`DM_CMD_TEST_RELOAD` is never produced by the shell (the `DM_TEST_RELOAD` binding
is handled directly via `dm_nvs_test_reload()` at `behavior_dynamic_macro.c:556`).
But the enum value, its IDLE-only legality column, and the `case … return DM_OK`
(`dm_machine.c:411`, commented "dispatched by dm_nvs, not a transition") are
**deliberate defensive scaffolding**, and the legality sweep test explicitly
special-cases it (`test_machine.c:366`, `legality_test_reload_passes_gate` `:388`).
Removing it would churn the enum, the `legality[][]` matrix, the test's
`expected_legality[][]`, and the dedicated test for net-zero value.

**Verdict: leave as-is.** Not a defect.

---

## 5. Status/SAVED preview suffix is not backpressured — CONFIRMED (minor/cosmetic)

### Trace
In `emit_iteration` the preview suffix is emitted inline, inside the
`if (done)` block of the refill (`dm_feedback_pump.c:240-246`), via
`dm_feedback_build_preview_suffix` → `emit_char`, which silently drops when the
ring is full (`sink_emit` `:64`). `dm_feedback_build_preview` returns `done` only
after the last preview unit emits successfully, which can leave the 64-slot ring
(`DM_FB_RING_SIZE`) nearly full. If a slot's rendered preview length lands in the
~57–62 range at completion, the suffix (`"' (N)\n"` for STATUS_SLOT, or
`preview_end + close` for SAVED) is partially dropped → a status line missing its
count/newline. Cosmetic, no crash; reachable only with `STATUS_DETAIL >=
USED_PREVIEW` (or a VERBOSE SAVED preview) and a long-enough slot.

### Why the first-pass fix ("add a space check") is incomplete
A space check would drop the suffix rather than malform it — still wrong. The
suffix needs to be emitted into a *fresh* ring once the preview has fully drained,
so it always fits (suffix ≤ ~10 chars « 63).

### Fix to implement (adjusted — restructure, don't space-check)
In `emit_iteration`, stop emitting the suffix inside the preview `if (done)`
block; only clear `preview_pending` there. Add a second refill pass that runs the
suffix once the preview is fully done and the ring is empty:

```c
if (ring_empty(f) && f->preview_pending) {
    /* ... existing preview walk ... */
    if (done) {
        f->preview_pending = false;     /* do NOT emit the suffix here */
    }
}
if (ring_empty(f) && f->suffix_pending && !f->preview_pending) {
    dm_fb_facts facts = gather_facts(f, &f->spec);
    dm_fb_sink  sink  = pump_sink(f);
    dm_feedback_build_preview_suffix(&f->spec, f->style, f->locale, &facts, &sink);
    f->suffix_pending = false;
}
```

This covers both the STATUS_SLOT and SAVED suffixes (same generic path). Existing
golden snapshots may need a refresh if any currently encode the truncated form;
verify against `tests/core/load_e2e` and the status cases.

---

## 6. Header comment "9 states × 14 commands" — DOC

`dm_machine.h:11`: there are **13** commands (`DM_CMD_REC..DM_CMD_OVERFLOW`;
`DM_CMD__COUNT == 13`), matching `legality[9][DM_CMD__COUNT]` and the test sweep.
Change "14" → "13".

---

## Suggested commit grouping (on `docs/architecture-redesign`)

1. **fix(pump): inert stale emit-iteration after auto-erase cancel** — item 1
   (+ regression test or documented repro).
2. **fix(pump): emit preview suffix in its own drained pass** — item 5
   (+ snapshot refresh if needed).
3. **refactor(machine): drop the unused store_clear_playing callback + do_rec
   observe call; fix command-count comment** — items 2, 3, 6.

Item 4 requires no change.

## Verified-solid (no action)
- `slot_store` arena packing/compaction, move dual-write ordering + rollback,
  `pending_delete`/generation staleness (white-box tested). The playing-slot
  guard in `slot_store_complete_delete` is defensive-only — the state machine
  makes `playing_slot == deleted idx` unreachable.
- `dm_nvs` header/version validation, slot-key parse bounds, async
  storage→completion→`deliver_async` flow.
- `dm_render`/`dm_feedback_build` locale tables are mutual inverses; `dm_query`
  shares the same renderer (no preview disagreement).
- Recording listener's effective-keystroke folding and bare-modifier deferral.
