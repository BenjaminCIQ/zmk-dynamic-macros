# Implementation plan: arena-backed slot storage

- **Status:** Proposed (not yet implemented)
- **Target branch:** built on top of `docs/architecture-redesign` (the modular
  `DM_NEW_STACK` only — the legacy single-path stack is untouched)
- **Goal:** stop paying worst-case RAM for every macro slot. Let a shared event
  pool be redistributed between long and short macros, with one user-facing knob
  that controls total RAM and degrades gracefully ("pool full") instead of
  silently truncating.

This plan is meant to be followed step-by-step without re-deriving the design.
Every change references the real symbol / file / line it touches on the
`docs/architecture-redesign` branch.

---

## 1. What changes, in one paragraph

Today each stored slot owns a fixed `struct dm_event events[MAX_EVENTS]` array, so
`SLOT_CAPACITY` slots cost `SLOT_CAPACITY × MAX_EVENTS` events of RAM whether full
or empty. We replace the per-slot arrays with **one shared event arena**
(`events_arena[ARENA_EVENTS]`) plus a tiny per-slot descriptor
(`meta[SLOT_CAPACITY] = {start, count}`). A slot's events live contiguously at
`events_arena[meta[i].start .. +count]`. Free space is reclaimed by **lazy
compaction** at the only points that allocate (draft-commit, move, boot-load),
all of which are guaranteed not to be mid-playback. The recording **draft buffer**
and the **NVS serialization buffers** stay full-size (`MAX_EVENTS`), because they
are staging/transfer buffers, not stored slots.

---

## 2. Design decisions (locked)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Keep `struct dm_slot { uint32_t event_count; struct dm_event events[MAX_EVENTS]; }` as the **staging/transfer** type (draft + NVS op + NVS buffers). Do **not** use it for stored slots. | The draft can't know its final length while recording, so it needs a full-size buffer. NVS copies must be size-`MAX_EVENTS` for async safety. |
| D2 | Stored slots become `events_arena[ARENA_EVENTS]` + `meta[SLOT_CAPACITY]`. | This is the RAM win: pay for total expected use, not worst case. |
| D3 | **Two knobs only:** keep existing `MAX_EVENTS` (per-macro cap, also sizes the draft); add `AVG_EVENTS_PER_SLOT` → `ARENA_EVENTS = AVG_EVENTS_PER_SLOT × SLOT_CAPACITY`. **No min/floor knob.** | Decided in design discussion: a floor adds a knob + accounting for a rare case the graceful "pool full" path already covers. `MAX_EVENTS` is kept (not renamed) so existing user configs don't break — its Kconfig prompt already says "per macro slot". |
| D4 | `slot_store_get()` returns a **view** by value (empty → `{0, NULL}`), instead of `const struct dm_slot *`. The view type is the **existing** `dm_render_slot_view { uint32_t event_count; const struct dm_event *events; }` — *not* a new `dm_slot_view`. To avoid `slot_store` depending on the renderer, the struct is hoisted into `dm_event.h` (the lowest header both already include) as `struct dm_slot_view`, and `dm_render.h` keeps `dm_render_slot_view` as a typedef alias so all renderer/query/build code compiles unchanged. | Stored events live in the arena, not in a `dm_slot`. Every new-stack caller already only needs count + pointer; three of four already build a `dm_render_slot_view` from it. **One** view type at this seam — a second identically-shaped struct would be a shallow wrapper whose only job is field-for-field copying (the original two-view sketch spread that copy across `dm_events.c`, `dm_feedback_pump.c`, and the nvs export). |
| D5 | The `dm_nvs_sink.save` signature changes from `(…, const struct dm_slot *s, …)` to `(…, const struct dm_event *events, uint32_t count, …)`. | The store no longer has a `dm_slot` to hand over. Avoids a ~516 B stack temp, matching the codebase's "static buffers, no big stack" convention. |
| D6 | **Lazy compaction**: `delete` only sets `count = 0` (leaves a hole); compaction runs inside the allocators (`draft_commit`, `move`, `load`). Compaction is **guarded at the store interface**: `arena_repack` refuses (returns "can't compact") whenever `playing_slot != -1`, and the allocators map that refusal to `DM_REJECTED_FULL` rather than relocating bytes. So compaction can never move bytes out from under a live playback pointer — the safety is a runtime guard in `slot_store`, not an assumption about which machine state called in. | Ports the existing playing-slot invariant (`fe3689e`) into the store itself, where it is testable at the store's own interface. The machine-state argument ("commit/move only run outside PLAYING") is then a *belt*, not the only line of defence. |
| D7 | `move` becomes a **descriptor reassignment** (`meta[dst] = meta[src]`, no event copy, no extra space). During the window between the reassignment and `free_slot(src)`, **two meta entries alias one arena region** — so that window must contain **no allocator call** (no `arena_repack`). `move` calls none; it is asserted (`arena_live <= ARENA_EVENTS` post-move) rather than trusted. | The arena makes move cheaper than today: dst aliases src's region until src is freed; the dual-write ordering is unchanged. The aliasing is safe only because nothing compacts inside the window — documented + asserted so a future edit that adds an allocator there fails loudly instead of double-packing the shared region. |
| D8 | A commit/move/load that doesn't fit returns `DM_REJECTED_FULL`. `slot_assign` (`dm_machine.c:233`) already treats any non-`DM_OK` from `draft_commit` as a failure that stays in `PENDING_ASSIGN`. | Graceful degradation with near-zero machine change. Optional polish in §9 adds a distinct "[DM POOL FULL]" message. |

**Default for `AVG_EVENTS_PER_SLOT`:** ship **32** (with `MAX_EVENTS` default 64).
That halves the default slot-storage RAM (arena = 32×16 = 512 events vs today's
16×64 = 1024) while still letting any single macro reach 64. Setting
`AVG_EVENTS_PER_SLOT == MAX_EVENTS` reproduces today's "every slot guaranteed
full" behavior with no savings. *(This is the one product value worth a second
look — see §10.)*

---

## 3. Data structures: before → after

### `struct slot_store` (`include/zmk-behavior-dynamic-macros/slot_store_priv.h`)

```c
/* BEFORE */
struct slot_store {
    struct dm_slot slots[SLOT_CAPACITY];
    bool           pending_delete[SLOT_CAPACITY];
    uint32_t       slot_generation[SLOT_CAPACITY];
    struct dm_slot draft;
    int            playing_slot;
    const dm_nvs_sink *sink;
};

/* AFTER */
struct dm_slot_meta {
    uint16_t start;   /* offset into events_arena[] (valid iff count > 0) */
    uint16_t count;   /* live events; 0 == empty slot                    */
};

struct slot_store {
    struct dm_event     events_arena[ARENA_EVENTS]; /* shared pool        */
    struct dm_slot_meta meta[SLOT_CAPACITY];
    bool                pending_delete[SLOT_CAPACITY];
    uint32_t            slot_generation[SLOT_CAPACITY];
    struct dm_slot      draft;        /* unchanged: full-size staging buf */
    int                 playing_slot; /* -1 when nothing is playing       */
    const dm_nvs_sink  *sink;
};
```

`uint16_t` for `start`/`count` is safe: `ARENA_EVENTS ≤ AVG(≤?)×64`; assert it fits
(see §7). `MAX_SLOTS ≤ 64`.

### View type (hoisted into `dm_event.h`, aliased in `dm_render.h`)

```c
/* dm_event.h — the lowest header slot_store.h and dm_render.h both include */
struct dm_slot_view {
    uint32_t               event_count;
    const struct dm_event *events;   /* points into the arena; NULL if empty */
};
```

```c
/* dm_render.h — keep the renderer's spelling as an alias; no caller churn */
typedef struct dm_slot_view dm_render_slot_view;
```

This is **one** type with two names, not two types. `slot_store_get` returns it;
the renderer/query/build code keeps saying `dm_render_slot_view`. The feedback,
events, and nvs-export callers stop copying fields — they pass the view straight
through.

---

## 4. RAM math (default `MAX_EVENTS=64`, `SLOT_CAPACITY=16`)

| Component | Today | Arena, `AVG=32` | Arena, `AVG=64` (parity) |
|---|---|---|---|
| stored slots | 16×516 = 8256 B | arena 512×8 = 4096 B | arena 1024×8 = 8192 B |
| meta[16] | — | 64 B | 64 B |
| draft | 516 B | 516 B | 516 B |
| pending/generation/etc | ~92 B | ~92 B | ~92 B |
| **total** | **≈ 8864 B** | **≈ 4768 B** (~46% less) | **≈ 8864 B** (same) |

The win scales with how far below worst-case `AVG` is set; `AVG=MAX_EVENTS` is the
zero-savings parity setting.

---

## 5. File-by-file changes

> Scope reminder: only the `DM_NEW_STACK` files. **Do not touch**
> `behavior_dynamic_macro.c`, `dm_feedback.c`, `dm_storage.c`, `dm_internal.h`
> (the legacy stack, gated by the `else()` branch in `CMakeLists.txt`).

### 5.1 `include/zmk-behavior-dynamic-macros/dm_config.h`

Add the arena sizing next to the existing slot sizing. Firmware pulls from Kconfig;
the host build keeps test-representative defaults.

```c
/* firmware (__ZEPHYR__) block, alongside MAX_EVENTS/NVS_SLOTS/RAM_SLOTS: */
#ifndef AVG_EVENTS
#define AVG_EVENTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_AVG_EVENTS_PER_SLOT
#endif

/* host block (no Kconfig): keep small enough that a unit test can fill the pool */
#ifndef AVG_EVENTS
#define AVG_EVENTS 16            /* ARENA_EVENTS = 16 × 16 = 256 on host */
#endif

/* after SLOT_CAPACITY is defined: */
#ifndef ARENA_EVENTS
#define ARENA_EVENTS (AVG_EVENTS * SLOT_CAPACITY)
#endif
```

Keep `MAX_EVENTS` exactly as-is.

### 5.2 `include/zmk-behavior-dynamic-macros/slot_store.h`

1. No new struct here — the view (`struct dm_slot_view`) lives in `dm_event.h`
   (§3), which `slot_store.h` already pulls in transitively via `dm_event.h`.
2. Change the accessor:
   ```c
   struct dm_slot_view slot_store_get(const slot_store *s, int idx); /* {0,NULL} if empty */
   ```
3. Change the sink `save` member (D5):
   ```c
   dm_result (*save)(void *ctx, int slot,
                     const struct dm_event *events, uint32_t count, uint32_t generation);
   ```
4. Update the doc comment on `slot_store_load` / `draft_commit` to mention the new
   `DM_REJECTED_FULL` (arena-full) outcome.

`struct dm_slot` itself stays defined here (staging type), unchanged.

### 5.3 `include/zmk-behavior-dynamic-macros/slot_store_priv.h`

Replace `slots[SLOT_CAPACITY]` with `events_arena[ARENA_EVENTS]` + add
`struct dm_slot_meta meta[SLOT_CAPACITY]` (see §3). `draft`, `pending_delete`,
`slot_generation`, `playing_slot`, `sink` stay.

### 5.4 `src/slot_store.c` — the core rewrite

Add two static helpers:

```c
/* sum of live (count>0) events, including pending-delete slots (their bytes
 * are still parked in the arena until completion frees them). */
static uint16_t arena_live(const slot_store *s) {
    uint16_t n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) n += s->meta[i].count;
    return n;
}

/* Repack all live slots to the low end in index order, writing the first free
 * offset (== arena_live) to *out_free. Returns false WITHOUT touching anything if
 * a slot is playing: relocating bytes would dangle the playback pointer the emit
 * handler holds (see D6 — this is the store-level enforcement of fe3689e, not a
 * caller assumption). memmove because shifted regions may overlap. */
static bool arena_repack(slot_store *s, uint16_t *out_free) {
    if (s->playing_slot != -1) {
        return false;             /* compaction unsafe while playing */
    }
    uint16_t w = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (s->meta[i].count == 0) continue;
        if (s->meta[i].start != w) {
            memmove(&s->events_arena[w], &s->events_arena[s->meta[i].start],
                    (size_t)s->meta[i].count * sizeof(struct dm_event));
            s->meta[i].start = w;
        }
        w += s->meta[i].count;
    }
    *out_free = w;
    return true;
}
```

The guard returns `DM_REJECTED_FULL` / `false` up through the allocators: a
commit or load that arrives while a slot plays is rejected, not corrupting — the
exact same graceful "pool full" path the user already sees, and `slot_assign`
already routes to. In practice the machine never issues an allocating command
during `PLAYING`, so the guard is rarely hit; it exists so the *store* is safe
even if that ever changes.

Function-by-function:

| Function | Change |
|---|---|
| `slot_empty` | `return s->meta[idx].count == 0 \|\| s->pending_delete[idx];` |
| `zero_slot` → `free_slot` | `s->meta[idx].count = 0;` (no memset needed — bytes return to the pool) |
| `slot_store_init` | unchanged (`memset` zeroes meta/arena; set `playing_slot=-1`, `sink`) |
| `slot_store_get` | return `{ meta[idx].count, &events_arena[meta[idx].start] }`, or `{0,NULL}` if invalid/empty |
| `slot_store_count` | keep using the `slot_empty()` helper (which ORs in `pending_delete`) — do **not** inline `meta[i].count != 0`, or a pending-delete slot would be miscounted as present |
| `slot_empty` / `draft_chain` | `draft_chain` keeps its `slot_empty(s, src)` guard, so a pending-delete src is rejected before its (still-parked) arena bytes are read |
| `nvs_save` | `return s->sink->save(ctx, idx, &s->events_arena[s->meta[idx].start], s->meta[idx].count, gen);` |
| `nvs_delete` | unchanged |
| `slot_store_move` | descriptor reassignment (D7): `meta[dst]=meta[src]` in step 1; on save-fail roll back with `meta[dst].count=0`; on success `free_slot(src)`. Ordering + generation bumps identical to today. **No event copy, no allocator call** in the aliased window. End with `DM_ASSERT(arena_live(s) <= ARENA_EVENTS)` so a future allocator slipped into the window trips immediately. |
| `slot_store_persist` | unchanged (delegates to `nvs_save`) |
| `slot_store_delete` | RAM path: `meta[idx].count = 0;` NVS path: unchanged (pending + enqueue) |
| `slot_store_complete_delete` | replace `zero_slot(idx)` with `free_slot(idx)`; keep the `playing_slot != idx` guard and the generation/pending checks verbatim |
| `slot_store_draft_*` | `reset`/`append`/`count` unchanged (operate on `s->draft`, still `MAX_EVENTS`). `draft_chain`: read source from `&s->events_arena[meta[src].start]` and `meta[src].count` instead of `s->slots[src]`; the `MAX_EVENTS` cap check is unchanged |
| `slot_store_draft_commit` | new body (below) |
| `slot_store_load` | new body (below) |
| `slot_store_reset` | `for i: meta[i] = (struct dm_slot_meta){0,0}; pending_delete[i]=false; slot_generation[i]=0;` (draft untouched, as today) |
| `mark_playing`/`clear_playing` | unchanged |

`slot_store_draft_commit`:
```c
dm_result slot_store_draft_commit(slot_store *s, int dst) {
    if (!idx_valid(dst) || !slot_empty(s, dst)) return DM_REJECTED_OCCUPIED;
    uint32_t n = s->draft.event_count;             /* n <= MAX_EVENTS by construction */
    uint16_t used;
    if (!arena_repack(s, &used)) return DM_REJECTED_FULL;  /* playing ⇒ can't compact */
    if (n > (uint32_t)(ARENA_EVENTS - used)) return DM_REJECTED_FULL;
    if (n > 0)
        memcpy(&s->events_arena[used], s->draft.events, (size_t)n * sizeof(struct dm_event));
    s->pending_delete[dst] = false;
    s->slot_generation[dst]++;
    s->meta[dst] = (struct dm_slot_meta){ .start = used, .count = (uint16_t)n };
    return DM_OK;                                  /* RAM-only; persist is separate */
}
```

`slot_store_load` (boot restore / TEST_RELOAD):
```c
bool slot_store_load(slot_store *s, int idx, const struct dm_event *events, uint32_t count) {
    if (!idx_valid(idx) || count > MAX_EVENTS) return false;
    s->meta[idx].count = 0;                        /* free any prior occupant first */
    uint16_t used;
    if (!arena_repack(s, &used)) return false;     /* boot/reload ⇒ not playing; guard anyway */
    if (count > (uint32_t)(ARENA_EVENTS - used)) return false; /* arena overflow */
    if (count > 0)
        memcpy(&s->events_arena[used], events, (size_t)count * sizeof(struct dm_event));
    s->meta[idx] = (struct dm_slot_meta){ .start = used, .count = (uint16_t)count };
    s->pending_delete[idx] = false;                /* no generation bump (load contract) */
    return true;
}
```

### 5.5 `src/dm_nvs.c`

- `sink_save` (line 248): new signature, pass through to `enqueue`:
  ```c
  static dm_result sink_save(void *ctx, int slot,
                             const struct dm_event *events, uint32_t count, uint32_t generation) {
      (void)ctx;
      return enqueue(DM_STORAGE_OP_SAVE, slot, events, count, generation);
  }
  ```
- `enqueue` (line 227): take `events`+`count` instead of `const struct dm_slot *`:
  ```c
  static dm_result enqueue(enum dm_storage_op_type type, int slot_idx,
                           const struct dm_event *events, uint32_t count, uint32_t generation) {
      struct dm_storage_op op = {0};
      op.type = type; op.slot_idx = slot_idx; op.generation = generation;
      op.slot.event_count = count;
      if (events != NULL && count > 0)
          memcpy(op.slot.events, events, (size_t)count * sizeof(struct dm_event));
      /* …unchanged k_msgq_put / submit / queue-full mapping… */
  }
  ```
  `sink_del` → `enqueue(DM_STORAGE_OP_DELETE, slot, NULL, 0, generation)`.
- Export (line 423): `slot_store_get` now returns a view —
  ```c
  struct dm_slot_view v = slot_store_get(dm_store, i);
  if (v.events == NULL) continue;
  header.event_count = v.event_count;
  memcpy(export_buf + sizeof(struct dm_slot_header), v.events,
         (size_t)v.event_count * sizeof(struct dm_event));
  ```
- The `op.slot` field, `save_buf`/`load_buf`/`export_buf` (all `MAX_EVENTS`-sized)
  and `slot_store_load` call stay **unchanged**. The boot-load `event_count > MAX_EVENTS`
  guard stays (the arena-overflow check lives in `slot_store_load`, which logs via
  its `false` return).

### 5.6 `src/behaviors/behavior_dynamic_macro_v2.c`

- `playback_work_handler` (lines 292–316): swap the pointer for the view:
  ```c
  struct dm_slot_view slot = slot_store_get(&inst->store, inst->playback_slot);
  if (slot.events == NULL || inst->playback_event >= slot.event_count) { playback_finish(inst); return; }
  const struct dm_event *ev = &slot.events[inst->playback_event++];
  …
  if (inst->playback_event >= slot.event_count) { playback_finish(inst); }
  ```
  (Indices are re-read from the view each tick exactly as today; the events
  pointer is valid for the duration of the handler — no compaction runs during
  `PLAYING`.)
- Add BUILD_ASSERTs near the existing block (lines 55–59):
  ```c
  BUILD_ASSERT(AVG_EVENTS >= 1, "AVG_EVENTS_PER_SLOT must be at least 1");
  BUILD_ASSERT(MAX_EVENTS <= ARENA_EVENTS,
               "A single macro (MAX_EVENTS) cannot exceed the shared pool (AVG_EVENTS_PER_SLOT × slots)");
  BUILD_ASSERT(ARENA_EVENTS <= UINT16_MAX, "arena offsets must fit in uint16_t");
  ```

### 5.7 `src/dm_feedback_pump.c` (lines 119–135)

`slot_store_get` returns a view; both call sites already want a view. With the
single view type, `slot_view()` collapses to the accessor itself:
```c
struct dm_slot_view v = slot_store_get(f->store, spec->slot);
facts.slot_is_empty       = (v.events == NULL);
facts.preview_event_count = (int)v.event_count;     /* was s ? s->event_count : 0 */
…
/* slot_view(f, slot) becomes a one-liner — get already returns the render view: */
static dm_render_slot_view slot_view(const dm_feedback *f, int slot) {
    return (slot >= 0) ? slot_store_get(f->store, slot)
                       : (dm_render_slot_view){0, NULL};
}
```

### 5.8 `src/dm_events.c` (lines 86–90, 109–115, 128–134)

Same swap to the view at all three `slot_store_get` sites. Because `get` now
returns the render view directly, `view_for()` (line 85) becomes a pass-through:
```c
static dm_render_slot_view view_for(slot_store *store, int slot_idx) {
    return slot_store_get(store, slot_idx);   /* {0,NULL} when empty */
}
```
`dm_get_slot_events` (line 101) and `dm_get_preview_string` (line 118) read the
view's fields: `struct dm_slot_view v = slot_store_get(...); *count = v.event_count;
return v.events;` — the `s == NULL` empty check becomes `v.events == NULL`.

### 5.9 `Kconfig`

After `ZMK_BEHAVIOR_DYNAMIC_MACRO_MAX_EVENTS` (line 9):
```kconfig
config ZMK_BEHAVIOR_DYNAMIC_MACRO_AVG_EVENTS_PER_SLOT
    int "Average events per slot (sizes the shared recording pool)"
    default 32
    range 1 64
    help
      All macro slots share one recording pool sized AVG_EVENTS_PER_SLOT × number
      of slots. A single macro can be longer than this average, as long as the
      total recorded across all slots stays within the pool — so a few long
      macros leave less room for the rest. A typical keystroke is 2 events
      (press + release). Set this equal to "Maximum events per slot" to guarantee
      every slot can be filled to the maximum at once (no pool sharing, no RAM
      savings).
```
Update the `MAX_EVENTS` help text to: *"Maximum events a single macro can hold.
Also sizes the in-progress recording buffer. Must be ≤ AVG_EVENTS_PER_SLOT ×
slots."*

### 5.10 `dm_result.h` + machine + feedback (graceful "pool full")

**Minimal (recommended for v1):** nothing to change. `draft_commit`/`load`/`move`
return `DM_REJECTED_FULL`, and `slot_assign` (`dm_machine.c:234`) already routes
any non-`DM_OK` to `speak(DM_FB_SLOT_FULL)` + stay in `PENDING_ASSIGN`. The macro
is simply not stored; the user sees the existing "slot full" feedback.

**Optional polish (accurate message):** add `DM_REJECTED_NO_SPACE` to
`dm_result.h`; return it (instead of `DM_REJECTED_FULL`) from the arena-full paths;
in `slot_assign` pick the message:
`speak(m, rc == DM_REJECTED_NO_SPACE ? DM_FB_POOL_FULL : DM_FB_SLOT_FULL, …)`.
Adding `DM_FB_POOL_FULL` means mirroring `DM_FB_SLOT_FULL` everywhere it appears —
the spec id enum (`dm_spec.h`), the builder (`dm_feedback_build.c`), and any style
tables. Treat this as a separate follow-up commit so the core RAM change lands
clean.

---

## 6. Invariants preserved (port checklist)

All of these are existing `slot_store` invariants; the rewrite must keep them, now
expressed over `meta`/arena instead of `slots[]`:

- [ ] Move dual-write ordering (`a2865b3`): dst persisted before src freed;
      dst-save-enqueue fail rolls dst back, src intact; src-delete-enqueue fail
      leaves dst safe and surfaces `DM_DELETE_QUEUE_FULL`.
- [ ] Don't free a **playing** slot on delete completion (`fe3689e`): the
      `playing_slot != idx` guard in `slot_store_complete_delete` stays.
- [ ] Generation-stamped completions: stale completion ignored.
- [ ] `draft_commit` is RAM-only; persist is the separate deferred step.
- [ ] Load is a raw populate: no sink echo, no generation bump, clears pending.
- [ ] **New invariant (D6):** the arena is only ever compacted while
      `playing_slot == -1`. Enforced at the **store interface**: `arena_repack`
      returns false (→ `DM_REJECTED_FULL` / load `false`) when a slot plays.
      Tested directly at the store: `mark_playing(x)` then a `draft_commit` that
      needs compaction → rejected, no bytes moved (R1). Plus the
      playing + pending-delete intersection (R3): a pending-delete *playing* slot's
      bytes are never relocated, because the only thing that would relocate them
      is a repack, which the guard blocks while playing.
- [ ] **Move aliasing window (D7):** no allocator runs between `meta[dst]=meta[src]`
      and `free_slot(src)`; asserted by `arena_live <= ARENA_EVENTS` at move's end.

---

## 7. Build-time guards

In `behavior_dynamic_macro_v2.c` (§5.6). The `MAX_EVENTS ≤ ARENA_EVENTS` assert is
the one that turns a nonsensical config (a single macro that can't fit the whole
pool) into a build error.

---

## 8. Test impact

### 8.1 Host unit tests — `tests/unit/test_slot_store.c` (white-box)

This file reads the private layout directly, so it needs mechanical updating plus
new cases. **No Makefile change** (arena sizing comes from `dm_config.h` host
defaults; with `AVG_EVENTS=16`, `ARENA_EVENTS=256` — big enough for the existing
fixtures, small enough to fill in a new test).

**Rewrite the `seed_slot` helper** to bump-allocate in the arena instead of writing
a per-slot array:
```c
static void seed_slot(slot_store *s, int idx, uint32_t n) {
    uint16_t start = arena_live_test(s);   /* sum of meta[].count; or track a cursor */
    for (uint32_t i = 0; i < n; i++)
        s->events_arena[start + i].keycode = (uint16_t)(0x04 + i);
    s->meta[idx].start = start;
    s->meta[idx].count = (uint16_t)n;
}
```
(Define a tiny local `arena_live_test` summing `meta[].count`, or keep a static
cursor reset in `fresh_store`. Seeding in ascending index order keeps regions
non-overlapping.)

**Mechanical assertion substitutions** (every occurrence):
- `s->slots[X].event_count`         → `s->meta[X].count`
- `s->slots[X].events[i].keycode`   → `s->events_arena[s->meta[X].start + i].keycode`

Affected existing tests (assertions only; the behavior under test is unchanged):
`move_dst_persist_fail_rolls_back`, `move_src_delete_fail_keeps_dst`,
`move_happy_orders_dst_then_src`, `move_occupied_dst_rejected`,
`delete_while_playing_skips_zero`, `delete_complete_zeroes_idle_slot`,
`stale_completion_ignored`, `draft_commit_copies_to_slot_ram_only`,
`persist_*`, `delete_enqueue_full_rolls_back_pending`,
`draft_chain_appends_slot_events`, `load_populates_without_persist_echo`,
`load_rejects_overflow`, `reset_clears_slots_pending_generations`.

The fake sink's `fake_save` must adopt the new signature (D5):
```c
static dm_result fake_save(void *ctx, int slot,
                           const struct dm_event *events, uint32_t count, uint32_t generation) {
    (void)events; (void)count; /* … rest identical … */
}
```

**No tests removed.** Every invariant still has a home.

**New unit tests to add:**
1. `commit_full_pool_rejected` — fill the arena (seed slots summing to
   `ARENA_EVENTS`), then `draft_commit` a non-empty draft → `DM_REJECTED_FULL`;
   target slot stays empty.
2. `delete_then_commit_reclaims_space` — fill the arena, delete one slot
   (RAM slot, immediate), then commit a macro that only fits if the freed space
   was reclaimed by compaction → `DM_OK`. Proves lazy compaction.
3. `move_reuses_region_no_extra_space` — fill the arena to exactly `ARENA_EVENTS`
   with one slot, `move` it to an empty slot → `DM_OK` (descriptor reassignment
   needs no free space); src freed, dst holds the events, `arena_live` unchanged.
4. `load_rejects_arena_overflow` — load slots until the next would exceed
   `ARENA_EVENTS` → that `slot_store_load` returns `false`, slot untouched.
5. `compaction_repacks_after_hole` — seed slots A,B,C; delete B; commit D;
   assert `meta[].start` values are contiguous (no permanent hole).
6. `commit_while_playing_rejected_no_move` (R1) — fill the arena leaving a hole,
   `mark_playing` some slot, `draft_commit` a macro that would need compaction to
   fit → `DM_REJECTED_FULL`; assert every `meta[].start` is unchanged (no bytes
   moved). `clear_playing`, retry → `DM_OK` (proves the guard, not the math,
   blocked it).
7. `play_pending_delete_bytes_pinned` (R3) — seed a playing slot, mark it
   pending-delete, then attempt an allocating commit on another slot → rejected
   while playing; the playing slot's `start`/`count` are untouched.

### 8.2 Host unit tests — others

`test_render.c`, `test_query.c`, `test_feedback_build.c`, `test_machine.c` build
their own `dm_render_slot_view`/fakes and **do not** touch `slot_store` internals
→ no change. Confirm they still compile (the `slot_store.h` view type is additive).

### 8.3 Integration / `native_sim` snapshot tests (`tests/core`, `tests/events`, `tests/feedback`)

These are black-box (keymap → `keycode_events.snapshot`); they don't read
internals. They record only a few short macros, well under `ARENA_EVENTS` at the
default `AVG=32` (pool = 512), so **expected to pass unchanged**.

Procedure:
1. Run the full `west test` suite after the change.
2. If any snapshot test now records more total events than its pool and changes
   output, pin that test's `native_sim.conf` with
   `CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_AVG_EVENTS_PER_SLOT=64` (parity) — do **not**
   blanket-edit all confs.

**New `native_sim` test added — `tests/core/pool_full/`** (`overflow/` as the
template). **New-stack-only:** the shared pool and its "full" rejection do not
exist on the legacy per-slot stack (which would simply store all three macros),
so this case bakes `CONFIG_…_NEW_STACK=y` into its own `native_sim.conf` and is
listed in `generate.sh`'s `EXCLUDE` (no legacy oracle to mirror, so no parity
twin).
- `native_sim.conf`: `…_MAX_EVENTS=8`, `…_AVG_EVENTS_PER_SLOT=2`,
  `…_RAM_SLOTS=4` (pool = 2×4 = **8** events, == `MAX_EVENTS` so the
  `MAX_EVENTS ≤ ARENA_EVENTS` build guard holds — a 3-slot/pool-6 config would
  fail to build), `PERSIST=n`, `FEEDBACK_OFF=y`, `ASSIGN_TIMEOUT=500`,
  `NEW_STACK=y`.
- `native_sim.keymap`: record A (2 ev) → slot 0; record B B (4 ev) → slot 1;
  record C (2 ev) → slot 2 (pool now 8/8 full); record D (2 ev) → assign slot 3
  must be **rejected** (stays PENDING_ASSIGN); wait past the timeout to cancel;
  play slot 3 (empty, no output) then slots 0,1,2 (intact).
- `keycode_events.snapshot` / `events.patterns`: with `FEEDBACK_OFF` the snapshot
  is pure record-passthrough + playback — `A / B B / C / D` (live record, incl.
  the rejected take's passthrough) then `A / B B / C` (playback of the three
  stored slots), and **nothing** for slot 3, proving the fourth macro was not
  stored.
  **The committed snapshot is the hand-derived expectation; it must be confirmed
  by one `west test` record run** (the new-stack native_sim build can't run on the
  Windows host loop). CI is the first place it actually executes.

---

## 9. Suggested commit sequence (keeps the tree green at each step)

1. **Config + asserts**: `dm_config.h`, `Kconfig`, BUILD_ASSERTs. (No behavior
   change yet; `ARENA_EVENTS` defined but unused.)
2. **Header contracts**: add `struct dm_slot_view`, change `slot_store_get` +
   sink `save` signatures in `slot_store.h`; update the four call sites
   (`v2`, `pump`, `events`, `nvs` export) and `dm_nvs.c` `sink_save`/`enqueue` and
   the test `fake_save`. Compiles, behavior identical (still array-backed).
   *(Optional: do this step with the array still in place by having
   `slot_store_get` build a view from `slots[idx]` — lets the API change land and
   be tested before the storage swap.)*
3. **Storage swap**: `slot_store_priv.h` (arena+meta) + `slot_store.c` rewrite +
   `arena_live`/`arena_repack`. Rewrite `test_slot_store.c` (`seed_slot` +
   substitutions) red→green.
4. **New unit tests** (§8.1) red→green.
5. **`native_sim` verify** + add `pool_full` test (§8.3).
6. **Docs**: update `docs/architecture-redesign.md` §2.1 (describe arena, meta, the
   two knobs, the `DM_REJECTED_FULL` outcome, the compaction-at-safe-points
   invariant) and promote this plan's decision to `docs/adr/0003-arena-slot-storage.md`.
7. *(Optional, separate)* graceful `DM_FB_POOL_FULL` message (§5.10).

---

## 10. Open decision

**Default value of `AVG_EVENTS_PER_SLOT`.** This plan ships **32** (real
out-of-box savings; any single macro can still reach `MAX_EVENTS=64`; snapshots
expected unaffected). The conservative alternative is to default it to
`MAX_EVENTS` (64) — exact behavioral + RAM parity with today, savings strictly
opt-in. Choose before step 1; everything else in the plan is independent of the
value.

---

## 11. Files touched (summary)

**Modified:** `include/zmk-behavior-dynamic-macros/dm_config.h`,
`…/dm_event.h` (hoist `struct dm_slot_view`), `…/dm_render.h` (alias
`dm_render_slot_view`), `…/slot_store.h`, `…/slot_store_priv.h`,
`src/slot_store.c`, `src/dm_nvs.c`,
`src/behaviors/behavior_dynamic_macro_v2.c`, `src/dm_feedback_pump.c`,
`src/dm_events.c`, `Kconfig`, `tests/unit/test_slot_store.c`,
`docs/architecture-redesign.md`.
**Added:** `docs/adr/0003-arena-slot-storage.md`, `tests/core/pool_full/*`.
**Optional:** `include/…/dm_result.h`, `src/dm_machine.c`, `src/dm_feedback_build.c`,
`include/…/dm_spec.h` (graceful-full message).
**Explicitly untouched (legacy stack):** `src/behaviors/behavior_dynamic_macro.c`,
`src/dm_feedback.c`, `src/dm_storage.c`, `include/…/dm_internal.h`.
