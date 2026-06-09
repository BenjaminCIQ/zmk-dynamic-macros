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

**Arrows added at the 2026-06-09 review** (the diagram above predates them; each is
specified in the section cited):

- `dm_feedback → slot_store` — **read-only queries**: status streaming and saved/chain
  previews need live slot contents mid-typing (§2.5).
- `dm_feedback → dm_nvs` — **knob persistence**: `dm_nvs_save_knobs()` for the runtime
  level/style/erase keys (§2.4, §2.5). Symmetric with `slot_store`'s slot persistence.
- `dm_nvs ⇢ slot_store / dm_feedback` *(dashed)* — **export read-back**: settings export
  reads current slots + knobs through public queries only; the one documented up-read,
  single-instance-anchored (§2.4, ADR-0002).
- Boot restore is delivered upward through narrow setters (`slot_store_load`,
  `dm_feedback_restore_knobs`) — a *delivery* to the data's owner, the load-time mirror
  of `deliver_async`, not a call into logic (§2.4).

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
    DM_SAVE_QUEUE_FULL,    /* save enqueue refused — storage queue saturated */
    DM_DELETE_QUEUE_FULL,  /* delete enqueue refused — storage queue saturated */
    DM_SAVE_FAILED,        /* NVS write failed (async only) */
    DM_DELETE_FAILED,      /* NVS delete failed (async only) */
    DM_REJECTED_OCCUPIED,  /* target slot not empty */
    DM_REJECTED_EMPTY,     /* source/target slot empty */
    DM_REJECTED_FULL,      /* recording draft / chain would overflow MAX_EVENTS */
} dm_result;
```

> **`DM_REJECTED_FULL` was added at step 3** (not in the original draft). The
> chain-no-room rejection (old `feedback_chain_no_room`) has a *distinct*
> user-facing message from both slot-empty and storage-queue-full, so collapsing
> it onto a queue-full code would force feedback (step 5) to disambiguate
> buffer-full from msgq-full by out-of-band knowledge — the exact thing this enum
> exists to prevent. One code per distinct outcome keeps the feedback dispatch
> type-checked. `slot_store_draft_chain` returns it; `draft_empty` reuses
> `DM_REJECTED_EMPTY` (the old code likewise reused the slot-empty message).

> **Queue-full was split per op (2026-06-09 review; replaced a single
> `DM_QUEUE_FULL`)** by the same rule. The old code emits *distinct* messages —
> `[DM SAVE QUEUE FULL <slot>]` vs `[DM DEL QUEUE FULL <slot>]` — and a move can
> fail either way: dst-save-enqueue-full (rolled back, message names **dst**) or
> src-delete-enqueue-full (dst kept, message names **src**). With one shared code
> the machine could not pick the message *or the slot to name* without knowing
> which phase of the move failed — out-of-band knowledge again. The sink's `save`
> returns `DM_OK | DM_SAVE_QUEUE_FULL`; `del` returns `DM_OK |
> DM_DELETE_QUEUE_FULL`; `slot_store` passes them through untranslated.

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

/* The NVS sink — slot_store's only downward dependency for persistence, injected
 * at init so the dual-write ordering is host-testable against a fake (§4). save/
 * del are the ENQUEUE step (save: DM_OK | DM_SAVE_QUEUE_FULL; del: DM_OK |
 * DM_DELETE_QUEUE_FULL — split per §2.0); the async outcome of a delete comes
 * back via slot_store_complete_delete(). RAM slots never reach it. */
typedef struct {
    dm_result (*save)(void *ctx, int slot, const struct dm_slot *s, uint32_t generation);
    dm_result (*del)(void *ctx, int slot, uint32_t generation);
    void *ctx;
} dm_nvs_sink;

void slot_store_init(slot_store *s, const dm_nvs_sink *sink);  /* sink NULL only if RAM-only */

/* Queries */
bool            slot_store_is_empty(const slot_store *s, int idx);
const dm_slot  *slot_store_get(const slot_store *s, int idx);   /* NULL if empty */
int             slot_store_count(const slot_store *s, slot_class cls); /* NVS|RAM|ALL */

/* Mutations — move/delete handle their own persistence for NVS slots; assign's
 * persist is the SEPARATE slot_store_persist step, deferred by the machine to
 * typing-finished (§2.7.3 — ports feedback_post_save_slot) */
dm_result slot_store_move(slot_store *s, int src, int dst);  /* ordering hidden */
dm_result slot_store_delete(slot_store *s, int idx);
dm_result slot_store_persist(slot_store *s, int idx);        /* assign's deferred enqueue */

/* Async delete completion (called by the nvs driver once settings_delete returns).
 * Honors the playing-slot rule + generation staleness internally; returns the
 * filtered outcome (DM_OK | DM_DELETE_FAILED). */
dm_result slot_store_complete_delete(slot_store *s, int idx, uint32_t generation, bool ok);

/* Draft buffer (the recording buffer) — a dm_slot-shaped staging area, owned here
 * because every operation on it is slot-to-slot byte work (see §2.7 / Risk 3). The
 * machine asks for counts and commits; it never touches the bytes. */
void     slot_store_draft_reset(slot_store *s);                       /* REC start */
bool     slot_store_draft_append(slot_store *s, const dm_event *e);   /* listener; false = full */
uint32_t slot_store_draft_count(const slot_store *s);                 /* guard input */
dm_result slot_store_draft_chain(slot_store *s, int src);             /* chain slot src into draft */
dm_result slot_store_draft_commit(slot_store *s, int dst);            /* assign: draft → dst, RAM ONLY */

/* Playback ownership — lets delete-completion avoid zeroing a playing slot */
void slot_store_mark_playing(slot_store *s, int idx);
void slot_store_clear_playing(slot_store *s);

/* Restore surface — dm_nvs ONLY (boot settings_load + DM_TEST_RELOAD). load is
 * a raw populate: no sink echo, no generation bump, clears a stale pending bit;
 * serialization validation (version/length) stays in dm_nvs, the store defends
 * only count <= MAX_EVENTS. reset zeroes slots/pending/generations ahead of a
 * settings_load re-run; the draft is untouched (reload is IDLE-only). */
bool slot_store_load(slot_store *s, int idx, const dm_event *events, uint32_t count);
void slot_store_reset(slot_store *s);
```

(`slot_store_draft_commit` replaces the old `slot_store_assign` — assign is always
"commit the draft into a slot", so a separate `assign(macro)` taking an external `dm_slot`
was a phantom second path. The only producer of an assignable macro is the draft buffer.)

**Internal invariants (ported fixes):**
- `move`: write+persist dst first; only on success zero+delete src. On dst enqueue
  failure, roll dst back, leave src intact (`DM_SAVE_QUEUE_FULL`). On src
  delete-enqueue failure, dst is safe; surface `DM_DELETE_QUEUE_FULL`. *(ports
  `a2865b3`)* Same-slot move never reaches the store — the machine's guard turns
  it into a cancel (§2.7.2).
- **assign persists late, by design**: `draft_commit` is RAM-only; the machine
  calls `slot_store_persist` from `dm_machine_typing_finished()` (ports
  `feedback_post_save_slot`) so the SAVED message types from a settled state and
  a queue-full outcome can speak afterwards. At feedback levels that type
  nothing, typing-finished fires synchronously (§2.7.3), which degenerates to
  the old immediate save — one rule, both old paths.
- delete-completion: never `memset` a slot that is currently playing back; leave the
  RAM copy for the next op to overwrite. *(ports `fe3689e`)*
- every async op is stamped with the slot's generation; a completion whose generation
  is stale is ignored.
- `draft_append` returns `false` when the draft is at `MAX_EVENTS` (the recording-overflow
  guard input); `draft_chain` rejects a chain that is empty (`DM_REJECTED_EMPTY`) or would
  not fit (`DM_REJECTED_FULL`), as a `dm_result`. The draft is plain RAM — no persistence —
  until `draft_commit`.

**The `dm_nvs_sink` is a test seam, not a second instancing of storage.** ADR-0002 keeps
`dm_nvs` file-scoped/single-instance with no instance handle; the sink vtable does not
re-instance it. It exists so `slot_store` (pure) can be host-tested against a fake that
injects queue-full and drives `complete_delete` synchronously — the only way to pin
the dual-write ordering with a C compiler (§4). The firmware wires a one-line adapter whose
`save`/`del` call straight into the file-scoped `dm_nvs` enqueue; there is exactly one sink
in the real build. The seam is real (a fake adapts it in tests), so by the LANGUAGE rule it
earns the vtable — unlike the cadence (§2.5), which nothing adapts.

### 2.2 `dm_machine` — the only state writer

```c
typedef struct dm_machine dm_machine;

dm_result dm_machine_command(dm_machine *m, dm_command cmd, int param);
dm_state  dm_machine_state(const dm_machine *m);

/* Up-calls — commands, not state writes. Feedback and the storage backend
 * report completion here; the machine owns the resulting transition. See §2.7. */
void dm_machine_typing_finished(dm_machine *m);                 /* feedback emitter, on ring drain */
void dm_machine_deliver_async(dm_machine *m, dm_result outcome, int slot); /* nvs deferred completion */
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

**Ported invariants:**
- REC from PENDING_ASSIGN with a non-empty draft buffer discards the unassigned take
  and logs/surfaces the discard. The machine detects this by asking
  `slot_store_draft_count() > 0`, never by reading buffer bytes. *(ports `539260c`)*
- **Rejection preserves the pending state — it does not silently drop to IDLE.** A guard
  rejection carries a *return-state*, and it is data-dependent: an occupied/empty target
  during a move returns to `MOVE_PENDING`; an occupied target during assign returns to
  `PENDING_ASSIGN`. The machine writes that return-state and reschedules
  `assign_timeout_work`; the timeout is the *only* escape. This is the "slot full → press
  another slot to assign there" affordance — the same behavior the `nvs_load`
  investigation proved was intentional (an occupied-slot assign keeps PENDING_ASSIGN by
  design). The lone exception is occupied-in-DELETE, which the guard sends to IDLE. Full
  rule in §2.7.

### 2.3 `dm_render` — pure, host-testable

The duplication-killer. One event-walk, emitting to an abstract sink.

```c
typedef struct {
    void (*emit_char)(void *ctx, char c);
    bool (*space_for)(void *ctx, uint8_t n);   /* backpressure for the ring sink */
    void *ctx;
} dm_sink;

/* Pause/resume state — caller-owned so the renderer stays stateless. Carries
 * BOTH the position and the modifier state accumulated before the pause (a held
 * Ctrl from event i-2 still modifies the token at event i). The typed equivalent
 * of the old preview_idx/preview_mods fields. Zero-init to start. */
typedef struct { uint32_t idx; uint8_t active_mods; } dm_render_cursor;

/* true = walk complete; false = sink refused space for the next unit — nothing
 * was emitted for it, cursor holds the resume point, re-enter after drain.
 * cursor == NULL is a one-shot render from the start (the buffer-sink shape). */
bool dm_render_slot(const dm_render_slot_view *view, dm_locale locale, dm_sink *sink,
                    dm_render_cursor *cursor);
```

> **The cursor was added at the 2026-06-09 review.** The original draft signature
> (`void dm_render_slot(slot, locale, sink)`) claimed resumability but could not
> deliver it: re-entering restarts at event 0, and a sub-view starting at the
> pause point loses the modifier state accumulated before it. Pinned by
> `resume_preserves_held_modifier` / `resume_does_not_reemit` (host tests).

Two adapters:
- **ring sink** — used by the live typing pump; `space_for` returns ring headroom so
  rendering pauses when the ring is full and resumes on drain, re-entering with the
  same `dm_render_cursor`.
- **buffer sink** — used by `dm_get_preview_string`; `space_for` checks buffer length,
  `emit_char` appends to a `char*`; passes a NULL cursor (one-shot).

**Truncation is stop-at-first-non-fit — a deliberate, pinned change (decided
2026-06-09, same precedent as the step-2 DE/FR tightening).** The old
`dm_get_preview_string` walk *skipped* an oversized token but kept appending later,
smaller characters, so a clipped preview could show a sequence with a silently
missing middle token. `dm_render` instead stops at the first unit that does not fit:
a truncated preview is always an honest prefix. Observable only when the destination
buffer is smaller than the preview. Do not "fix" this back to skip-and-continue.

**The sink is char-only — there is no `emit_token`, by design (step-1 decision).** A token
like `<LCTL+C>` is emitted as its individual characters through `emit_char`; `dm_render`
owns the token *formatting* (the `<…>` delimiters, `+` separators, mod-name spelling, and
the plain-locale spacing variant) in **one** place, so the two sinks cannot disagree — which
is the whole point of the consolidation. Verified against every consumer: the ring types
chars, `dm_get_preview_string` returns a string of chars, and `space_for(n)` covers the
only thing the old structured `token_size()` was for (telling the sink how many chars are
coming, for backpressure). A sink never needs the structured `(mods, page, keycode)` form,
because a widget that wants to render tokens its *own* structured way already bypasses
`dm_render` entirely and reads the raw `dm_event[]` via `dm_get_slot_events()`. Re-adding an
`emit_token` vtable slot would re-introduce per-sink formatting — the exact duplication this
module deletes — to serve a consumer that does not exist. Don't.

**The sink is a runtime vtable, not a compile-time-selected call — and this is forced,
not a default.** Under the common config (`EVENTS` + typing) *both* adapters are
compiled and both run: `dm_get_preview_string` (buffer) and the live pump (ring) are
live in the same build, so there is no single build-time winner to `#if` on. The ring
sink is moreover *resumable* — today's `render_slot_contents_stream` returns "ring full,
re-enter me after drain" and is driven one `fb_event` per `TAP_DELAY` by the emit
handler; `space_for` is exactly that backpressure point. A compile-time direct call
can express neither the coexistence nor the pause/resume without re-coupling the caller
to the renderer's internals. **The indirection is free on cycles** (≈3 indirect calls
per char against a 30 ms `TAP_DELAY` budget per char) and its flash cost is recovered by
deleting the duplicated second walk.

Locale → character mapping is a `static const` table per locale (replacing the
`#if DM_LOCALE` ladders). The replayable-vs-token decision (`is_replayable_event`)
lives here, used by both sinks — so they cannot disagree. *(ports `49c4f1a`, `86993af`)*

**Locale is compile-time; style and level are not — do not conflate them.** `DM_LOCALE`
is a Kconfig constant (one keyboard layout per firmware), so the locale table is
genuinely link-time-fixed and may be selected at link time. **Style (`FULL`/`ARROW`) and
feedback level are runtime-mutable** (`DM_STYLE_TOGGLE`/`DM_FEEDBACK_INC`/`_DEC` write
`current_feedback_style`/`current_feedback_level`, read live on the emit path). The
renderer therefore takes locale *as link-time data* but must never bake in style or
level. Level is checked *upstream* of `dm_render_slot` (the builder early-returns before
rendering), so it does not touch the sink; style affects which message-table strings are
appended and is settled in `dm_feedback` (see §2.5), not frozen into the render table.

### 2.4 `dm_nvs` — storage backend (single-instance)

Async work queue + serialization, unchanged in mechanism. Owns the message queue, the
work handler, and the `settings` handler. **File-scoped single-instance** per
[ADR-0002](adr/0002-single-instance-internals.md): one work queue, one msgq, one
save/load buffer, shared — per-instance work queues would cost a kernel stack each for
unreachable genericity. It therefore takes no instance handle; the per-instance
`slot_store` calls into it. Serializes via an **aligned local header** *(ports
`277f0c8`)*. Completion is **delivered on the system work queue** (§3): the storage
thread finishes the settings op and submits one completion that runs
`slot_store_complete_delete` + `dm_machine_deliver_async` back-to-back. Compiled
only under `PERSIST`.

**Three surfaces added at the 2026-06-09 review** (the original draft could not be
implemented without them):

- **Boot restore.** `dm_settings_set` decodes and validates (version, length,
  `event_count` bound) exactly as today, then delivers via the narrow restore
  setters — `slot_store_load()` for slots, `dm_feedback_restore_knobs()` for the
  feedback level/style/erase keys. Boot restore is a sanctioned upward *delivery*
  (data handed to its owner), not an upward call into logic; it is the load-time
  mirror of `deliver_async`.
- **Knob persistence.** `dm_nvs_save_knobs(level, style, erase)` keeps today's
  third storage-op type (`DM_STORAGE_OP_SAVE_FEEDBACK`); called by `dm_feedback`
  when a knob command lands (§2.5). Queue-full is logged, not user-spoken — as
  today.
- **Settings export.** `dm_settings_export` (any `settings_save()` full dump) needs
  the current slots + knobs. At step-7 wiring the shell hands `dm_nvs` the
  instance's `slot_store *` and `dm_feedback` handle; export reads back **only
  through the public query API** (`slot_store_get`/`slot_store_count`, knob
  getters). This is the one documented up-read, anchored to the same
  `BUILD_ASSERT(<=1)` as the rest of the single-instance internals
  ([ADR-0002](adr/0002-single-instance-internals.md), fourth tier) and drawn
  dashed in §1.

**`DM_TEST_RELOAD` lives here.** The test-only flush+reload command (`dm_storage_flush` /
`dm_storage_test_reload`, behind `CONFIG_..._TEST_RELOAD`) is `dm_nvs`'s own surface — it
drains the work queue, zeroes RAM via `slot_store_reset()`, and re-runs
`settings_load`, all of which are `dm_nvs`-internal mechanics. The behavior shell only *dispatches* the command (it is
ignored unless IDLE, like any other); the machine is not involved because reload is not a
user-facing transition. This keeps the single-instance `dm_devices[]` iteration in the
reload path beside the rest of the file-scoped storage code, not leaked into the shell.

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

**Invariant — the spec must not freeze the runtime style.** `current_feedback_style`
(`FULL`/`ARROW`) is mutable at runtime via `DM_STYLE_TOGGLE`, so the message-part
selection inside `speak` must read it live; a `dm_feedback_spec` that bakes in style at
build time (or caches a style-specific part list) silently breaks the toggle. The level
gate (`feedback_enabled_for`) is the other runtime knob and is already the early-return
at the top of `speak`. Locale, by contrast, is link-time-fixed and belongs to the render
table (§2.3) — only style and level vary per keypress.

**The three runtime knobs (level, style, auto-erase) live here, persistence included**
(2026-06-09). The knob commands arrive as ordinary machine commands (their IDLE-only
gate is just a legality-matrix row, deleting the hand-rolled state checks); the effect
adjusts the knob, calls `dm_nvs_save_knobs()` (a thin downward arrow, symmetric with
`slot_store`'s slot persistence), and speaks the confirmation. Boot restore arrives via
`dm_feedback_restore_knobs()` (§2.4), and the **ARROW-requires-full-punctuation-locale
rule lives once, inside that setter** — today it is duplicated between
`cmd_style_toggle` and `dm_settings_set`; the consolidation preserves the Kconfig-
documented "persisted ARROW silently ignored on a plain locale" fallback by
construction.

**Ported invariant:** deferred completions (`deleted`, `delete_failed`,
`save_failed`) only speak when the machine is IDLE; otherwise they drop rather than
hijack an active op.

**Below-level is "finish instantly", not "do nothing" (2026-06-09 — the OFF-path
rule).** Today every feedback function raises its widget notification *before* the
level gate, and the gated early-return still applies the return-state and reschedules
timeouts (the `!DM_TYPING_ENABLED` stub block is 20 functions of exactly this). In the
new stack: the **machine** raises the notification before calling `speak` (so it fires
at every level by construction), and `speak`'s below-level early-return calls
`dm_machine_typing_finished()` **synchronously** before returning. typing_finished does
identical work either way — return-state, timeout reschedule, post-save persist
(`slot_store_persist`, §2.1), erase kick — so one rule covers OFF, below-level, and
empty-message, and the stub block is deleted rather than rewritten. Pinned test-first
in step 5 at each feedback level; this is the easiest quiet-regression in the rewrite.

**Status invariants (ported verbatim, each a parity test):** the STATUS gate is the
**compile-time** `DM_STATUS_DETAIL` knob, independent of the runtime level; the header
line renders the filled count + slot-range; iteration is used-slots-only below
`STATUS_FULL`, all slots at `STATUS_FULL`; each slot's preview streams with a
continuation cursor and gets its count suffix on completion; status output is excluded
from auto-erase. Status needs live slot contents *mid-typing*, so `dm_feedback` holds
the `slot_store *` and reads through its public queries — the read-only
`dm_feedback → slot_store` arrow in §1.

`feedback_complete` (today's tangle) **splits along the module boundary** (§2.7): the
*rendering-continuation* half — "is there a next status slot to type?" — stays in
`dm_feedback` and resolves before the machine hears anything. The *state-return* half —
apply return-state, reschedule pending-timeout, fire post-save persist + erase — moves
**into `dm_machine`** behind `dm_machine_typing_finished()`, because it writes `state`
and only the machine may. Each resulting hook is a named, individually-testable function.

The feedback **emitter** (ring → keystrokes, with streaming preview refill) is one of
two emitters driven by the shared `tap_cadence` primitive; the other is the **playback
emitter** (slot `dm_event`s → keystrokes), co-located with playback. The cadence owns
only the `TAP_DELAY` timer lifecycle and press/release phasing; the two emit bodies do
not share. *(Co-located primitives — no own file; see §1, §7.)*

**Why the cadence is not a seam (unlike `dm_render`'s sink).** The obvious objection: two
emitters consume one cadence, so by "two adapters = a real seam" (LANGUAGE) this should be
a `dm_emitter` vtable with playback and feedback as adapters, symmetric with the sink. It
is not, and the asymmetry is the point. `dm_render`'s two sinks are **live in the same
build and on overlapping calls** — `dm_get_preview_string` (buffer) and the typing pump
(ring) both run, and the ring sink is *resumable* mid-render; the vtable expresses a
coexistence and a pause/resume a direct call cannot. The two emitters, by contrast, are
**mutually exclusive states**: `emit_work_handler` is already an `if (state == PLAYING)
… else if (TYPING_*)` — the `state` field has *already* dispatched before any emit body
runs. A `dm_emitter` vtable would add a function-pointer indirection to re-express a choice
`dm_machine` made for free, and nothing swaps the emitter at runtime or in any test. By the
**deletion test**: delete the hypothetical vtable and no complexity reappears — the state
branch was the dispatch. So the cadence is shared *code* (timer + phasing), not a shared
*seam*; promoting it to a module would pay flash + indirection on a footprint-conscious
target to perform modularity. Co-located is the depth-correct call.

### 2.6 `dm_events` — notifications + query projection

Raises `zmk_dynamic_macro_state_changed` and exposes the read-only `dm_get_*` query
API as a **projection over a single behavior instance's `slot_store` + `dm_machine`
state**. Widgets query by slot index, not by device, so the projection resolves the
one instance internally — the single-instance assumption lives *here*, in one place,
beside the accessor and its `BUILD_ASSERT(<=1)`
([ADR-0002](adr/0002-single-instance-internals.md)). Compiled only under `EVENTS`.

The full notification inventory — which transitions raise an event and which
deliberately do not — is pinned in
[§8](#8-appendix--notification-inventory-parity-checklist).
Ported quirk to retain: when typing is compiled out (`!DM_TYPING_ENABLED`),
`dm_get_preview_string` returns the `"(N events)"` fallback instead of a rendered
preview.

### 2.7 Interaction contracts — orchestration, completion, draft ownership

The §1 diagram says "arrows point down; no module calls up." Today's code violates this
in three places that look like separate problems but share one spine: **the command
handler is the orchestrator and freely interleaves four concerns** (state write, slot
byte-work, persistence, feedback), and **feedback reaches up to write `state`.** These
three contracts make the arrows actually point down. They are specified here because each
is a place where a clean-looking rewrite most easily ships a *quiet behavior change* —
the contract is the regression guard.

#### 2.7.1 Orchestration: the machine runs the transition as a transaction

`dm_machine_command` *is* the orchestrator that `cmd_slot` is today — but the three
concerns sit behind interfaces and obey a fixed order:

1. **Legality** (table) → `IGNORED` returns `DM_OK`, no effect.
2. **Guard** → asks `slot_store` (`is_empty`, `count`, `draft_count`); never reads
   `slots[]`. A failed guard returns a rejection (§2.7.2) and applies its return-state.
3. **`state =` is written exactly once, *before* any effect that can emit feedback.**
   This generalizes today's load-bearing "move to IDLE up front" trick (`cmd_slot`
   MOVE_PENDING, with its 9-line comment) into a law: the queue-full/failure feedback
   paths only speak from a settled state and start their own typing, so the machine must
   not still be mid-transition when an effect calls back.
4. **Effect** → `slot_store_move/delete/draft_commit`. The dual-write ordering and
   rollback live *inside* `slot_store_move` and surface only as a `dm_result`
   (`DM_OK | DM_SAVE_QUEUE_FULL | DM_DELETE_QUEUE_FULL`). The machine knows
   "moved / which enqueue failed", never "dst-before-src".

> **Rejected alternative — "machine returns an effect list, caller executes":** reads
> clean but re-exposes the dst→src ordering to the caller (the exact leak ADR-0001
> removes) and cannot express "roll dst back on the failure of step N". The
> transaction-inside-the-machine keeps ordering hidden where §2.1 owns it.

#### 2.7.2 Rejection carries a return-state (data-dependent, not uniform)

A guard rejection is `(dm_result, return_state)`. The return-state is **computed from the
current state**, mirroring today's helpers exactly *(table corrected & completed at the
2026-06-09 review — the original draft missed the recording-context rejections and had a
phantom "occupied in DELETE" row; occupied in DELETE is the delete success path)*:

| Reject cause | While moving | While recording (chain) | Otherwise |
| --- | --- | --- | --- |
| target occupied (`DM_REJECTED_OCCUPIED`) | → `MOVE_PENDING` | — | → `PENDING_ASSIGN` *(ports `feedback_slot_full`)* |
| source/target empty (`DM_REJECTED_EMPTY`) | → `MOVE_PENDING` | → `RECORDING` *(ports `feedback_chain_empty`)* | → `IDLE` *(ports `feedback_slot_empty`; covers play-empty and the empty target in DELETE — the one drop-to-idle)* |
| no room (`DM_REJECTED_FULL`) | — | → `RECORDING` *(ports `feedback_chain_no_room`)* | — |
| recording overflow (draft full) | — | → `PENDING_ASSIGN` + timeout *(ports `feedback_overflow`)* | — |

Two cases that look like rejections but are not:

- **Same-slot move is a cancel, not a rejection.** The machine guard checks
  `src == dst` *before* calling `slot_store_move`: cancel the timeout, clear the
  move source, → `IDLE`, speak move-cancelled *(ports `feedback_move_cancelled`)*.
  It never reaches the store — a naive pass-through would return
  `DM_REJECTED_OCCUPIED` and turn the cancel affordance into a bogus
  slot-full stuck in `MOVE_PENDING`.
- **Recording overflow is a machine event, not a store rejection.** The listener
  appends via `slot_store_draft_append` (§2.7.4); on `false` the shell issues an
  internal machine command (`DM_CMD_OVERFLOW`, legality: ALLOWED only in
  `RECORDING`) so the RECORDING → typing → `PENDING_ASSIGN` transition stays a
  machine write.

The machine writes the return-state and, if it is a `*_PENDING` state, reschedules
`assign_timeout_work`. **The timeout is the only escape from a preserved pending state.**
This is the "press another slot" affordance the `nvs_load` investigation proved
intentional — it must survive verbatim (also listed as a §2.2 ported invariant).

#### 2.7.3 Completion: feedback reports up, the machine transitions

Feedback **never writes `state`.** Four sanctioned up-calls (commands, not writes —
the erase pair was added at the 2026-06-09 review) replace the illegal up-arrows in
today's `feedback_complete` / deferred handlers / erase scheduler:

- **`dm_machine_typing_finished(m)`** — the feedback emitter calls this when the ring
  drains, **or `speak` calls it synchronously when the level gate types nothing**
  (§2.5 — the OFF-path rule). The machine applies the `return_state` it parked when it
  *started* the feedback transition (the return-state lives in the machine now, not in
  `feedback_return_state`), reschedules any pending-timeout, fires the post-save
  persist (`slot_store_persist`, §2.1 — ports `feedback_post_save_slot`), and kicks
  auto-erase. The status-cursor "another slot to type?" loop is resolved *inside*
  `dm_feedback` first and does **not** reach the machine until typing is truly done.
- **`dm_machine_deliver_async(m, outcome, slot)`** — the `dm_nvs` deferred handler
  (running on the system queue after `k_work_submit`) calls this for `DELETED /
  SAVE_FAILED / DELETE_FAILED`. The **IDLE-suppression rule lives here, in the machine**:
  it drops the outcome unless the machine is IDLE, else drives the feedback. This removes
  the last `state = IDLE` writes from `dm_feedback` and homes the "deferred only when
  IDLE" invariant in one place (resolving the §2.5-vs-machine ownership question in favor
  of the machine).
- **`dm_machine_erase_due(m)`** — the erase scheduler's delayable work reports the
  delay expiring. The machine parks the current state as the erase-return-state
  (**parked once**: a continuation batch arriving while already in `TYPING_ERASE`
  keeps the original parked state, as today), writes `TYPING_ERASE`, and asks
  feedback to emit the backspaces. Everything else stays feedback-internal: the
  char count (non-RET presses during `TYPING_FEEDBACK` only), the
  `FB_RING_SIZE−1` batching with re-arm, the status-output exclusion. Batch
  continuation resolves inside `dm_feedback` before typing-finished is reported —
  the same rendering-continuation rule as status.
- **`dm_machine_erase_cancel(m)`** — both cancel paths (any DM binding press, any
  keycode through the listener) report through here; the machine restores the
  parked erase-return-state and feedback drains its ring. Today both paths write
  `state` directly from `dm_feedback_cancel_erase` — that up-write is what this
  call removes.

#### 2.7.4 Draft ownership: the recording buffer belongs to `slot_store`

The recording buffer is a `dm_slot`-shaped staging area; every operation on it is
slot-to-slot byte work (assign = copy draft→slot; chain = copy slot→draft tail), which is
`slot_store`'s domain — not the machine's (it would force the machine to do `memcpy`) and
not a new "recorder" module (a noun with no second consumer or adapted seam; §7's rule).
It lives behind the draft surface in §2.1. The **listener** (behavior shell) is the one
external writer, via `slot_store_draft_append` — a clean append, not a struct-poke. The
**machine** only asks `slot_store_draft_count` (for the REC-discard and chain-room guards)
and calls `draft_commit` on assign.

#### Implementation order (these interlock)

1. **2.7.4 with step 3** (`slot_store`) — the machine's guards in step 4 need
   `draft_count` to exist.
2. **2.7.1 + 2.7.2 + 2.7.3 with step 4** (`dm_machine`) — the orchestration contract *is*
   step 4; the rejection table is a ported invariant written test-first. The §2.7.3
   up-calls (`typing_finished`/`deliver_async`) and the IDLE-suppression rule are part of
   the new machine's interface and are asserted **test-first** here. They encode the
   riskiest design in the rewrite — the feedback→machine→state call-graph inversion — so it
   is proven entirely in the new stack under test, with the old behavior available to diff
   against. The old `feedback_*` bodies are **not** repointed; they keep writing their own
   `state` until the step-8 cut-over removes them.
3. **The new `dm_feedback` is step 5** — built as a fresh module whose `speak(spec)` calls
   *into* the step-4 machine's up-calls. It is verified by the **parity harness** (§5.2)
   against the old feedback output, plus a targeted test per deferred-completion path
   (`save_failed` / `delete_failed` / `deleted`) pinning the unified IDLE check. The old
   23-clone `dm_feedback.c` stays untouched and live alongside it until step 8.

---

## 3. Threading model (narrowed at the 2026-06-09 review)

- Behavior handlers + event listener + feedback pump run on the **system work queue**
  (cooperative, single-threaded).
- `dm_nvs` runs its serialization on a **dedicated work queue** at priority 10.
- **Delete-completion is delivered on the system queue** *(the one deliberate
  narrowing)*: the storage thread finishes `settings_delete` and submits a single
  completion that runs `slot_store_complete_delete` + `dm_machine_deliver_async`
  back-to-back on the system queue. Today the storage thread `memset`s `slots[]`
  itself and only the *feedback* is deferred; moving the whole completion over
  shifts the memset by one work-queue hop — unobservable, because the slot already
  reads empty via `pending_delete` (set on the system queue before the enqueue).
- Consequence: **every `slot_store` entry point runs on the system work queue.**
  The store is single-threaded *by contract* — its plain (non-atomic)
  `pending_delete` words are correct, not a host-test shortcut — and the storage
  thread never touches `slots[]`, `pending_delete`, or `slot_generation` again.
  The playing-slot rule stays regardless: zeroing a mid-playback slot would still
  logically corrupt playback even on one thread.
- The only remaining cross-thread contact is the storage **msgq** and the op
  payload it carries (each op copies its slot data + generation stamp at enqueue,
  as today).
- Feedback is never driven from the nvs thread; completions are deferred to the
  system queue (unchanged).

Apart from the completion hop, the rewrite does not change these rules — it moves
them behind two module interfaces so they're documented in one place each instead
of spread across three files.

**Tuning constants are ported verbatim, not re-derived.** `FB_RING_SIZE` (64), the
storage msgq depth (4), the storage work-queue priority (10), and its stack size (1024)
are carried over unchanged. They are field-tuned values, not architecture: the rewrite
moves them behind `dm_feedback`/`dm_nvs` but does **not** treat them as open questions,
because changing them is a behavior/footprint experiment independent of the module split.
If the step-8 footprint pass shows one of them is now mis-sized (e.g. the ring could
shrink once the renderer streams differently), that is a *measured* follow-up, not a
decision this plan owns. Documented here so a reader does not mistake the silent carry-over
for an oversight.

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
| `dm_machine` | **TDD, test-first** | drive command sequences, assert state + rejections; **also covers the §2.7.3 up-calls** (`typing_finished`/`deliver_async`) and the IDLE-suppression rule, so the call-graph inversion is test-first |
| `slot_store` | **TDD, test-first** | fake `dm_nvs` sink; assert dual-write ordering + rollback |
| `dm_feedback`, `dm_events`, new shell | parity / characterization | **parity harness** (§5.2) diffs new vs. old path; the existing `native_sim` snapshots gate the step-8 cut-over |

### 4.1 Dual-mode harness (new — Step 0 of the build)

The repo today has **only** native_sim snapshot tests (`tests/core/*` etc.), discovered
and run by ZMK's `run-test.sh` via `urob/zmk-actions` — that runner finds test cases
**solely by `native_sim.keymap`** and diffs emitted keycode snapshots; it does **not** run
Ztest/Twister (and the `urob` Nix shell lacks Twister's Python deps, with no pip to add
them — so a Twister rail was tried and abandoned). Even ZMK upstream has *no* Ztest suites;
its behavior tests are all keymap-snapshot. So we do not fight the ecosystem:

- **Pure-core tests run as a standalone host binary, in CI via plain `gcc`** — the Zephyr-free
  `.c` modules + test files compiled directly (`tests/unit/Makefile`; `run-host.ps1` is the
  MSVC equivalent), giving a **sub-second loop locally** and a trivial Ubuntu CI job
  (`.github/workflows/host-tests.yml`: `apt gcc` + `make`). This is the *truest* expression
  of ADR-0001's host-testable goal — the pure core tests with nothing but a C compiler. A
  thin `ztest_shim.h` maps `zassert_*` to `assert()`/`printf` off-Zephyr; the same source
  also compiles as Ztest should a Zephyr rail ever be wanted.
- **The standalone compile doubles as a decoupling proof**: if a pure module fails to compile
  without Zephyr, the decoupling has regressed — the host build breaks immediately.
- **Anything that needs the live old code** (the render parity golden capture) uses the
  **keymap-snapshot** mechanism the `urob` job already runs reliably — see §5.2.

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

> **Progress** (updated as steps land):
> - [x] **Step 0** — dual-mode harness. Host rail green locally (`tests/unit/run-host.ps1`, MSVC) and in CI via plain `gcc` (`.github/workflows/host-tests.yml`). (A Twister rail was tried and abandoned — the `urob` Nix shell has no pip for Twister's deps, and ZMK upstream itself uses no Ztest; host-gcc is the rail.)
> - [x] **Step 1** — `dm_render` pure module, test-first, host tests green (8/8 incl. parity). Render parity **proven against the live old walk**: the keymap-snapshot test captured `[DM SAVED R0: '<LCTL+C>#']`; `dm_render` reproduces `<LCTL+C>` (Ctrl+C) and `#` (US Shift+3) exactly. Old walks remain untouched (parallel-stack, §5.0). CI: gcc host job + keymap-snapshot job both green.
> - [x] **Step 2** — locale data tables inside `dm_render`. The per-locale `#if`/`switch` ladders in `printable_char_for_keycode` are now `static const dm_keymap` tables (one per locale, keyed by keycode via a small linear scan; letters stay algorithmic). US/UK behavior unchanged (parity test still green); **plain locales (DE/FR) deliberately tightened** to letters/digits/space only (the old code fell through to the US punctuation branch, but the encoder never emits punctuation on a plain locale, so this is unobservable through real recording — confirmed and chosen 2026-06-09). New behavior pinned by `de_plain_digit` / `de_plain_shifted_punctuation_is_token`. Host tests 10/10. **Footprint checkpoint:** the ARM/gcc toolchain is not on the dev box, so the quantitative `dm_render` flash number is deferred to CI / the step-9 pass; qualitatively the change is flash-neutral-to-negative by construction — small rodata tables (US ≈ 66 B, UK ≈ 69 B, plain ≈ 33 B, shared digit rows folded via macro) replace branch-heavy `switch`/`#if` instruction sequences and per-locale jump tables.
> - [x] **Step 3** — `slot_store` pure module, test-first, host tests green (25/25, +15 store). Dual-write ordering + rollback are internal and pinned by a fake `dm_nvs_sink`: move with dst-persist-enqueue-fail rolls dst back (src intact); dst-ok/src-delete-fail keeps dst safe and surfaces the error; the happy path's `S{dst}D{src}` ordering is asserted via a sink trace. Delete-while-playing skips the RAM zero on completion (`fe3689e`); a stale-generation completion is a no-op; the draft surface (append-to-`MAX_EVENTS`, chain empty/no-room, commit→slot) is covered. New shared headers: `dm_result.h`, `dm_config.h` (host-overridable sizing), `slot_store.h` + white-box `slot_store_priv.h`. Old `cmd_slot`/storage path untouched (parallel-stack). **`dm_result` gained `DM_REJECTED_FULL`** for the chain/draft-overflow rejection — see §2.0.
> - [x] **2026-06-09 review amendments** — the plan was evaluated against the live old
>   code; gaps and contradictions were resolved and folded into §§1, 2.0–2.7, 3, 8.
>   Landed in code immediately (pure modules, test-first, host suite 34/34): the
>   per-op queue-full split (`DM_SAVE_QUEUE_FULL`/`DM_DELETE_QUEUE_FULL`), the
>   RAM-only `draft_commit` + deferred `slot_store_persist`, the
>   `slot_store_load`/`slot_store_reset` restore surface, and the `dm_render_cursor`
>   pause/resume (held-modifier state pinned). Deferred to their natural steps:
>   machine-side contracts (§2.7.2 completed table, move-cancel guard, `DM_CMD_OVERFLOW`,
>   erase up-calls, OFF-path rule — step 4/5), knob ownership + restore wiring (steps
>   5–7), export read-back (step 7).
> - [x] **Step 4** — `dm_machine` pure module, test-first, host tests green (67/67, +33). Two-valued `legality[state][command]` matrix (every cell decided), data-dependent guards behind a callback vtable (never reads slot bytes), `state` written once per transition before any feedback-emitting effect. Ported transition rules pinned: same-slot move = cancel (never hits the store), REC-from-pending discards the take, rejections preserve the pending state (only delete-empty drops to IDLE), per-op queue-full names the right slot. Up-calls land the feedback→machine→state inversion: `typing_finished` (fires deferred persist + applies return-state), `deliver_async` (IDLE-suppression), `erase_due`/`erase_cancel` (park-once). Old behavior path untouched (parallel-stack).
> - [x] **Step 5a** — pure `dm_feedback_build` core, host tests green (86/86, +19). The 23 near-clone `feedback_*` builders collapse into one `dm_feedback_build(spec)` over `static const` message tables; pure, emitting `fb_event` keystrokes to an abstract sink. The preview routes through `dm_render`'s char stream, each char re-encoded via `ascii_to_hid` (the inverse of `printable_char_for_keycode`), so a replayed key types the same keycode the live pump would. Host tests decode each `fb_event` back to its char and assert the message string-for-string vs. the old output (US/UK FULL+ARROW scaffolding, N/R prefixes, status header/slot lines, literal + `<LCTL+C>` token previews).
> - [ ] **Step 5b** — the Zephyr **pump shell** (ring + `k_timer`/`k_work` emit loop + `raise_zmk_keycode_state_changed` + the `speak(spec)` ritual + erase scheduler + status continuation), calling *into* the step-4 machine up-calls. **Deferred to step 7**: it is Zephyr-coupled and cannot run in the host loop, so it is written alongside the `native_sim` e2e parity harness that actually verifies it, rather than landing unexercised. The pure core (5a) is the host-verifiable deliverable; the pump is mechanism with no new decisions.
> - [ ] Steps 6–7 — `dm_events` projection; new shell + pump (5b) + e2e parity harness.
> - [ ] **Step 8** — single cut-over. [ ] **Step 9** — footprint pass.

### 5.0 Method: two fully separate paths; one cut-over at the very end

The new modules are built as a **complete parallel stack**, kept on **entirely separate
paths** from the old code for the whole rewrite. The existing `behavior_dynamic_macro.c` /
`dm_feedback.c` / `dm_storage.c` stay **untouched** — same internals, same call graph,
still wired to ZMK as the sole **live/shipping** path. The old call sites are **never
repointed mid-flight**: we do not splice `dm_render` into `dm_get_preview_string`, nor route
`cmd_slot` through `slot_store`, while the rewrite is in progress.

The new stack (`dm_render`, `slot_store`, `dm_machine`, `dm_feedback`, `dm_events`, the new
shell) is reachable **only from tests** — the `tests/unit` host + Ztest suites and a parallel
`native_sim` parity harness — until the rewrite is complete. A real keypress never enters the
new path until the **single final cut-over** (step 8): the old module is removed wholesale and
the new shell switched in, in one step, once the *entire* new stack is at proven parity.

```
ZMK keypress ─▶ OLD behavior_dynamic_macro      ← live / shipping, untouched
                old dm_feedback / dm_storage

new dm_render / slot_store / dm_machine / …      ← driven ONLY by tests +
   (the parallel stack being built)                a native_sim parity harness,
                                                    never a real keypress until step 8
```

**Why two separate paths, not per-concern repointing:**

- **The old code is an untouched oracle.** "Full feature/config parity" (ADR-0001) is the
  hard requirement. The original keeps running *exactly* as it shipped, so every new module
  is checked against a live reference — not a half-migrated hybrid, not a reconstruction from
  memory or snapshots. When outputs diverge, both are runnable and "which is right?" is
  answerable.
- **No half-migrated intermediate state ever exists.** Because no old call site is repointed
  mid-flight, no commit has a concern split "half old, half new." The old path is green at
  every step *by construction* — it was never modified — and the new path's correctness is a
  separate, test-only question until the end.
- **Parity is proven before exposure.** The riskiest changes (the §2.7.3 call-graph
  inversion, the dual-write ordering) are exercised entirely in the new stack under test,
  with the old behavior available to diff against, before a single real keypress reaches them.
- **The final switch is one reviewable change.** Step 8 is the only place behavior can change
  for a user; it is a single, isolated, snapshot-gated cut-over, not a trail of per-concern
  repoints each carrying its own regression risk.

The cost — both implementations co-resident for the whole rewrite (not just briefly) — is a
transient flash/RAM bump in *intermediate builds only*, never in the shipped artifact: the
old code is removed at step 8, and the footprint pass measures only the final, single-path
state.

### 5.1 Steps

Each step ends with the existing `native_sim` suite green. Pure-core steps (1, 3, 4)
are **test-first**: the red Ztest/host test lands before the module it specifies.

**Why this order** (render → store → machine → feedback): the dependency arrows in §1
point down, and the build climbs them leaf-first so each step rests on already-extracted,
already-tested modules. `dm_render` depends on nothing in the system, so it is the only
module that can be extracted with *zero* coupling to the rest — it is the cheapest proof
of the TDD loop. `slot_store` comes before `dm_machine` because the machine's guards
(§2.2) *ask* `slot_store` for `draft_count`/`is_empty`/`count`; the machine cannot be
test-driven against an interface that does not yet exist (§2.7.4 implementation order
makes this explicit). `dm_feedback` (step 5) comes after the machine because the §2.7.3
up-calls (`typing_finished`/`deliver_async`) it must call *into* are part of the machine's
interface. The order is forced by the contracts, not chosen for convenience.

Every step below **adds to the new parallel stack only**; the old path is untouched and
therefore green at every step by construction (§5.0). "Green" for a new module means its
host + Ztest suites pass and, where applicable, the **parity harness** (§5.2) shows its
output matches the old path's. No old call site is repointed until the single cut-over (step
8).

0. **Stand up the dual-mode harness** (§4.1). A `tests/unit/` Ztest suite that runs
   under `west test`, plus a standalone host target + `ztest_shim.h` for the fast local
   loop. Prove it with one trivial passing test. *No production code moves yet.*
1. **`dm_render`, test-first — new module, parallel.** Write the red render tests (§4.2) →
   build `dm_render` as a pure new module **beside** the existing walks. The old
   `dm_get_preview_string` loop and `render_slot_contents_stream` are **left exactly as they
   are**; nothing is repointed. *Net: a new, pure, host-tested renderer exists; the old path
   is unchanged and still green.*
2. **Locale data tables** inside the new `dm_render`, replacing its `#if DM_LOCALE` logic
   with `static const` tables. The step-1 render tests are the safety net. *(The old path
   keeps its own locale machinery untouched until the cut-over.) Footprint checkpoint: note
   the new module's flash in isolation.*
3. **`slot_store`, test-first — new module, parallel.** Write the red dual-write/rollback
   tests against a fake `dm_nvs` sink → build `slot_store` with the invariant internal,
   **including the draft buffer** (§2.7.4). It is exercised only by its tests; `cmd_slot` and
   the old nvs completion path **still poke `slots[]` as today**. Nothing repointed.
4. **`dm_machine`, test-first — new module, parallel.** Write the red legality-matrix + guard
   tests → build the two-valued `static const` matrix + guard logic as a new module that
   owns its own `state`. **Installs the orchestration + rejection contracts** (§2.7.1–2.7.2)
   and the §2.7.3 up-calls (`typing_finished`/`deliver_async`) + IDLE-suppression rule, all
   asserted test-first. The state-ownership inversion (feedback→machine→state) is the
   riskiest design, so it is **proven entirely in the new stack under test** here, with the
   old behavior available to diff against — *not* spliced into the old `feedback_*` bodies,
   which keep writing their own `state` as today.
5. **`dm_feedback` (new), the `speak` builder.** Build the new feedback module: one
   `speak(spec)` over a message spec, the *rendering-continuation* split per §2.7.3, calling
   *into* the step-4 machine's up-calls. The old `dm_feedback.c` with its 23 clones is
   untouched. Guarded by the **parity harness** (§5.2) against the old feedback output, plus
   targeted tests per deferred-completion path (`save_failed` / `delete_failed` / `deleted`).
6. **`dm_events` (new)** — build the projection module; assert single-instance in one place.
   Old query/notification code untouched.
7. **New `behavior_dynamic_macro` shell** — build the thin wiring/dispatch shell that
   composes the new stack (machine + store + render + feedback + events). Still **not** wired
   to ZMK; reachable only from the parity harness. At the end of this step the *entire* new
   path exists and runs the full `native_sim` parity corpus at parity with the old path.
   **Shell inventory checklist** (definition-of-done; easy to drop at cut-over, all live
   today): build-time keymap validation macros (`DM_VALIDATE_*`), behavior metadata
   tables, param2 range checks returning `-EINVAL`, the BUILD_ASSERTs (8-byte `dm_event`,
   slot limits, single instance, zero-slot warning), `suppress_recording` ownership (the
   shell owns the flag — it owns the listener; emitters set/clear through one
   shell-provided inline), IDLE-only `DM_TEST_RELOAD` dispatch, and the wiring handed to
   `dm_nvs` at init (sink adapter + export read-back references, §2.4).
8. **The cut-over (the one and only switch).** This is the sole step where user-visible
   behavior *can* change. Preconditions: the full new stack (steps 1–7) is green and the
   parity harness (§5.2) shows the new shell matching the old path across the entire
   `native_sim` corpus. Then, in one isolated change: wire ZMK to the new
   `behavior_dynamic_macro` shell and **remove the old `behavior_dynamic_macro.c` /
   `dm_feedback.c` / `dm_storage.c` wholesale.** The existing snapshot suite — unchanged
   since before the rewrite began — runs against the new path; any diff is a cut-over
   regression to explain, not re-bless. Because the old path was never modified, this is the
   first and only commit that can regress a shipping user.
9. **Footprint pass:** with the old path now gone, measure flash/RAM vs. the v0.3.1 baseline;
   claw back only where it demonstrably hurts, never at the cost of a documented invariant.

Steps 0–7 carry **zero regression risk to the shipping path** — they only add a parallel,
test-only stack; the old code runs untouched throughout. All user-facing risk is
concentrated in the single step-8 cut-over, which is therefore the one to rehearse (parity
harness green first) and review in isolation.

### 5.2 The parity harness

Because the new stack is test-only until step 8, its correctness is established by
**differential testing against the old path as oracle** — not just by its own unit tests. The
parity harness drives the *same* input (a command/keycode sequence, or a slot's events) into
both the old implementation and the new module, and asserts identical output:

- **Render parity** (step 1+) — **golden-string**, because the old buffer walk
  (`dm_get_preview_string`) is a *pure deterministic function* of (slot, locale): no async,
  no state, no timing. The old walk lives in Zephyr-coupled code and cannot link into the
  pure host loop, so instead of running it live each iteration we capture its output **once**
  as a golden table, and the fast host (`gcc`) test asserts `dm_render` matches the golden.
  Faithfulness to the live old code is preserved by a **keymap-snapshot capture test**
  (`tests/parity/render/native_sim.keymap`) — it records token-producing keys (Ctrl+C,
  Shift+3, …), saves at VERBOSE + US locale so the **old walk types the preview**, and the
  emitted-keycode snapshot is the live old output. That snapshot is decoded **once** into
  `golden_us.h`; the decode is cross-checked against the independent first-principles
  assertions in `test_render.c`, so the golden is anchored to the old code, not to the new
  renderer. (The keymap-snapshot rail is the one the `urob` CI already runs reliably; a
  bespoke per-corpus keymap was judged not worth the fiddly mock-keypress + decode work for
  the token cases the existing feedback snapshots and a single targeted keymap already cover.)
- **Store parity** (step 3+) — **live `native_sim` diff**, because the old slot handling is
  *not* a pure function: it involves `pending_delete`, generation stamping, async NVS
  completion, and ordering. A static golden could not capture that, so this slice drives
  assign/move/delete sequences (incl. failure injections) into both the old `cmd_slot` path
  and the new `slot_store` live, asserting identical `slots[]` / `pending_delete` /
  generation outcomes.
- **End-to-end parity** (step 7) — **live `native_sim` diff**: run the full command corpus
  through both the old behavior and the new shell; assert identical emitted keycode streams —
  the same thing the snapshot suite checks, now old-vs-new rather than old-vs-recorded.

**Golden vs. live is chosen per slice by whether the old code is pure.** Render is pure →
golden (fast host loop, oracle captured once from the old code). Store and end-to-end carry
state/async/timing → live diff. The harness lives in `tests/` (host for the render golden,
`native_sim` for the capture test and the store/e2e live diffs) and is what "at parity" means
in the step list. It exists only during the rewrite; step 8 removes the old side, after which
the snapshot suite alone is the net.

---

## 6. Open questions for review

Genuinely deferred — implementation-level, cheap to change, decided against real code
at the relevant build step. They do not move module boundaries.

- **Sink vtable cost — cycles settled, flash to confirm:** the runtime vtable is *not*
  optional (§2.3: two sinks coexist; the ring sink is resumable), so its shape is no
  longer open. Its cycle cost is negligible by construction (≈3 indirect calls per char
  vs. a 30 ms `TAP_DELAY` per char). The only thing left to *measure* — not decide — is
  net flash at the step-2 checkpoint; expected neutral-to-negative once the duplicated
  second walk is deleted.
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
| Rewrite method | Build the new modules as a **complete parallel stack** on entirely separate paths; the old code is untouched and stays the sole live path. New stack is **test-only** (unit + parity harness) until **one final cut-over** (step 8) removes the old modules wholesale. No per-concern repointing. | §5.0, §5.2 |
| Module granularity | Seven modules with real interfaces get own files; `tap_cadence` and `playback` are co-located primitives — split where there's a seam, not a noun. | §1 |
| Cadence is not a seam | The two emitters are mutually-exclusive *states* (`emit_work_handler`'s `if (PLAYING) … else if (TYPING)`), so `state` already dispatches; a `dm_emitter` vtable would re-express that for free at a flash/indirection cost. Unlike `dm_render`'s sinks, the emitters never coexist on a call. Deletion test: removing the vtable concentrates no complexity. | §2.5 |
| Handle granularity | Resolved by ADR-0002: the ZMK `dev` pointer is the handle; no separate `dm_context` aggregate. | ADR-0002 |
| Sink dispatch | Runtime vtable, not compile-time-selected: both sinks coexist in one build and the ring sink is resumable. Cycle cost negligible vs. `TAP_DELAY`. | §2.3, §6 |
| Locale vs. style/level | Locale is link-time-fixed (render table); style and level are runtime-mutable and read live — never baked into the render table or the `dm_feedback_spec`. | §2.3, §2.5 |
| Orchestration | The machine runs each command as a transaction (legality → guard → single `state=` before effects → effect); `slot_store`/`dm_feedback` are called *down* into. Not a returned effect-list. | §2.7.1 |
| Rejection return-state | Rejection carries a data-dependent return-state (occupied/empty × moving-or-not); preserves the pending state, timeout is the only escape. Not a uniform drop-to-IDLE. | §2.7.2, §2.2 |
| Completion up-calls | Feedback never writes `state`; it reports up via `dm_machine_typing_finished()` / `dm_machine_deliver_async()`. The deferred IDLE-suppression rule lives in the machine. **Designed and tested in step 4** (new machine, test-first) — the call-graph inversion is the riskiest change, proven in the new stack under test against the old path as oracle, never spliced into the old feedback bodies. | §2.7.3, §5 |
| Draft buffer owner | The recording buffer belongs to `slot_store` (slot-to-slot byte work), behind a `draft_*` surface — not `dm_machine`, not a new recorder module. | §2.7.4 |

**Added at the 2026-06-09 review** (plan evaluated against the live old code):

| Decision | Resolution | Where |
| --- | --- | --- |
| Queue-full granularity | Split per op: `DM_SAVE_QUEUE_FULL` / `DM_DELETE_QUEUE_FULL` (replaced one `DM_QUEUE_FULL`) — a move can fail either way with distinct messages naming different slots. | §2.0 |
| Assign persist timing | `draft_commit` is RAM-only; the machine fires `slot_store_persist` from `typing_finished` (ports `feedback_post_save_slot`). Resolved the §2.1-vs-§2.7.3 contradiction in favor of parity. | §2.1, §2.7.3 |
| Render resumability | Caller-owned `dm_render_cursor {idx, active_mods}`; the draft signature could not actually resume (modifier state lost across the pause). | §2.3 |
| Preview truncation | Stop-at-first-non-fit (honest prefix), deliberately replacing the old skip-and-continue. Observable only with a too-small widget buffer. | §2.3 |
| Restore surface | `slot_store_load` / `slot_store_reset` + `dm_feedback_restore_knobs`: boot/TEST_RELOAD restore is a sanctioned upward *delivery* by `dm_nvs`; validation stays in `dm_nvs`. | §2.1, §2.4 |
| Export read-back | `dm_nvs` holds the instance's `slot_store *`/`dm_feedback` refs from step-7 wiring; reads via public queries only. A plain pointer, not a vtable — nothing adapts the seam (deletion test). | §2.4, ADR-0002 |
| Knob ownership | Level/style/erase live in `dm_feedback`, persist via `dm_nvs_save_knobs`; the ARROW-on-plain-locale rule consolidates into the restore setter (was duplicated). | §2.5 |
| Same-slot move | A cancel handled in the machine guard before `slot_store_move` — never a store rejection. | §2.7.2 |
| Recording overflow | A machine command (`DM_CMD_OVERFLOW`) raised by the shell when `draft_append` returns false; → `PENDING_ASSIGN`. | §2.7.2 |
| Auto-erase surface | Two more machine up-calls (`erase_due` / `erase_cancel`); counting, batching, and the status exclusion stay feedback-internal. | §2.7.3 |
| OFF-path rule | Below-level `speak` calls `typing_finished` synchronously; the machine raises notifications before `speak`, so they fire at every level. Deletes the 20-function stub block. | §2.5, §2.7.3 |
| Threading narrowing | Delete-completion delivered on the system queue; `slot_store` single-threaded by contract; storage thread never touches slot state. | §3 |

---

## 8. Appendix — notification inventory (parity checklist)

`zmk_dynamic_macro_state_changed` notifications, transcribed from the old code.
In the new stack they are raised by **the machine** (via `dm_events`), *before*
feedback speaks — so they fire **regardless of feedback level**, as today (every
old feedback function raises before its level gate). The parity harness asserts
this inventory; widgets depend on it.

| Notification | Raised when |
| --- | --- |
| `RECORDING_STARTED` | REC accepted (incl. re-record / discard-and-restart) |
| `RECORDING_STOPPED` | STP with a non-empty draft |
| `SAVED` | assign committed |
| `DELETED` | RAM delete, or NVS delete-completion (deferred, IDLE-gated speech) |
| `MOVED` | move completed (slot = dst) |
| `PLAY_STARTED` / `PLAY_FINISHED` | playback start / last event emitted |
| `PREVIEW_READY` | slot pressed in PREVIEW_PENDING |
| `ERROR_NO_RECORDING` | STP with an empty draft |
| `ERROR_SLOT_EMPTY` | empty-slot guard rejection (play/delete/move-source/chain) |
| `ERROR_OVERFLOW` | recording draft full |
| `ERROR_SAVE_FAILED` / `ERROR_DELETE_FAILED` | async NVS outcome (deferred) |
| `ERROR_QUEUE_FULL` | save **or** delete enqueue refused (one event type for both, even though the typed messages differ — ported quirk) |

**Deliberately no notification** (ported): slot-occupied rejections
(`feedback_slot_full`), move prompt / source-selected / cancelled, chain insert /
empty / no-room, status output, and the knob commands (level/style/erase). Do not
"complete" these — widgets were never told about them.
