# ADR-0002: Per-instance interfaces, single-instance internals

- **Status:** Accepted
- **Date:** 2026-06-09

## Decision

The behavior follows ZMK's per-instance convention at every **interface** — state
lives in `dev->data`, and module interfaces thread the instance (`slot_store *s`,
`dm_machine *m`), exactly as `behavior_macro`, `behavior_hold_tap`, and
`behavior_caps_word` do. But the module is hard single-instance
(`BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(...) <= 1)`), and we deliberately **do not pay
runtime or RAM cost to make internals multi-instance-correct** when the assert proves
a second instance never exists.

Three tiers:

1. **Interfaces — always per-instance.** Every public function takes the instance
   (via `dev->data` or an explicit handle). Non-negotiable; it is free (a pointer
   parameter) and it is the ZMK convention.
2. **Behavior-owned state — always per-instance, never shared.** Slots, machine state,
   recording buffer, feedback ring all live in `dev->data`. Also free; stays correct.
3. **Single-instance internals — may assume one instance.** An implementation may
   assume the single instance **only when** (a) true multi-instance correctness would
   add state or branches, **and** (b) the `<=1` assert guarantees the assumption. Each
   such site is commented `/* single-instance: safe under BUILD_ASSERT(<=1) */`.

Tier-3 members (the complete list):

- **The storage layer in its entirety** (`dm_nvs`): one file-scoped work queue, one
  message queue, one save/load buffer, shared across instances. Per-instance work
  queues would cost a kernel stack each — real RAM for unreachable genericity. The
  per-instance `slot_store` interface calls *into* this single-instance backend.
- **Query-API device resolution** (`dm_get_*`): widgets call with no device handle, so
  the projection resolves `dm_devices[0]` directly. ZMK has no convention here — this
  query surface is novel — so the singleton is anchored to the assert, beside the
  accessor.
- **Listener recording-suppression:** `dm_event_listener` bubbles if *any* instance is
  suppressing. This cross-talks between hypothetical instances; harmless under `<=1`.
  We keep the `dm_devices[]` loop shape but do not add per-instance suppression routing.

## Why this is surprising (and thus recorded)

A reader sees `dm_devices[]` iteration and `slot_store *` handles and reasonably infers
full multi-instance support. They will then find storage statics and suppression
cross-talk that only the `<=1` assert makes safe. This ADR is the answer to "why didn't
they finish the multi-instance work?" — it was a deliberate footprint trade-off, not an
oversight.

## Consequences

- `dm_nvs` does not thread an instance handle into its work queue; it stays file-scoped.
  This simplifies the `slot_store`/`dm_nvs` split in the redesign.
- The `BUILD_ASSERT(<=1)` is load-bearing. Removing it to support multiple instances is
  not a config change — it requires auditing every tier-3 site.
- The over-broad assert message in the current code (claiming recording suppression is
  "global across instances") is replaced by this scoped, per-site reasoning.
