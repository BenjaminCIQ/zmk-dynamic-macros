# ADR-0001: Deep-module architecture for the dynamic macro behavior

- **Status:** Accepted
- **Date:** 2026-06-09
- **Deciders:** Benjamin H

## Context

The behavior is implemented in three translation units (`behavior_dynamic_macro.c`
~1090 lines, `dm_feedback.c` ~1590 lines, `dm_storage.c` ~595 lines) that share a
single mutable struct, `behavior_dynamic_macro_data`, by reference. That struct
holds ~40 fields spanning five unrelated concerns — slot store, state machine,
recorder, feedback/typing pump, and auto-erase scheduler — and all three files
write its fields directly.

Concrete symptoms:

- **`state` is edited in place at 49 sites** across two files; the legal-transition
  rules exist only as scattered conditionals. The hard-won fixes (move ordering,
  deferred-feedback-only-when-idle, REC-during-pending-assign) are transition rules
  with no home.
- **The slot-store dual-write invariant lives in the command handler.** `cmd_slot()`
  hand-orders "write dst → delete src" for crash safety (commit `a2865b3`); a caller
  could get it wrong because the ordering is visible to it.
- **The preview renderer is duplicated.** `dm_get_preview_string()` and
  `render_slot_contents_stream()` implement the same event-walk twice and are kept
  identical by a hand-written "so both agree" comment.
- **The locale/keycode mapping is a preprocessor thicket** (`#if DM_LOCALE == ...`),
  untestable per-locale and hard to read.

The module is intended to serve as a **showcase of clean ZMK behavior
architecture**, so the architecture itself is part of the deliverable.

## Decision

Rewrite around **deep modules** (Ousterhout's sense, adapted to "depth as leverage"):
small interfaces over substantial behavior, each module owning its own data, composed
by a thin top-level behavior shell. No shared God-struct; modules hold opaque handles.

Target modules:

| Module | Owns | Interface shape |
| --- | --- | --- |
| `behavior_dynamic_macro` | nothing; wiring | ZMK driver API; parse binding → dispatch command |
| `dm_machine` | `state`, timeouts, transition table | `dm_machine_command()`, `dm_machine_state()` |
| `slot_store` | `slots[]`, `pending_delete`, `slot_generation` | `assign / move / delete / get`; dual-write hidden |
| `dm_nvs` | settings serialization, async work queue | enqueue save/delete; calls back into store |
| `dm_render` | nothing (pure) | `dm_render_slot(view, locale, sink, cursor)` → ring or buffer |
| `dm_feedback` | ring, preview cursor, erase | message builder over `dm_render` + emit pump |
| `dm_events` | notification raising + query projection | `raise()`, read-only `dm_get_*` |

Binding decisions:

1. **`dm_machine` is the only writer of `state`.** All transitions go through it; the
   legal-transition table is `static const` and host-testable.
2. **`slot_store` owns the dual-write invariant internally.** Callers request
   `slot_store_move(src, dst)`; ordering and rollback are not visible to them.
3. **`dm_render` is pure** — `(events, locale, sink) → void`, no ZMK, no I/O. Two
   sinks (ring for live typing, char buffer for the query API) are the two adapters
   that justify the seam.
4. **Locale/keycode mappings become `static const` data tables**, one selected at
   link time — not `#if`-branched code.
5. **Full feature/config parity.** Every Kconfig option, locale, style, and feature
   survives. `PERSIST`/`EVENTS` stay as `#if` (they add/remove whole modules); the
   locale thicket does not.
6. **On a genuine clean-vs-footprint tie, clean wins** — but footprint is measured,
   not guessed, and the locale-table change is expected to be footprint-neutral or
   better.
7. **The pure core is built test-first (strict TDD).** `dm_render`, `dm_machine`, and
   `slot_store` are Zephyr-free, so their tests are written before the implementation
   and drive interface design. The integration layer (`dm_feedback` pump, `dm_events`
   wiring, behavior shell) is *not* TDD — the existing `native_sim` snapshot suite is
   its green-keeping safety net. Unit tests run **both** as Ztest under `west test`
   (one CI harness) and as a standalone host compile (sub-second local red-green loop,
   doubling as a decoupling proof). See the design doc §4.

## Invariant-porting checklist (must survive the rewrite)

Each is an existing fix encoding a real failure mode. The rewrite ports them as
documented module invariants, with a regression test where one is named.

- [ ] **Move dual-write ordering** (`a2865b3`) → `slot_store_move` internal contract.
- [ ] **Don't zero a playing slot on NVS delete completion** (`fe3689e`) →
      `slot_store` delete-completion checks playback ownership.
- [ ] **Generation-stamped async ops** ignore stale completions → `slot_store`/`dm_nvs`.
- [ ] **REC during pending-assign discards the unassigned take** (`539260c`) →
      `dm_machine` transition with a logged/observable discard.
- [ ] **Deferred feedback only speaks when IDLE** → `dm_machine_deliver_async()`
      suppression rule (the rule lives in the machine, not a `dm_feedback` entry guard;
      see redesign §2.7.3, landed test-first in step 4).
- [ ] **Aligned NVS header build** (`277f0c8`) → `dm_nvs` serialization.
- [x] **UK punctuation + Ctrl+printable → token** (`49c4f1a`, `86993af`) →
      `dm_render`, covered by host tests. *(Done: new pure `dm_render` module +
      red→green host tests `tests/unit/test_render.c`; `<LCTL+C>` and UK Shift+3→token
      asserted. Old walks still live; parity harness pending per redesign §5.2.)*
- [ ] **Single-instance assumption** stated in exactly one place (`dm_events`
      projection), not leaked across files.

## Consequences

**Positive**
- The transition table and the renderer become host-testable without flashing.
- The dual-write invariant cannot be violated by callers.
- The preview "both agree" invariant is enforced by the compiler, not by hand.
- The architecture reads as an intentional set of deep modules — the showcase goal.

**Negative / costs**
- Small footprint overhead from opaque handles and non-inlined accessors; to be
  measured, clawed back only where it demonstrably hurts.
- A real rewrite of working firmware carries regression risk to the encoded fixes;
  mitigated by the porting checklist and keeping the build green per step.

**Neutral**
- Single-instance support is retained, not expanded.

## Alternatives considered

- **Incremental deepening in place** (the four candidates from the architecture
  review) — lower risk, but leaves the God-struct and so leaves the root cause. Good
  if the goal were maintenance; rejected because the goal is a showcase rewrite.
- **Full feature simplification** (drop rarely-used locales/styles) — would shrink the
  surface, but rejected in favor of full parity.
