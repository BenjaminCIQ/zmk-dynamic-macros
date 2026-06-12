# Step 8 — Cut-over plan (the one and only switch)

Status: **proposed** · branch `docs/architecture-redesign` · author handoff for review

This is the single, isolated change that removes the legacy path and makes the
modular stack the sole live path. Per the redesign doc §5.0/§5.1-step-8, it is the
only commit in the rewrite that can regress a shipping user, so it is scoped here
exactly before anything is touched. Direction confirmed: **full removal** — old
code, legacy parity scaffolding, and the `NEW_STACK` seam all go; nothing is left
behind as a dead branch. A **separate follow-up commit** then strips the
`v2`/new/old naming.

The gate is met: full corpus green in CI on HEAD (after the `pool_full` fix), the
legality sweep is in, `nvs_delete_reload` is green, and the 36 e2e parity twins
match the legacy oracle case-for-case.

---

## The two commits

### Commit 1 — the cut-over (this plan)

Delete the legacy path, strip the seam, repoint the corpus. One reviewable commit.

### Commit 2 — the de-`v2` rename + comment scrub (separate, follows immediately)

No behavior change. Two parts, folded into one cleanup commit (matches the `ba583b7`
"describe code objectively, drop redesign/legacy framing" precedent):

**(a) Rename** `behavior_dynamic_macro_v2.c` → `behavior_dynamic_macro.c` and any
remaining `v2`/new/old identifiers + the CMake source line.

**(b) Comment scrub** — rewrite stale "old path / new stack / parity harness" framing
to describe the code as it now is (one stack, no oracle). Known sites at cut-over time:
- `src/dm_machine.c:440` — "as the old dm_feedback_save_queue_full did"
- `include/.../dm_feedback_pump.h:88-89` — "the parity harness wires a capture"
- `tests/unit/test_feedback_build.c:9,12` — "the old dm_feedback.c produced" / "pins parity"
- `tests/unit/test_machine.c:432` — "parity with the old cmd_record"
- `tests/unit/test_slot_store.c:413,605` — "parity with dm_storage_test_reload" / "load_e2e parity failure"
- `tests/events/nvs_delete_events/native_sim.keymap:15-16` — "the pre-fix new stack … the old path" (keep the bug-history *rationale*; drop the new/old framing)
- `tests/README.md:41` — "(new-stack only)" on `pool_full` (now the only stack)

Kept separate from commit 1 so the cut-over diff is *only* deletions + wiring (easy to
review as a behavior-preserving switch) and the rename/prose churn doesn't bury it.

---

## Commit 1 — precise file operations

### A. Delete the legacy production path (4 files)

| File | Size | Why it goes |
| --- | --- | --- |
| `src/behaviors/behavior_dynamic_macro.c` | 42 KB | legacy shell — replaced by `_v2` |
| `src/dm_feedback.c` | 56 KB | legacy 23-clone feedback — replaced by `dm_feedback_build` + `dm_feedback_pump` |
| `src/dm_storage.c` | 21 KB | legacy NVS — replaced by `dm_nvs.c` |
| `include/zmk-behavior-dynamic-macros/dm_feedback.h` | — | legacy feedback header |
| `include/zmk-behavior-dynamic-macros/dm_internal.h` | — | old-path-only; included **only** by the three files above + `dm_feedback.h` (verified: no new-stack TU includes it) |

**Pre-delete guard:** re-grep `dm_internal.h` / `dm_feedback.h` / `dm_storage.h`
includes across `src/ include/ tests/` and confirm zero references remain outside
the deleted set. (Confirmed at plan time; re-confirm at execution because the tree
may have moved.)

### B. Strip the CMake seam

`CMakeLists.txt` today is an `if (NEW_STACK) … else (legacy) … endif`. Collapse to the
new path only — unconditional, no `DM_NEW_STACK` define, no `else`:

```cmake
if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  zephyr_include_directories(include)

  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_render.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/slot_store.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_machine.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback_build.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback_pump.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/behaviors/behavior_dynamic_macro_v2.c)  # renamed in commit 2
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST app PRIVATE src/dm_nvs.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/dm_query.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/dm_events.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/events/dynamic_macro_state_changed.c)
endif()
```

(`dynamic_macro_state_changed.c` is shared — it was in *both* branches — and stays.)

### C. Remove the `DM_NEW_STACK` guards (make them unconditional)

`DM_NEW_STACK` is referenced in exactly two shared sources: `behavior_dynamic_macro_v2.c`
(2) and `dm_events.c` (2). With the compile-def gone, every `#if defined(DM_NEW_STACK)`
block must become unconditional (the new path is now the only path). Walk each of the 4
sites and delete the `#if/#endif`, keeping the enclosed code.

### D. Remove the Kconfig symbol

Delete `config ZMK_BEHAVIOR_DYNAMIC_MACRO_NEW_STACK` from `Kconfig` (lines ~209-216).
Nothing selects it anymore.

### E. Repoint the test corpus (the easy-to-miss part)

Today: the **legacy corpus** (`tests/core|feedback|events`, 37 cases) builds the **OLD**
path (its confs do *not* set `NEW_STACK`, except `pool_full` which is new-stack-only).
The **e2e parity twins** (`tests/parity/e2e`, 36 cases) build the NEW path. With the seam
stripped, **the only path is the new one**, so:

1. **The legacy corpus now builds the new path automatically** (no `NEW_STACK` needed —
   there is no other path). Remove the now-meaningless `CONFIG_…_NEW_STACK=y` line from
   `tests/core/pool_full/native_sim.conf` (the lone legacy case that set it).
2. **Delete the entire parity scaffolding** — it exists only to diff new-vs-old, and old
   is gone (§5.2: "step 8 removes the old side, after which the snapshot suite alone is
   the net"):
   - `tests/parity/e2e/` (36 twins + `generate.sh`)
   - `tests/parity/render/` + `tests/parity/render_corpus.h` (render golden capture)
   - i.e. the whole `tests/parity/` tree.
3. **Keep** the host unit suite (`tests/unit/`) and the 37 legacy snapshot cases — these
   become the permanent net, now running against the sole (new) path.

> **Why this ordering matters:** if A (delete old files) lands without E (repoint corpus),
> the 36 legacy cases that still target the old path fail to **link** (missing
> `behavior_dynamic_macro.c` etc.) — a build break, not a parity diff. A and E must be the
> same commit.

### F. Docs

- `tests/README.md` — drop the "parallel `tests/parity/e2e/` … (regenerate with generate.sh)"
  language from §"Test layers"; the parity rail is gone. The case tables stay.
- `README.md` — no user-facing change (the behavior is identical); leave it. The
  Firmware-Size placeholder is filled by step 9, not here.
- `docs/architecture-redesign.md` — tick step 8; note the parity harness retired.

---

## CI expectation (the real verification)

There is no local `native_sim` toolchain (Windows box), so CI is the gate. After commit 1:

- **Host tests (pure core)** — unaffected (pure modules unchanged): expect green, fast.
- **Run tests (main)** — now runs the 37 legacy snapshots **against the new path** (no
  parity twins). This is the moment of truth. Expectation: **green**, because the e2e twins
  already proved the new path reproduces every legacy snapshot. Any diff here is a genuine
  cut-over regression to explain — not to re-bless.

Count sanity: case count drops from (37 legacy + 36 e2e ≈ 73 PASS lines) to **37** — the
twins are gone, the legacy set now exercises the new path directly.

---

## Rollback

Commit 1 is a single revert away from the pre-cut-over state (old files restored, seam
back) if CI surfaces an unexpected regression. Because it is isolated, the revert is clean.

---

## Execution checklist

- [ ] Re-grep `dm_internal.h`/`dm_feedback.h`/`dm_storage.h` includes → zero outside deleted set
- [ ] Delete the 4 legacy files (+ `dm_internal.h`)
- [ ] Strip CMake `else` branch + `DM_NEW_STACK` define; new path unconditional
- [ ] Unguard the 4 `DM_NEW_STACK` sites (v2 shell ×2, dm_events.c ×2)
- [ ] Delete `NEW_STACK` Kconfig symbol
- [ ] Remove `NEW_STACK=y` from `tests/core/pool_full/native_sim.conf`
- [ ] Delete the whole `tests/parity/` tree
- [ ] Update `tests/README.md` (drop parity-rail language)
- [ ] Tick step 8 in `docs/architecture-redesign.md`
- [ ] Commit 1 (deletions + wiring); push; **watch CI to green**
- [ ] Commit 2 (de-`v2` rename) once CI is green
