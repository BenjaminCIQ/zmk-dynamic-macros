# Handoff — continue the deep-module rewrite at step 7

You're continuing a **deep-module rewrite** of the `zmk-dynamic-macros` ZMK
behavior module. Branch: `docs/architecture-redesign`. The plan is
`docs/architecture-redesign.md` (read §§1–2.7, 3, 5 first) and the two ADRs in
`docs/adr/`. Read those before touching anything — they own the *why*, and the
decision log in §7 is binding (don't re-litigate).

## Method (do not violate)

**Parallel stack, one cut-over at the end.** The old shipping path —
`src/behaviors/behavior_dynamic_macro.c`, `src/dm_feedback.c`,
`src/dm_storage.c` — stays **untouched** and live. All new modules are built
beside it, reachable only from tests, until a single step-8 cut-over. Never
repoint an old call site mid-flight.

**Comment style:** code reads as if it always looked this way. No process
narration — no dates, step numbers, commit hashes, "ports X", "the old draft",
or `§N` doc cross-references in `src/`/`include/`. Durable *why*-rationale
stays. (An earlier session scrubbed these; match that.)

## What's done (all committed, parallel stack, green)

The **entire pure core** exists and is host-tested — **92/92** via
`tests/unit/run-host.ps1` (MSVC local) / `tests/unit/Makefile` (gcc, the CI
rail):

- `dm_render` (steps 1–2), `slot_store` (3), `dm_machine` (4),
  `dm_feedback_build` (5a — the pure `speak(spec)` message builder),
  `dm_query` (6a — pure preview projection).
- Every architectural decision (dual-write ordering, feedback→machine→state
  inversion via the machine's callback vtable + up-calls, message-builder
  collapse, honest stop-at-first-non-fit truncation) is proven by a C compiler.

## What's left — all Zephyr-coupled, converges on step 7

Cannot run in the host loop; must be written *with* the `native_sim` e2e parity
harness that verifies it against the old path. Do not land Zephyr code
unexercised. The pieces (per the §5 progress block):

- **5b** — feedback pump: ring + `k_timer`/`k_work` emit loop +
  `raise_zmk_keycode_state_changed` + the `speak(spec)` ritual
  (gate→reset→build→start) + erase scheduler + status-slot continuation, all
  calling *into* the step-4 machine up-calls (`dm_machine_typing_finished`,
  `deliver_async`, `erase_due`/`erase_cancel`). Drives `dm_feedback_build`.
- **6b** — `dm_events` shell: resolve the single instance (`BUILD_ASSERT(<=1)`
  in one place), hold the `slot_store *`, map the machine's `notify` event codes
  → `zmk_dynamic_macro_state_changed` and raise, count queries over
  `slot_store`, wrap `dm_query`.
- **`dm_nvs`** wiring: sink adapter over file-scoped storage, boot restore
  (`slot_store_load`/`dm_feedback_restore_knobs`), `dm_nvs_save_knobs`, export
  read-back; `DM_TEST_RELOAD` via `slot_store_reset`.
- **new `behavior_dynamic_macro` shell** — the §5.1 step-7 **shell-inventory
  checklist** is the definition-of-done (keymap validation macros, metadata,
  param2 range checks, BUILD_ASSERTs, `suppress_recording` ownership, IDLE-only
  `DM_TEST_RELOAD`, `dm_nvs` init wiring).
- **`native_sim` e2e parity harness** (§5.2) — the gate; old-vs-new keycode-
  stream diff.

**Open seam to resolve in 5b:** in `dm_machine.c`, `do_knob()` parks state and
returns to IDLE but the knob *confirmation speech + persist* (level/style/erase,
the ARROW-on-plain-locale rule) was deferred to the shell/feedback, not given a
machine vtable slot. Confirm that division when wiring 5b.

## Before you start

Ask the user whether they can run `native_sim` to verify as you go (step 7 is
large and only the harness proves it). Also: four untracked items (`.claude/`,
`AGENTS.md`, `docs/agents/`, `log_files/`) predate this work and are
unaddressed — ask before touching. Commit only when the user asks.
