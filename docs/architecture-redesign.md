# Architecture redesign — design document

Status: **draft for review** · 2026-06-09 · branch `docs/architecture-redesign`

This is the implementation blueprint for the deep-module rewrite decided in
[ADR-0001](adr/0001-deep-module-architecture.md). Vocabulary follows
[CONTEXT.md](../CONTEXT.md). Read the ADR first for the *why*; this doc is the *how*.

The goal is a **showcase-quality** rewrite: robust, host-testable, navigable, and
footprint-conscious, at **full feature/config parity** with today's behavior.

---

## 1. Module map

**Seven modules with real interfaces** (own files), plus two **co-located primitives**
(`tap_cadence`, `playback`) that are shared *code* with no seam anyone adapts — they
live inside the files that use them, not standalone translation units. Splitting them
out would perform modularity rather than provide depth (see §7 decisions).

```
                  ┌─────────────────────────────┐
                  │   behavior_dynamic_macro     │  driver shell: binding → command
                  └──────────────┬───────────────┘
                                 │ dm_machine_command() -> dm_result
                  ┌──────────────▼───────────────┐
                  │          dm_machine          │  ONLY writer of `state`
                  │  legality matrix + guards    │  ignore illegal, reject guard-fails
                  └───┬──────────┬──────────┬─────┘
        assign/move/del│          │ notify   │ render request
            ┌──────────▼─┐   ┌────▼─────┐  ┌─▼──────────────┐
            │ slot_store │   │ dm_events│  │  dm_feedback   │
            │ dual-write │   │ raise +  │  │  builder +     │
            │ invariant  │   │ query    │  │ feedback emit  │
            └─────┬──────┘   │ project. │  └───────┬────────┘
                  │ persist  └──────────┘          │ dm_render_slot()
            ┌─────▼──────┐                   ┌──────▼───────┐
            │  dm_nvs    │                   │  dm_render   │  PURE — host-testable
            │ single-    │                   │ events→sink  │  no ZMK, no I/O
            │ instance,  │                   │ locale table │
            │ file-scope │                   └──────────────┘
            └────────────┘
       co-located primitives (no own file):
         · tap_cadence — TAP_DELAY timer lifecycle + press/release phasing
         · playback emitter — replays a slot's dm_events (reads slot_store)
```

Dependency rule: **arrows point down; no module calls up.** `dm_render` depends on
nothing in the system (only on a locale table + a sink vtable). `behavior_*` depends
on everything but is depended on by nothing.

**Instance model** (see [ADR-0002](adr/0002-single-instance-internals.md)): interfaces
thread the instance per ZMK convention (`dev->data`, `slot_store *`, `dm_machine *`),
but three tiers apply — interfaces and behavior-owned state are per-instance; the
storage layer, query resolution, and listener suppression are deliberately
single-instance internals anchored to `BUILD_ASSERT(<=1)`. `dm_nvs` is therefore
file-scoped and does **not** thread an instance handle; the per-instance `slot_store`
calls into it.

---

## 2. Module contracts

Interfaces are the showcase surface — they are specified here before any code moves.
Each `*` handle is an opaque pointer to that module's private struct; no module reads
another's fields. The handle is not invented — it is the module's slice of `dev->data`,
so the ZMK device pointer *is* the handle, framework-provided (cf. `behavior_caps_word`).

### 2.0 `dm_result` — the shared outcome type

One enum names every transition outcome, whether returned synchronously or delivered
late through the deferred-feedback path. It exists because feedback already dispatches
on exactly these cases — it makes that dispatch type-checked rather than `rc != 0` plus
out-of-band knowledge of which failure occurred.

```c
typedef enum {
    DM_OK,
    DM_QUEUE_FULL,         /* storage queue saturated — retry later */
    DM_SAVE_FAILED,        /* NVS write failed (async only) */
    DM_DELETE_FAILED,      /* NVS delete failed (async only) */
    DM_REJECTED_OCCUPIED,  /* target slot not empty */
    DM_REJECTED_EMPTY,     /* source/target slot empty */
} dm_result;
```

**Deliberate flattening:** a synchronous return *could* type-hold an async-only value
(`DM_SAVE_FAILED`); the type system doesn't forbid it. Accepted on purpose — one
outcome type is worth more than encoding timing in the type. Do not re-split this into
sync/async enums thinking the flattening is a bug. The deferred-feedback queue entry
carries a `dm_result`, collapsing the old `dm_deferred_fb_type` into it.

### 2.1 `slot_store` — the deep one

Owns `slots[]`, `pending_delete`, `slot_generation`. The RAM+NVS dual-write
ordering and rollback are **internal**; callers never see them.

```c
typedef struct slot_store slot_store;

/* Queries */
bool            slot_store_is_empty(const slot_store *s, int idx);
const dm_slot  *slot_store_get(const slot_store *s, int idx);   /* NULL if empty */
int             slot_store_count(const slot_store *s, slot_class cls); /* NVS|RAM|ALL */

/* Mutations — each handles its own persistence for NVS slots */
dm_result slot_store_assign(slot_store *s, int idx, const dm_slot *macro);
dm_result slot_store_move(slot_store *s, int src, int dst);  /* ordering hidden */
dm_result slot_store_delete(slot_store *s, int idx);

/* Playback ownership — lets delete-completion avoid zeroing a playing slot */
void slot_store_mark_playing(slot_store *s, int idx);
void slot_store_clear_playing(slot_store *s);
```

**Internal invariants (ported fixes):**
- `move`: write+persist dst first; only on success zero+delete src. On dst enqueue
  failure, roll dst back, leave src intact. On src delete-enqueue failure, dst is
  safe; surface the error. *(ports `a2865b3`)*
- delete-completion: never `memset` a slot that is currently playing back; leave the
  RAM copy for the next op to overwrite. *(ports `fe3689e`)*
- every async op is stamped with the slot's generation; a completion whose generation
  is stale is ignored.

### 2.2 `dm_machine` — the only state writer

```c
typedef struct dm_machine dm_machine;

dm_result dm_machine_command(dm_machine *m, dm_command cmd, int param);
dm_state  dm_machine_state(const dm_machine *m);
```

`dm_machine_command` is **two-phase**:

1. **Legality gate** — a `static const` matrix `legality[state][command]` with two
   verdicts: `ALLOWED` or `IGNORED` (silently dropped, no feedback). Legality is a
   function of `(state, command)` *alone*, so the matrix is exhaustively testable: every
   one of `9 states × ~14 commands` cells has a known verdict. `IGNORED` returns `DM_OK`
   with no side effect (e.g. REC during PLAYING).
2. **Transition logic** — runs only on `ALLOWED`. Applies the runtime **guards**
   (slot occupancy, recording-buffer room, whether a move source is selected) by
   *asking* `slot_store` — it never peeks at slots itself. A failed guard returns a
   rejection `dm_result` (`DM_REJECTED_OCCUPIED` / `DM_REJECTED_EMPTY`) which drives
   feedback. On success it performs the side effect and writes `state` — the single
   place `state =` is written.

Rejection is **not** a table verdict: no `(state, command)` pair is statically
illegal-with-feedback: every feedback-bearing rejection is a data-dependent guard
outcome. Keeping the table two-valued keeps it honestly minimal — it answers exactly
"does this command do anything in this state?".

**Ported invariant:** REC from PENDING_ASSIGN with a non-empty recording buffer
discards the unassigned take and logs/surfaces the discard. *(ports `539260c`)*

### 2.3 `dm_render` — pure, host-testable

The duplication-killer. One event-walk, emitting to an abstract sink.

```c
typedef struct {
    void (*emit_char)(void *ctx, char c);
    void (*emit_token)(void *ctx, uint8_t mods, uint16_t page, uint32_t keycode);
    bool (*space_for)(void *ctx, uint8_t n);   /* backpressure for the ring sink */
    void *ctx;
} dm_sink;

void dm_render_slot(const dm_slot *slot, dm_locale locale, dm_sink *sink);
```

Two adapters:
- **ring sink** — used by the live typing pump; `space_for` returns ring headroom so
  rendering pauses when the ring is full and resumes on drain.
- **buffer sink** — used by `dm_get_preview_string`; `space_for` checks buffer length,
  `emit_*` append to a `char*`.

Locale → character mapping is a `static const` table per locale (replacing the
`#if DM_LOCALE` ladders). The replayable-vs-token decision (`is_replayable_event`)
lives here, used by both sinks — so they cannot disagree. *(ports `49c4f1a`, `86993af`)*

### 2.4 `dm_nvs` — storage backend (single-instance)

Async work queue + serialization, unchanged in mechanism. Owns the message queue, the
work handler, and the `settings` handler. **File-scoped single-instance** per
[ADR-0002](adr/0002-single-instance-internals.md): one work queue, one msgq, one
save/load buffer, shared — per-instance work queues would cost a kernel stack each for
unreachable genericity. It therefore takes no instance handle; the per-instance
`slot_store` calls into it. Serializes via an **aligned local header** *(ports
`277f0c8`)*. Calls back into `slot_store` for completion (clearing `pending_delete`,
honoring the playing-slot rule). Compiled only under `PERSIST`.

### 2.5 `dm_feedback` — message builder + feedback emitter

Keeps the ring, preview cursor, and erase scheduler. Collapses today's 23 near-clone
`feedback_*` functions into one builder over a message spec:

```c
typedef struct {
    const char *parts[…];     /* message-table fields to append */
    int         slot;         /* or -1 */
    dm_state    return_state; /* where the machine goes after typing finishes */
    int         min_level;    /* gate */
} dm_feedback_spec;

void dm_feedback_speak(dm_feedback *f, const dm_feedback_spec *spec);
```

The five-step ritual (gate → reset → append → start) lives once. The OFF path becomes
a single early-return inside `speak`, not a second 23-function stub block.

**Ported invariant:** deferred completions (`deleted`, `delete_failed`,
`save_failed`) only speak when the machine is IDLE; otherwise they drop rather than
hijack an active op.

`feedback_complete` (today's tangle) becomes a small dispatcher: "typing finished →
ask the status cursor if there's a next slot; else return the machine to its
return-state and fire the post-save persist + erase hooks." Each hook is a named
function, individually testable.

The feedback **emitter** (ring → keystrokes, with streaming preview refill) is one of
two emitters driven by the shared `tap_cadence` primitive; the other is the **playback
emitter** (slot `dm_event`s → keystrokes), co-located with playback. The cadence owns
only the `TAP_DELAY` timer lifecycle and press/release phasing; the two emit bodies do
not share. *(Co-located primitives — no own file; see §1, §7.)*

### 2.6 `dm_events` — notifications + query projection

Raises `zmk_dynamic_macro_state_changed` and exposes the read-only `dm_get_*` query
API as a **projection over a single behavior instance's `slot_store` + `dm_machine`
state**. Widgets query by slot index, not by device, so the projection resolves the
one instance internally — the single-instance assumption lives *here*, in one place,
beside the accessor and its `BUILD_ASSERT(<=1)`
([ADR-0002](adr/0002-single-instance-internals.md)). Compiled only under `EVENTS`.

---

## 3. Threading model (unchanged, restated cleanly)

- Behavior handlers + event listener + feedback pump run on the **system work queue**
  (cooperative, single-threaded).
- `dm_nvs` runs its serialization on a **dedicated work queue** at priority 10.
- Cross-thread contact points, now localized to `slot_store`/`dm_nvs`:
  - `pending_delete` — atomic bits.
  - `slot_generation` — written by behavior thread, read by nvs thread for staleness.
  - `slots[]` — behavior thread writes on assign; nvs thread `memset`s on delete
    completion, guarded by `pending_delete` + the playing-slot rule.
  - feedback is never driven from the nvs thread; completions are deferred to the
    system queue.

The rewrite does not change these rules — it moves them behind two module interfaces
so they're documented in one place each instead of spread across three files.

---

## 4. Test strategy — TDD on the pure core

**The pure core is built test-first (strict red-green-refactor).** `dm_render`,
`dm_machine`, and `slot_store` are extracted as Zephyr-free C, so their tests are
written *before* the implementation and drive the interface design.

The integration layer is **not** TDD — the existing `native_sim` snapshot suite plays
the role of a green-keeping safety net the rewrite must never break, not test-first
specs. This split is deliberate; see [ADR-0001](adr/0001-deep-module-architecture.md).

| Layer | Discipline | How tested |
| --- | --- | --- |
| `dm_render` | **TDD, test-first** | feed `dm_event[]`, assert string per locale/style |
| `dm_machine` | **TDD, test-first** | drive command sequences, assert state + rejections |
| `slot_store` | **TDD, test-first** | fake `dm_nvs` sink; assert dual-write ordering + rollback |
| `dm_feedback`, `dm_events`, behavior shell | tests-after / characterization | existing `native_sim` snapshots |

### 4.1 Dual-mode harness (new — Step 0 of the build)

The repo today has **only** native_sim snapshot tests (`tests/core/*` driven by
`west test` via `urob/zmk-actions`); there is no host unit-test target. We add one,
running the *same* assertions two ways:

- **Ztest under `west test`** — `zassert_*` assertions in a `tests/unit/` Ztest suite,
  so CI sees one unified harness alongside the snapshot tests.
- **Standalone host compile** — the same Zephyr-free `.c` modules + test files
  compiled directly (`cc` / a tiny CMake target), giving a **sub-second local
  red-green loop**. A thin `ztest_shim.h` maps `zassert_equal` etc. to plain
  `assert()` when compiled off-Zephyr.

The standalone compile is also a *decoupling proof*: if a pure module fails to compile
without Zephyr, the decoupling has regressed — the harness catches it.

### 4.2 First failing tests (written before their modules exist)

Each ported invariant becomes a red test first:
- **render** — `<LCTL+C>` for Ctrl+printable; UK Shift+3 → token (not GBP char);
  DE/FR plain previews emit letters/digits/space only.
- **machine** — REC from PENDING_ASSIGN discards the unassigned take; the legality
  matrix is exhaustively asserted (every `(state, command)` cell is ALLOWED or
  IGNORED); guard failures return `DM_REJECTED_OCCUPIED` / `DM_REJECTED_EMPTY`.
- **store** — move with dst-persist-fail rolls back (src intact); delete-while-playing
  skips the zero.

### 4.3 The snapshot suite as safety net

The `native_sim` suite stays green at every step (see §5). The `nvs_load` flakiness was
**shared-`flash.bin` test-harness contention, not a firmware bug** — the rewrite must
not regress real behavior, and that test stays deterministic by deleting slots before
recording (already in place).

---

## 5. Sequencing — keep the build green at every step

Each step ends with the existing `native_sim` suite green. Pure-core steps (1, 3, 4)
are **test-first**: the red Ztest/host test lands before the module it specifies.

0. **Stand up the dual-mode harness** (§4.1). A `tests/unit/` Ztest suite that runs
   under `west test`, plus a standalone host target + `ztest_shim.h` for the fast local
   loop. Prove it with one trivial passing test. *No production code moves yet.*
1. **`dm_render`, test-first.** Write the red render tests (§4.2) → extract `dm_render`
   as pure → replace both the `dm_get_preview_string` loop and
   `render_slot_contents_stream` with calls via the two sinks. *Net: duplication gone,
   behavior identical, green.*
2. **Locale data tables** inside `dm_render`, deleting the `#if DM_LOCALE` ladders. The
   render tests from step 1 are the safety net. *Footprint checkpoint: measure flash.*
3. **`slot_store`, test-first.** Write the red dual-write/rollback tests against a fake
   `dm_nvs` sink → extract `slot_store` with the invariant internal → `cmd_slot` and the
   nvs completion path call its interface instead of poking `slots[]`.
4. **`dm_machine`, test-first.** Write the red legality-matrix + guard tests → install
   the two-valued `static const` matrix + guard logic → route all 49 `state =` writes
   through `dm_machine_command`.
5. **Collapse `dm_feedback`** to the `speak` builder; untangle `feedback_complete` into
   named hooks. Guarded by snapshots (characterization, not TDD).
6. **Carve out `dm_events`** as the projection; assert single-instance in one place.
7. **Thin `behavior_dynamic_macro`** to wiring + dispatch.
8. **Footprint pass:** measure flash/RAM vs. the v0.3.1 baseline; claw back only where
   it demonstrably hurts, never at the cost of a documented invariant.

Steps 0–2 are low-risk and independently landable — they stand up the test harness and
kill the renderer duplication before any state/store surgery, validating the whole
approach (and the TDD loop) before committing to 3–7.

---

## 6. Open questions for review

Genuinely deferred — implementation-level, cheap to change, decided against real code
at the relevant build step. They do not move module boundaries.

- **Sink vtable cost:** the `dm_sink` indirection adds an indirect call per emitted
  char. Accepted for clean wins (clean-wins-on-ties, ADR-0001); *confirm the real cost
  at the step-2 footprint checkpoint* and claw back only if it demonstrably hurts.
- **`dm_feedback_spec` shape:** the exact field layout of the message spec (array of
  parts vs. a small builder DSL) — settle when collapsing the 23 clones in step 5.
- **`ztest_shim.h` surface:** which `zassert_*` macros the host shim needs to map —
  grows as the unit tests are written in steps 1/3/4.

---

## 7. Decisions resolved (grilling, 2026-06-09)

Recorded here so they are not re-litigated; the load-bearing ones became ADRs.

| Decision | Resolution | Where |
| --- | --- | --- |
| Instance model | Per-instance interfaces; single-instance internals in 3 tiers (storage, query, suppression). `dev->data` is the handle. | [ADR-0002](adr/0002-single-instance-internals.md) |
| Storage instancing | `dm_nvs` wholly single-instance, file-scoped — no instance handle. | ADR-0002 |
| Error model | One `dm_result` domain-outcome enum across sync + async delivery; errno confined inside `dm_nvs`. Deliberate timing-flattening accepted. | §2.0 |
| State machine | Two-valued legality matrix `[state][command] → ALLOWED\|IGNORED`; guards in code; rejection is a `dm_result`, not a table verdict. | §2.2 |
| Emit pump | `tap_cadence` primitive + two separate emitters (playback, feedback); not a unified `dm_pump`. | §2.5, CONTEXT |
| Module granularity | Seven modules with real interfaces get own files; `tap_cadence` and `playback` are co-located primitives — split where there's a seam, not a noun. | §1 |
| Handle granularity | Resolved by ADR-0002: the ZMK `dev` pointer is the handle; no separate `dm_context` aggregate. | ADR-0002 |
