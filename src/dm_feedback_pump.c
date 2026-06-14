/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_feedback_pump — the Zephyr feedback emitter (see dm_feedback_pump.h).
 *
 * The message bytes come from the pure dm_feedback_build; this module turns them
 * into TAP_DELAY-spaced keystrokes through a press/release ring, streams the
 * resumable preview portion under ring backpressure, and reports completion up to
 * dm_machine. It writes no dm_state — typing_finished / erase_due / erase_cancel
 * are the only contact with the machine.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk-behavior-dynamic-macros/dm_feedback_pump.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TAP_DELAY CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_TAP_DELAY

/* The erase-delay Kconfig only exists when auto-erase is enabled. The erase
 * scheduler is still compiled (erase_enabled gates it at runtime, defaulting off),
 * so give the unused constant a harmless value when the Kconfig is absent. */
#ifndef CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_ERASE_DELAY
#define CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_ERASE_DELAY 1500
#endif

/* HID backspace, and the modifier bit the build core emits for shifted ASCII. */
#define HID_BACKSPACE 0x2A
#define HID_RETURN    0x28

/* ---- ring -------------------------------------------------------------------
 * Power-of-two ring with masked head/tail. One usable slot is reserved so
 * head==tail means empty unambiguously.
 */

static inline uint8_t ring_count(const dm_feedback *f) {
    return (uint8_t)((f->ring_head - f->ring_tail) & (DM_FB_RING_SIZE - 1));
}

static inline uint8_t ring_space(const dm_feedback *f) {
    return (uint8_t)(DM_FB_RING_SIZE - 1 - ring_count(f));
}

static inline bool ring_empty(const dm_feedback *f) {
    return f->ring_head == f->ring_tail;
}

static void ring_push(dm_feedback *f, uint16_t keycode, uint8_t mods) {
    f->ring[f->ring_head] = (struct dm_fb_event){.keycode = keycode, .mods = mods};
    f->ring_head = (uint8_t)((f->ring_head + 1) & (DM_FB_RING_SIZE - 1));
}

/* ---- the dm_fb_sink the build core emits into --------------------------------
 * emit appends one keystroke; space_for is the backpressure point — the ring
 * sink's space_for returning false is exactly what pauses a preview mid-walk.
 */

static void sink_emit(void *ctx, uint16_t keycode, uint8_t mods) {
    dm_feedback *f = ctx;
    if (ring_space(f) < 1) {
        LOG_WRN("Dynamic macro feedback ring full");
        return;
    }
    ring_push(f, keycode, mods);
}

static bool sink_space_for(void *ctx, uint8_t n) {
    dm_feedback *f = ctx;
    return ring_space(f) >= n;
}

static dm_fb_sink pump_sink(dm_feedback *f) {
    return (dm_fb_sink){.emit = sink_emit, .space_for = sink_space_for, .ctx = f};
}

/* ---- per-spec presentation facts -------------------------------------------- */

/* The runtime level a spec must reach to type anything. Status uses the separate
 * compile-time status_detail gate, handled in speak(). */
static int spec_min_level(dm_fb_kind kind) {
    switch (kind) {
    case DM_FB_SLOT_EMPTY:
    case DM_FB_CHAIN_EMPTY:
        return DM_FB_LEVEL_BASIC;
    case DM_FB_OVERFLOW:
    case DM_FB_SAVE_FAILED:
    case DM_FB_DELETE_FAILED:
    case DM_FB_SAVE_QFULL:
    case DM_FB_DELETE_QFULL:
        return DM_FB_LEVEL_ERROR;
    case DM_FB_CHAIN_INSERT:
        return DM_FB_LEVEL_VERBOSE; /* a chain insert is preview-only */
    default:
        return DM_FB_LEVEL_COMMAND;
    }
}

/* The SAVED message shows a preview only at VERBOSE; all other previewing specs
 * carry show_preview from the machine/shell already. Resolve it live here so the
 * runtime level — not the spec — owns the VERBOSE decision for SAVED. */
static bool spec_wants_preview(const dm_feedback *f, const dm_feedback_spec *spec) {
    if (spec->kind == DM_FB_SAVED) {
        return f->level >= DM_FB_LEVEL_VERBOSE;
    }
    return spec->show_preview;
}

static dm_fb_facts gather_facts(const dm_feedback *f, const dm_feedback_spec *spec) {
    dm_fb_facts facts = {
        .filled_count = slot_store_count(f->store, DM_SLOT_CLASS_ALL),
        .nvs_slots = f->nvs_slots,
        .max_slots = f->max_slots,
        .preview_event_count = 0,
        .slot_is_empty = false,
    };
    if (spec->slot >= 0) {
        struct dm_slot_view v = slot_store_get(f->store, spec->slot);
        facts.slot_is_empty = (v.events == NULL);
        facts.preview_event_count = (int)v.event_count;
    }
    return facts;
}

static dm_render_slot_view slot_view(const dm_feedback *f, int slot) {
    /* slot_store_get already returns the render view; just guard the negative slot. */
    return (slot >= 0) ? slot_store_get(f->store, slot)
                       : (dm_render_slot_view){.event_count = 0, .events = NULL};
}

/* ---- finishing & continuation ----------------------------------------------- */

static void start_timer(dm_feedback *f) {
    k_timer_start(&f->emit_timer, K_NO_WAIT, K_NO_WAIT);
}

/* Begin streaming the next status slot's line, or finish the status sequence.
 * Returns true if a status line was started (typing continues), false if the
 * sequence is done and the caller should report typing_finished. */
static bool status_advance(dm_feedback *f) {
    bool show_preview = f->status_detail >= DM_FB_STATUS_USED_PREVIEW;
    bool show_all = f->status_detail >= DM_FB_STATUS_FULL;

    while (f->status_next_slot < f->max_slots) {
        bool empty = slot_store_is_empty(f->store, f->status_next_slot);
        if (show_all || !empty) {
            break;
        }
        f->status_next_slot++;
    }
    if (f->status_next_slot >= f->max_slots) {
        return false;
    }

    int slot = f->status_next_slot++;

    /* reset ring for this line; the spec drives label + (optional) preview */
    f->ring_head = f->ring_tail = 0;
    f->press_phase = true;
    f->cursor = (dm_render_cursor){0};
    f->preview_pending = false;
    f->suffix_pending = false;

    f->spec = (dm_feedback_spec){
        .kind = DM_FB_STATUS_SLOT,
        .slot = slot,
        .slot2 = -1,
        .show_preview = show_preview,
    };
    dm_fb_facts facts = gather_facts(f, &f->spec);
    dm_fb_sink sink = pump_sink(f);
    bool preview_follows = dm_feedback_build(&f->spec, f->style, f->locale, &facts, &sink);
    if (preview_follows) {
        f->preview_pending = true;
        f->suffix_pending = true;
    }
    start_timer(f);
    return true;
}

/* The ring drained and no more preview/suffix is owed for the current line. */
static void on_typing_drained(dm_feedback *f) {
    /* An erase batch finishing is its own completion path: it must restore the
     * state parked at erase_due (NOT the last speak's return-state) and clear the
     * erase-active flags so a later keycode does not fire a phantom cancel. A
     * remaining batch (count exceeded one ring) is rescheduled here, staying in
     * the erase sequence; only the final batch finishes it. */
    if (f->erase_in_progress) {
        if (f->erase_pending && f->erase_char_count > 0) {
            k_work_reschedule(&f->erase_work, K_NO_WAIT);
            return;
        }
        f->erase_in_progress = false;
        f->emit_active = false;
        f->set_suppress(f->ctx, false);
        dm_machine_erase_finished(f->machine);
        return;
    }

    if (f->status_mode && status_advance(f)) {
        return; /* another status line started: still actively emitting */
    }

    bool was_status = f->status_mode;
    f->status_mode = false;
    f->have_spec = false;
    f->emit_active = false;

    f->set_suppress(f->ctx, false);

    /* auto-erase: only after real feedback typing (not status output), only if a
     * non-RET char was actually typed. Counting happens on the press phase. */
    if (f->erase_enabled && f->erase_char_count > 0 && !was_status) {
        f->erase_pending = true;
        k_work_reschedule(&f->erase_work,
                          K_MSEC(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_ERASE_DELAY));
    }

    dm_machine_typing_finished(f->machine);
}

/* ---- the emit loop ---------------------------------------------------------- */

/*
 * One work iteration: refill from the streaming preview if the ring drained
 * mid-walk, then emit one press OR release of the tail keystroke. Press and
 * release are separate iterations (TAP_DELAY apart) so the host does not coalesce
 * a tight burst; the tail only advances after the release.
 */
static void emit_iteration(dm_feedback *f) {
    /* A stale timer fire after a cancel: the iteration is no longer owed. Bail
     * before on_typing_drained so it cannot report a phantom typing_finished. */
    if (!f->emit_active) {
        return;
    }

    /* refill the ring from the preview walk when it empties */
    if (ring_empty(f) && f->preview_pending) {
        dm_render_slot_view view = slot_view(f, f->spec.slot);
        dm_fb_sink sink = pump_sink(f);
        bool done = dm_feedback_build_preview(&view, f->locale, &sink, &f->cursor);
        if (done) {
            f->preview_pending = false; /* the suffix waits for a fresh, empty ring below */
        }
    }

    /* The suffix ("' (N)\n", or SAVED's preview_end + close) is short (<= ~10
     * chars) but the preview walk above can leave the ring nearly full, where an
     * inline suffix emit would be silently dropped by the sink. Emit it once the
     * preview has fully drained AND the ring is empty, so it always fits. */
    if (ring_empty(f) && f->suffix_pending && !f->preview_pending) {
        dm_fb_facts facts = gather_facts(f, &f->spec);
        dm_fb_sink sink = pump_sink(f);
        dm_feedback_build_preview_suffix(&f->spec, f->style, f->locale, &facts, &sink);
        f->suffix_pending = false;
    }

    if (ring_empty(f)) {
        on_typing_drained(f);
        return;
    }

    const struct dm_fb_event *ev = &f->ring[f->ring_tail];
    f->raise_keycode(f->ctx, ev->keycode, ev->mods, f->press_phase);

    if (f->press_phase) {
        f->press_phase = false;
        /* count erasable chars on the press half: feedback typing only (not
         * erase backspaces), and RET is excluded (it is not a visible char). */
        if (!f->erase_in_progress && ev->keycode != HID_RETURN) {
            f->erase_char_count++;
        }
    } else {
        f->press_phase = true;
        f->ring_tail = (uint8_t)((f->ring_tail + 1) & (DM_FB_RING_SIZE - 1));
    }

    k_timer_start(&f->emit_timer, K_MSEC(TAP_DELAY), K_NO_WAIT);
}

static void emit_work_handler(struct k_work *work) {
    dm_feedback *f = CONTAINER_OF(work, dm_feedback, emit_work);
    emit_iteration(f);
}

static void emit_timer_handler(struct k_timer *timer) {
    dm_feedback *f = CONTAINER_OF(timer, dm_feedback, emit_timer);
    k_work_submit(&f->emit_work);
}

/* ---- speak ------------------------------------------------------------------ */

void dm_feedback_speak(dm_feedback *f, const dm_feedback_spec *spec) {
    /* STATUS gates on the compile-time status_detail knob, not the runtime level.
     * A KNOB confirmation always types its cue, regardless of the resulting level.
     * Everything else gates on the runtime level. */
    bool is_status = (spec->kind == DM_FB_STATUS_HEADER);
    if (is_status) {
        if (f->status_detail < DM_FB_STATUS_COUNT) {
            dm_machine_typing_finished(f->machine);
            return;
        }
    } else if (spec->kind != DM_FB_KNOB && f->level < spec_min_level(spec->kind)) {
        /* OFF-path rule: below level types nothing, but typing_finished still runs
         * synchronously so the return-state / persist / timeout work happens. */
        dm_machine_typing_finished(f->machine);
        return;
    }

    /* fresh ring + streaming state for this message */
    f->ring_head = f->ring_tail = 0;
    f->press_phase = true;
    f->cursor = (dm_render_cursor){0};
    f->preview_pending = false;
    f->suffix_pending = false;
    f->erase_char_count = 0;
    f->erase_in_progress = false;

    f->spec = *spec;
    f->spec.show_preview = spec_wants_preview(f, spec);
    f->have_spec = true;

    f->status_mode = is_status && f->status_detail >= DM_FB_STATUS_USED && f->max_slots > 0;
    f->status_next_slot = 0;

    f->set_suppress(f->ctx, true);

    dm_fb_facts facts = gather_facts(f, &f->spec);
    dm_fb_sink sink = pump_sink(f);
    bool preview_follows = dm_feedback_build(&f->spec, f->style, f->locale, &facts, &sink);
    if (preview_follows) {
        f->preview_pending = true;
        f->suffix_pending = true;
    }

    /* an empty message (e.g. status header that fit nothing meaningful, or a
     * preview-only spec with an empty slot) still has to drain through the loop
     * so status continuation / typing_finished run consistently. */
    f->emit_active = true;
    start_timer(f);
}

/* ---- knob commands ---------------------------------------------------------- */

static const char *level_name(uint8_t level) {
    switch (level) {
    case DM_FB_LEVEL_ERROR:   return "ERROR";
    case DM_FB_LEVEL_COMMAND: return "COMMAND";
    case DM_FB_LEVEL_BASIC:   return "BASIC";
    case DM_FB_LEVEL_VERBOSE: return "VERBOSE";
    default:                  return "?";
    }
}

static void speak_knob(dm_feedback *f, const char *text) {
    dm_feedback_spec spec = {.kind = DM_FB_KNOB, .slot = -1, .slot2 = -1, .knob_text = text};
    dm_feedback_speak(f, &spec);
}

void dm_feedback_knob_level(dm_feedback *f, int direction) {
    int new_level = (int)f->level + direction;
    if (new_level < DM_FB_LEVEL_ERROR) {
        new_level = DM_FB_LEVEL_ERROR;
    }
    if (new_level > DM_FB_LEVEL_VERBOSE) {
        new_level = DM_FB_LEVEL_VERBOSE;
    }
    f->level = (uint8_t)new_level;
    if (f->save_knobs) {
        f->save_knobs(f->ctx, f->level, f->style, f->erase_enabled);
    }
    speak_knob(f, level_name(f->level));
}

static bool locale_is_plain(dm_locale locale) {
    return locale != DM_LOCALE_US && locale != DM_LOCALE_UK;
}

void dm_feedback_knob_style_toggle(dm_feedback *f) {
    uint8_t new_style = (f->style == DM_FB_STYLE_ARROW) ? DM_FB_STYLE_FULL : DM_FB_STYLE_ARROW;
    /* ARROW requires a full-punctuation locale; the toggle is a no-op otherwise. */
    if (new_style == DM_FB_STYLE_ARROW && locale_is_plain(f->locale)) {
        dm_machine_typing_finished(f->machine);
        return;
    }
    f->style = new_style;
    if (f->save_knobs) {
        f->save_knobs(f->ctx, f->level, f->style, f->erase_enabled);
    }
    speak_knob(f, f->style == DM_FB_STYLE_ARROW ? "ARROW" : "FULL");
}

void dm_feedback_knob_erase_toggle(dm_feedback *f) {
    f->erase_enabled = !f->erase_enabled;
    if (f->save_knobs) {
        f->save_knobs(f->ctx, f->level, f->style, f->erase_enabled);
    }
    speak_knob(f, f->erase_enabled ? "ERASE ON" : "ERASE OFF");
}

/* ---- boot restore ----------------------------------------------------------- */

void dm_feedback_restore_level(dm_feedback *f, uint8_t level) {
    if (level >= DM_FB_LEVEL_ERROR && level <= DM_FB_LEVEL_VERBOSE) {
        f->level = level;
    }
}

void dm_feedback_restore_style(dm_feedback *f, uint8_t style) {
    /* The ARROW-on-plain-locale rule lives here, once: a persisted ARROW on a
     * plain locale is silently kept as the default (FULL), by construction. */
    if (style == DM_FB_STYLE_FULL || (!locale_is_plain(f->locale) && style == DM_FB_STYLE_ARROW)) {
        f->style = style;
    }
}

void dm_feedback_restore_erase(dm_feedback *f, bool erase) {
    f->erase_enabled = erase;
}

uint8_t dm_feedback_level(const dm_feedback *f) { return f->level; }
uint8_t dm_feedback_style(const dm_feedback *f) { return f->style; }
bool    dm_feedback_erase(const dm_feedback *f) { return f->erase_enabled; }

/* ---- auto-erase scheduler --------------------------------------------------- */

static void erase_work_handler(struct k_work *work) {
    struct k_work_delayable *d = k_work_delayable_from_work(work);
    dm_feedback *f = CONTAINER_OF(d, dm_feedback, erase_work);

    if (!f->erase_pending || f->erase_char_count == 0) {
        f->erase_pending = false;
        return;
    }

    uint16_t count = f->erase_char_count;
    f->erase_pending = false;
    f->erase_char_count = 0;

    /* tell the machine erase is due (parks the return-state ONCE, writes
     * TYPING_ERASE) before we emit anything. */
    dm_machine_erase_due(f->machine);

    /* emit backspaces through the ring so each gets TAP_DELAY spacing; batch at
     * the ring capacity and re-arm for the remainder. */
    f->ring_head = f->ring_tail = 0;
    f->press_phase = true;
    f->preview_pending = false;
    f->suffix_pending = false;
    f->status_mode = false;
    f->erase_in_progress = true;

    uint16_t batch = MIN(count, (uint16_t)(DM_FB_RING_SIZE - 1));
    for (uint16_t i = 0; i < batch; i++) {
        ring_push(f, HID_BACKSPACE, 0);
    }
    if (count > batch) {
        f->erase_char_count = (uint16_t)(count - batch);
        f->erase_pending = true;
    }

    f->emit_active = true;
    f->set_suppress(f->ctx, true);
    start_timer(f);
}

/* The shell calls this from any DM binding press or any keycode through the
 * listener: cancel a scheduled erase and abort one already mid-emission. */
static void cancel_erase(dm_feedback *f) {
    if (f->erase_pending) {
        k_work_cancel_delayable(&f->erase_work);
        f->erase_pending = false;
    }
    if (f->erase_in_progress) {
        /* drain the ring to abort mid-sequence, drop suppression, and let the
         * machine restore the parked return-state. Clearing emit_active makes the
         * emit_timer fire already in flight inert (it cannot be un-queued once the
         * timer ISR has submitted emit_work), so it cannot report a phantom
         * typing_finished that would clobber the command pressed to cancel. */
        k_timer_stop(&f->emit_timer);
        f->ring_head = f->ring_tail;
        f->erase_in_progress = false;
        f->emit_active = false;
        f->set_suppress(f->ctx, false);
        dm_machine_erase_cancel(f->machine);
    }
}

/* ---- public erase entry (the shell calls this) ------------------------------ */

void dm_feedback_pump_cancel_erase(dm_feedback *f) {
    cancel_erase(f);
}

/* ---- lifecycle -------------------------------------------------------------- */

void dm_feedback_pump_init(dm_feedback *f, const dm_feedback_config *cfg) {
    memset(f, 0, sizeof(*f));
    f->machine = cfg->machine;
    f->store = cfg->store;
    f->locale = cfg->locale;
    f->status_detail = cfg->status_detail;
    f->nvs_slots = cfg->nvs_slots;
    f->max_slots = cfg->max_slots;
    f->raise_keycode = cfg->raise_keycode;
    f->save_knobs = cfg->save_knobs;
    f->set_suppress = cfg->set_suppress;
    f->ctx = cfg->ctx;

    f->level = cfg->default_level;
    f->style = cfg->default_style;
    f->erase_enabled = cfg->default_erase;
    f->press_phase = true;

    k_timer_init(&f->emit_timer, emit_timer_handler, NULL);
    k_work_init(&f->emit_work, emit_work_handler);
    k_work_init_delayable(&f->erase_work, erase_work_handler);
}
