/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_feedback_build — pure message-builder core (see dm_feedback_build.h).
 *
 * PURE: no Zephyr, no I/O. Emits HID keystrokes (fb_event) to an abstract sink.
 * The preview portion is rendered by dm_render (chars), each char mapped back to
 * its keystroke through ascii_to_hid — the exact inverse of the renderer's
 * printable_char_for_keycode, so a replayed 'a' types the same keycode the live
 * pump would.
 */

#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_feedback_build.h>

/* LSHIFT modifier bit (HID). The builder only ever needs shift for ASCII. */
#define FB_MOD_SHIFT 0x02

/* ---- ASCII -> HID, per locale --------------------------------------------- */

struct hid_keycode {
    uint8_t keycode;
    bool    shift;
};

static bool locale_is_plain(dm_locale locale) {
    return locale != DM_LOCALE_US && locale != DM_LOCALE_UK;
}

static struct hid_keycode letter_to_hid(dm_locale locale, char c, bool upper) {
    uint8_t keycode = 0x04 + (uint8_t)(c - 'a');

    if (locale == DM_LOCALE_DE) {
        if (c == 'y') {
            keycode = 0x1D; /* Z position */
        } else if (c == 'z') {
            keycode = 0x1C; /* Y position */
        }
    } else if (locale == DM_LOCALE_FR) {
        if (c == 'a') {
            keycode = 0x14;
        } else if (c == 'q') {
            keycode = 0x04;
        } else if (c == 'w') {
            keycode = 0x1D;
        } else if (c == 'z') {
            keycode = 0x1A;
        } else if (c == 'm') {
            keycode = 0x33;
        }
    }
    return (struct hid_keycode){.keycode = keycode, .shift = upper};
}

static struct hid_keycode digit_to_hid(dm_locale locale, char c) {
    uint8_t keycode = (c == '0') ? 0x27 : (uint8_t)(0x1E + (c - '1'));
    if (locale == DM_LOCALE_FR) {
        return (struct hid_keycode){.keycode = keycode, .shift = true};
    }
    return (struct hid_keycode){.keycode = keycode, .shift = false};
}

static struct hid_keycode ascii_to_hid(dm_locale locale, char c) {
    if (c >= 'a' && c <= 'z') {
        return letter_to_hid(locale, c, false);
    }
    if (c >= 'A' && c <= 'Z') {
        return letter_to_hid(locale, c - 'A' + 'a', true);
    }
    if (c >= '0' && c <= '9') {
        return digit_to_hid(locale, c);
    }

    switch (c) {
    case ' ':  return (struct hid_keycode){0x2C, false};
    case '\n': return (struct hid_keycode){0x28, false};
    default:   break;
    }

    if (locale_is_plain(locale)) {
        return (struct hid_keycode){0x2C, false}; /* space for unknown on plain */
    }

    bool uk = (locale == DM_LOCALE_UK);
    switch (c) {
    case '[':  return (struct hid_keycode){0x2F, false};
    case ']':  return (struct hid_keycode){0x30, false};
    case '\'': return (struct hid_keycode){0x34, false};
    case '"':  return uk ? (struct hid_keycode){0x1F, true} : (struct hid_keycode){0x34, true};
    case ':':  return (struct hid_keycode){0x33, true};
    case ';':  return (struct hid_keycode){0x33, false};
    case '+':  return (struct hid_keycode){0x2E, true};
    case '=':  return (struct hid_keycode){0x2E, false};
    case '-':  return (struct hid_keycode){0x2D, false};
    case '_':  return (struct hid_keycode){0x2D, true};
    case '.':  return (struct hid_keycode){0x37, false};
    case ',':  return (struct hid_keycode){0x36, false};
    case '<':  return (struct hid_keycode){0x36, true};
    case '>':  return (struct hid_keycode){0x37, true};
    case '/':  return (struct hid_keycode){0x38, false};
    case '?':  return (struct hid_keycode){0x38, true};
    case '!':  return (struct hid_keycode){0x1E, true};
    case '@':  return uk ? (struct hid_keycode){0x34, true} : (struct hid_keycode){0x1F, true};
    case '#':  return uk ? (struct hid_keycode){0x32, false} : (struct hid_keycode){0x20, true};
    case '$':  return (struct hid_keycode){0x21, true};
    case '%':  return (struct hid_keycode){0x22, true};
    case '^':  return (struct hid_keycode){0x23, true};
    case '&':  return (struct hid_keycode){0x24, true};
    case '*':  return (struct hid_keycode){0x25, true};
    case '(':  return (struct hid_keycode){0x26, true};
    case ')':  return (struct hid_keycode){0x27, true};
    case '{':  return (struct hid_keycode){0x2F, true};
    case '}':  return (struct hid_keycode){0x30, true};
    case '\\': return uk ? (struct hid_keycode){0x64, false} : (struct hid_keycode){0x31, false};
    case '|':  return uk ? (struct hid_keycode){0x64, true} : (struct hid_keycode){0x31, true};
    case '`':  return (struct hid_keycode){0x35, false};
    case '~':  return uk ? (struct hid_keycode){0x32, true} : (struct hid_keycode){0x35, true};
    default:   return (struct hid_keycode){0x2C, false};
    }
}

/* ---- low-level sink emits ------------------------------------------------- */

static void emit_char(dm_locale locale, dm_fb_sink *sink, char c) {
    if (!sink->space_for(sink->ctx, 1)) {
        return;
    }
    struct hid_keycode hk = ascii_to_hid(locale, c);
    sink->emit(sink->ctx, hk.keycode, hk.shift ? FB_MOD_SHIFT : 0);
}

static void emit_str(dm_locale locale, dm_fb_sink *sink, const char *s) {
    for (const char *p = s; *p; p++) {
        emit_char(locale, sink, *p);
    }
}

void dm_feedback_emit_ascii(dm_locale locale, dm_fb_sink *sink, const char *s) {
    emit_str(locale, sink, s);
}

static void emit_number(dm_locale locale, dm_fb_sink *sink, int n) {
    char buf[8];
    int len = 0;
    if (n == 0) {
        emit_char(locale, sink, '0');
        return;
    }
    while (n > 0 && len < (int)sizeof(buf)) {
        buf[len++] = (char)('0' + (n % 10));
        n /= 10;
    }
    for (int i = len - 1; i >= 0; i--) {
        emit_char(locale, sink, buf[i]);
    }
}

/* ---- message tables (FULL/ARROW, plain variant of FULL) ------------------- */

struct dm_msg_table {
    const char *rec;
    const char *stop;
    const char *no_rec;
    const char *saved;
    const char *slot;
    const char *del;
    const char *del_fail;
    const char *save_fail;
    const char *save_full;
    const char *del_full;
    const char *empty;
    const char *mov;
    const char *mov_src;
    const char *mov_dest;
    const char *mov_sep;
    const char *mov_cancel;
    const char *chain;
    const char *preview_start;
    const char *preview_end;
    const char *fb_prefix;
    const char *close;
};

static const struct dm_msg_table msg_arrow = {
    .rec = ">*", .stop = ">.", .no_rec = "?*", .saved = ">", .slot = ">",
    .del = "-", .del_fail = "!-",
    .save_fail = "!>", .save_full = "!>%", .del_full = "!-%",
    .empty = "?",
    .mov = "<>", .mov_src = "<>", .mov_dest = ">",
    .mov_sep = ">>", .mov_cancel = "<>x",
    .chain = ">>",
    .preview_start = ":'", .preview_end = "'",
    .fb_prefix = ">FB:", .close = "",
};

static const struct dm_msg_table msg_full_punct = {
    .rec = "[DM REC]", .stop = "[DM STOP]", .no_rec = "[DM NO REC]", .saved = "[DM SAVED ",
    .slot = "[DM SLOT ", .del = "[DM DEL ", .del_fail = " FAILED]",
    .save_fail = "[DM SAVE FAILED ", .save_full = "[DM SAVE QUEUE FULL ",
    .del_full = "[DM DEL QUEUE FULL ",
    .empty = ": -]",
    .mov = "[DM MOV]", .mov_src = "[DM MOV SRC ", .mov_dest = "[DM MOV ",
    .mov_sep = "->", .mov_cancel = "[DM MOV CANCEL]",
    .chain = "[DM +",
    .preview_start = ": '", .preview_end = "'",
    .fb_prefix = "[DM FB:", .close = "]",
};

static const struct dm_msg_table msg_full_plain = {
    .rec = "DM REC", .stop = "DM STOP", .no_rec = "DM NO REC", .saved = "DM SAVED ",
    .slot = "DM SLOT ", .del = "DM DEL ", .del_fail = "DM DEL FAILED",
    .save_fail = "DM SAVE FAILED ", .save_full = "DM SAVE QUEUE FULL ",
    .del_full = "DM DEL QUEUE FULL ",
    .empty = " EMPTY",
    .mov = "DM MOV", .mov_src = "DM MOV SRC ", .mov_dest = "DM MOV ",
    .mov_sep = " TO ", .mov_cancel = "DM MOV CANCEL",
    .chain = "DM PLUS",
    .preview_start = "", .preview_end = "",
    .fb_prefix = "DM FB ", .close = "",
};

static const struct dm_msg_table *msg(dm_fb_style style, dm_locale locale) {
    if (style == DM_FB_STYLE_ARROW) {
        return &msg_arrow;
    }
    return locale_is_plain(locale) ? &msg_full_plain : &msg_full_punct;
}

static char slot_prefix(const dm_fb_facts *facts, int slot) {
    return (slot < facts->nvs_slots) ? 'N' : 'R';
}

/* ---- scaffolding sub-builders (style/locale-branching in ONE place) ------- */

static void build_slot_ref(const struct dm_msg_table *m, dm_fb_style style, dm_locale locale,
                           const dm_fb_facts *facts, dm_fb_sink *sink, int slot) {
    (void)m; (void)style;
    emit_char(locale, sink, slot_prefix(facts, slot));
    emit_number(locale, sink, slot);
}

static void build_slot_empty(const struct dm_msg_table *m, dm_fb_style style, dm_locale locale,
                             const dm_fb_facts *facts, dm_fb_sink *sink, int slot) {
    if (style == DM_FB_STYLE_ARROW) {
        emit_str(locale, sink, m->empty);
        build_slot_ref(m, style, locale, facts, sink, slot);
    } else {
        emit_str(locale, sink, m->slot);
        build_slot_ref(m, style, locale, facts, sink, slot);
        emit_str(locale, sink, m->empty);
    }
}

static void build_slot_full_suffix(dm_fb_style style, dm_locale locale, dm_fb_sink *sink) {
    if (style == DM_FB_STYLE_ARROW) {
        emit_char(locale, sink, '%');
    } else if (locale_is_plain(locale)) {
        emit_str(locale, sink, " FULL");
    } else {
        emit_str(locale, sink, " FULL]");
    }
}

static void build_delete_failed(const struct dm_msg_table *m, dm_fb_style style, dm_locale locale,
                                const dm_fb_facts *facts, dm_fb_sink *sink, int slot) {
    if (style == DM_FB_STYLE_ARROW) {
        emit_str(locale, sink, m->del_fail);
        build_slot_ref(m, style, locale, facts, sink, slot);
    } else {
        emit_str(locale, sink, m->del);
        build_slot_ref(m, style, locale, facts, sink, slot);
        emit_str(locale, sink, m->del_fail);
    }
}

static void build_status_slot_label(dm_fb_style style, dm_locale locale, const dm_fb_facts *facts,
                                    dm_fb_sink *sink, int slot) {
    if (style == DM_FB_STYLE_ARROW) {
        emit_char(locale, sink, '>');
        emit_char(locale, sink, slot_prefix(facts, slot));
        emit_number(locale, sink, slot);
        emit_char(locale, sink, ':');
    } else {
        emit_char(locale, sink, slot_prefix(facts, slot));
        emit_number(locale, sink, slot);
        if (locale_is_plain(locale)) {
            emit_char(locale, sink, ' ');
        } else {
            emit_str(locale, sink, ": ");
        }
    }
}

static void build_slot_range(dm_fb_style style, dm_locale locale, const dm_fb_facts *facts,
                             dm_fb_sink *sink) {
    int nvs = facts->nvs_slots, max = facts->max_slots;
    if (nvs > 0 && nvs < max) {
        if (style == DM_FB_STYLE_ARROW) {
            emit_str(locale, sink, " N0-");
            emit_number(locale, sink, nvs - 1);
            emit_str(locale, sink, " R");
            emit_number(locale, sink, nvs);
            emit_char(locale, sink, '-');
        } else if (locale_is_plain(locale)) {
            emit_str(locale, sink, " NVS 0 TO ");
            emit_number(locale, sink, nvs - 1);
            emit_str(locale, sink, " RAM ");
            emit_number(locale, sink, nvs);
            emit_str(locale, sink, " TO ");
        } else {
            emit_str(locale, sink, " NVS:0-");
            emit_number(locale, sink, nvs - 1);
            emit_str(locale, sink, " RAM:");
            emit_number(locale, sink, nvs);
            emit_char(locale, sink, '-');
        }
        emit_number(locale, sink, max - 1);
    } else if (nvs == 0) {
        emit_str(locale, sink, " RAM");
    } else {
        emit_str(locale, sink, " NVS");
    }
}

static void build_status_header(dm_fb_style style, dm_locale locale, const dm_fb_facts *facts,
                               dm_fb_sink *sink) {
    if (style == DM_FB_STYLE_ARROW) {
        emit_str(locale, sink, "==");
    } else if (locale_is_plain(locale)) {
        emit_str(locale, sink, "DM ");
    } else {
        emit_str(locale, sink, "[DM ");
    }
    emit_number(locale, sink, facts->filled_count);
    if (style != DM_FB_STYLE_ARROW && locale_is_plain(locale)) {
        emit_str(locale, sink, " OF ");
    } else {
        emit_char(locale, sink, '/');
    }
    emit_number(locale, sink, facts->max_slots);
    if (facts->max_slots == 0) {
        emit_str(locale, sink, " NO SLOTS");
    } else {
        build_slot_range(style, locale, facts, sink);
    }
    if (style != DM_FB_STYLE_ARROW && !locale_is_plain(locale)) {
        emit_char(locale, sink, ']');
    }
    emit_char(locale, sink, '\n');
}

/* ---- the build entry points ----------------------------------------------- */

bool dm_feedback_build(const dm_feedback_spec *spec, dm_fb_style style, dm_locale locale,
                       const dm_fb_facts *facts, dm_fb_sink *sink) {
    const struct dm_msg_table *m = msg(style, locale);
    int s = spec->slot;

    switch (spec->kind) {
    case DM_FB_REC:    emit_str(locale, sink, m->rec);    return false;
    case DM_FB_STOP:   emit_str(locale, sink, m->stop);   return false;
    case DM_FB_NO_REC: emit_str(locale, sink, m->no_rec); return false;

    case DM_FB_SAVED:
        emit_str(locale, sink, m->saved);
        build_slot_ref(m, style, locale, facts, sink, s);
        if (spec->show_preview) {
            emit_str(locale, sink, m->preview_start);
            return true; /* preview follows */
        }
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_SLOT_FULL:
        emit_str(locale, sink, m->slot);
        build_slot_ref(m, style, locale, facts, sink, s);
        build_slot_full_suffix(style, locale, sink);
        return false;

    case DM_FB_SLOT_EMPTY:
        build_slot_empty(m, style, locale, facts, sink, s);
        return false;

    case DM_FB_OVERFLOW:
        /* full message: arrow "!%", full "[DM FULL]"/"DM FULL" */
        emit_str(locale, sink,
                 style == DM_FB_STYLE_ARROW ? "!%" : (locale_is_plain(locale) ? "DM FULL" : "[DM FULL]"));
        return false;

    case DM_FB_MOVE_PROMPT: emit_str(locale, sink, m->mov); return false;

    case DM_FB_MOVE_SRC:
        emit_str(locale, sink, m->mov_src);
        build_slot_ref(m, style, locale, facts, sink, s);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_MOVED:
        emit_str(locale, sink, m->mov_dest);
        build_slot_ref(m, style, locale, facts, sink, s);
        emit_str(locale, sink, m->mov_sep);
        build_slot_ref(m, style, locale, facts, sink, spec->slot2);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_MOVE_CANCEL: emit_str(locale, sink, m->mov_cancel); return false;

    case DM_FB_CHAIN_INSERT:
        return true; /* preview only, no scaffolding */

    case DM_FB_CHAIN_EMPTY:
        build_slot_empty(m, style, locale, facts, sink, s);
        return false;

    case DM_FB_CHAIN_NO_ROOM:
        emit_str(locale, sink, m->chain);
        build_slot_ref(m, style, locale, facts, sink, s);
        build_slot_full_suffix(style, locale, sink);
        return false;

    case DM_FB_DELETED:
        emit_str(locale, sink, m->del);
        build_slot_ref(m, style, locale, facts, sink, s);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_DELETE_FAILED:
        build_delete_failed(m, style, locale, facts, sink, s);
        return false;

    case DM_FB_SAVE_FAILED:
        emit_str(locale, sink, m->save_fail);
        build_slot_ref(m, style, locale, facts, sink, s);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_SAVE_QFULL:
        emit_str(locale, sink, m->save_full);
        build_slot_ref(m, style, locale, facts, sink, s);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_DELETE_QFULL:
        emit_str(locale, sink, m->del_full);
        build_slot_ref(m, style, locale, facts, sink, s);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_KNOB:
        emit_str(locale, sink, m->fb_prefix);
        emit_str(locale, sink, spec->knob_text);
        emit_str(locale, sink, m->close);
        return false;

    case DM_FB_STATUS_HEADER:
        build_status_header(style, locale, facts, sink);
        return false;

    case DM_FB_STATUS_SLOT:
        build_status_slot_label(style, locale, facts, sink, s);
        if (facts->slot_is_empty) {
            emit_char(locale, sink, '-');
            emit_char(locale, sink, '\n');
            return false;
        }
        if (spec->show_preview) {
            /* open quote: skipped for plain locale non-arrow */
            if (style == DM_FB_STYLE_ARROW || !locale_is_plain(locale)) {
                emit_char(locale, sink, '\'');
            }
            return true; /* preview follows, then the count suffix */
        }
        emit_number(locale, sink, facts->preview_event_count);
        emit_char(locale, sink, '\n');
        return false;
    }
    return false;
}

/* ---- preview streaming via dm_render -------------------------------------- */

/* dm_render emits chars; this adapter maps each back to a keystroke. The sink
 * passed to dm_render carries our fb_sink + locale so emit_char can re-encode. */
struct preview_adapter {
    dm_fb_sink *fb;
    dm_locale   locale;
};

static void preview_emit_char(void *ctx, char c) {
    struct preview_adapter *pa = ctx;
    struct hid_keycode hk = ascii_to_hid(pa->locale, c);
    pa->fb->emit(pa->fb->ctx, hk.keycode, hk.shift ? FB_MOD_SHIFT : 0);
}

static bool preview_space_for(void *ctx, uint8_t n) {
    struct preview_adapter *pa = ctx;
    return pa->fb->space_for(pa->fb->ctx, n);
}

bool dm_feedback_build_preview(const dm_render_slot_view *view, dm_locale locale, dm_fb_sink *sink,
                               dm_render_cursor *cursor) {
    struct preview_adapter pa = {.fb = sink, .locale = locale};
    dm_sink rsink = {.emit_char = preview_emit_char, .space_for = preview_space_for, .ctx = &pa};
    return dm_render_slot(view, locale, &rsink, cursor);
}

void dm_feedback_build_preview_suffix(const dm_feedback_spec *spec, dm_fb_style style,
                                      dm_locale locale, const dm_fb_facts *facts, dm_fb_sink *sink) {
    const struct dm_msg_table *m = msg(style, locale);

    if (spec->kind == DM_FB_STATUS_SLOT) {
        /* "' (N)\n" (arrow/punct) or " N EVENTS\n" (plain non-arrow) */
        if (style != DM_FB_STYLE_ARROW && locale_is_plain(locale)) {
            emit_char(locale, sink, ' ');
            emit_number(locale, sink, facts->preview_event_count);
            emit_str(locale, sink, " EVENTS");
        } else {
            emit_str(locale, sink, "' (");
            emit_number(locale, sink, facts->preview_event_count);
            emit_char(locale, sink, ')');
        }
        emit_char(locale, sink, '\n');
        return;
    }

    /* SAVED preview suffix: preview_end + close */
    emit_str(locale, sink, m->preview_end);
    emit_str(locale, sink, m->close);
}
