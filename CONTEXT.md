# Context

Domain glossary for `zmk-dynamic-macros` — a ZMK behavior module that records
keystroke sequences at runtime and replays them, with optional NVS persistence,
on-keyboard typed feedback, and a query/event API for display widgets.

This file is the source of truth for domain vocabulary. Code, docs, issues, and
design discussion should use these terms exactly and avoid drifting to synonyms.

## Core concepts

**Slot** — a numbered container holding one recorded macro (an ordered array of
events). Slots are either **NVS slots** (persisted to flash, survive reboot) or
**RAM slots** (volatile). The slot index space is `[0, NVS_SLOTS)` for NVS and
`[NVS_SLOTS, MAX_SLOTS)` for RAM. A slot is **empty** when its event count is
zero or it is flagged `pending_delete`.

**Event** — one captured HID keycode transition: usage page, keycode, implicit
and explicit modifiers, and press/release. The 8-byte `dm_event` is the unit of
both recording and playback. Avoid "keystroke" — one logical keystroke is two
events (press + release).

**Macro** — the full ordered sequence of events stored in one slot. What the
user records and replays.

**Recording** — capturing live keycode events into the **recording buffer**
until stopped. A finished recording is **unassigned** until the user picks a
slot for it (the **pending-assign** step). Recording can **chain** an existing
slot's contents into the buffer.

**Playback** — replaying a slot's events as synthetic keycode events, paced by
`TAP_DELAY`. Playback **suppresses recording** so replayed events are not
re-captured.

**Assign** — committing the recording buffer into a chosen slot. **Move** —
relabeling a macro from a source slot to an empty destination slot. **Delete** —
clearing a slot. For NVS slots, assign/move/delete each imply a persistence op.

## Feedback & rendering

**Feedback** — on-keyboard typed output that narrates what the macro system is
doing (e.g. `[DM REC]`, `[DM SAVED N0]`). Emitted as synthetic keystrokes
through the **emit pump**. Gated by a **feedback level** (OFF → ERROR → COMMAND
→ BASIC → VERBOSE) and rendered in a **style** (FULL or ARROW).

**Preview** — a human-readable rendering of a slot's contents, where literal
text stays literal and non-printable actions become `<TOKENS>` (e.g.
`<LCTL+C>`). Produced two ways that must agree: typed live via the emit pump,
and returned as a string by the query API (`dm_get_preview_string`).

**Locale** — the host keyboard layout (US/UK/DE/FR) that determines how a
keycode maps to a printable character. US/UK are **full-punctuation** locales;
DE/FR are **plain** locales (`DM_LOCALE_PLAIN`) where previews emit only
letters, digits, and space.

**Tap cadence** — the shared timing primitive that fires every `TAP_DELAY` to
drive one emit step at a time, with press→release phasing, until its emitter
signals done. Owns the timer/work-queue lifecycle only — not what is emitted.
_Avoid_: "emit pump" as a single module; the cadence is shared, the emitters are not.

**Emitter** — a consumer of the tap cadence that produces one kind of output:
the **playback emitter** replays a slot's `dm_event`s (reading the slot store),
and the **feedback emitter** types `fb_event`s from the ring with streaming
preview refill. Two emitters, one cadence; their emit bodies do not share.

## Instance model

**Instance** — one configured `zmk,behavior-dynamic-macro` devicetree node, with
its own `dev->data`. Module interfaces are **per-instance** per ZMK convention,
but the behavior is hard **single-instance** (build-asserted), and some internals
(the whole storage layer, query resolution, listener suppression) deliberately
assume one instance rather than pay for unreachable multi-instance correctness.
_Avoid_: "device" for the logical macro system; reserve "device" for the Zephyr
`struct device`.

## State

**State machine** — the system is always in exactly one **state**: IDLE,
RECORDING, PENDING_ASSIGN, DELETE_PENDING, MOVE_PENDING, PREVIEW_PENDING,
PLAYING, TYPING_FEEDBACK, or TYPING_ERASE. A **transition** moves between
states; the set of legal transitions is the machine's contract.

**Legality** — whether a command does anything in the current state, decided by
a static `[state][command]` matrix with two verdicts: **ALLOWED** (the command
proceeds) or **IGNORED** (silently dropped, no feedback). Legality is a function
of `(state, command)` alone.

**Guard** — a runtime check *inside* an ALLOWED transition that depends on live
data (slot occupancy, recording-buffer room, whether a move source is selected).
A failed guard produces a rejection **outcome** (a `dm_result`) with feedback —
distinct from an IGNORED command, which is silent. _Avoid_: conflating "ignored"
(silent, illegal-in-state) with "rejected" (allowed but guard-failed, with feedback).

**Outcome** — the `dm_result` of a transition: success or a specific failure
(queue-full, save-failed, delete-failed, slot-occupied, slot-empty). One outcome
type spans synchronous returns and asynchronous (deferred) delivery; some
outcomes only ever arrive late.

## Persistence

**Storage backend** — the NVS/`settings` layer that serializes slots to flash on
a dedicated work queue, asynchronously from the behavior thread. A **storage op**
(save / delete / save-feedback) is enqueued and processed off-thread.

**Dual-write invariant** — a move is two non-atomic NVS ops (write dst, delete
src). They are ordered so the only reachable failure is a benign transient
duplicate, never data loss. This invariant is load-bearing; see ADR-0001.

**Generation** — a per-slot counter (`slot_generation`) used to detect stale
async ops: a completion that arrives for an out-of-date generation is ignored.

## Query & events

**Query API** — read-only functions (`dm_get_state`, `dm_get_preview_string`,
slot counts, …) for display widgets. A **read projection** over the live state
of a single behavior instance, safe under ZMK's cooperative threading. Widgets
query by slot index, not by device, so the projection resolves the one instance
internally (see [single-instance internals](docs/adr/0002-single-instance-internals.md)).

**Event** *(overloaded — disambiguate)* — at the HID layer, a captured/replayed
keycode transition (`dm_event`). At the widget layer, a `zmk_dynamic_macro_state_changed`
**notification** raised when something changes. Prefer "notification" for the
latter when both are in scope.
