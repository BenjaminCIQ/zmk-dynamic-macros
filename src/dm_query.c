/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * dm_query — pure projection behind the dm_get_* widget API (see dm_query.h).
 *
 * PURE: no Zephyr, no global state. The preview string is the dm_render walk
 * with a buffer sink, so the widget preview and the live-typed preview come
 * from the same renderer and cannot disagree.
 */

#include <stdio.h>
#include <string.h>

#include <zmk-behavior-dynamic-macros/dm_query.h>

/* Buffer sink: emit_char appends to buf[pos]; space_for guards the remaining
 * room (reserving one byte for the terminating NUL). */
struct buf_sink {
    dm_sink sink;
    char   *buf;
    size_t  pos;
    size_t  cap; /* total buffer length incl. the NUL slot */
};

static void buf_emit(void *ctx, char c) {
    struct buf_sink *b = ctx;
    if (b->pos < b->cap - 1) {
        b->buf[b->pos++] = c;
    }
}

static bool buf_space_for(void *ctx, uint8_t n) {
    struct buf_sink *b = ctx;
    return b->pos + n < b->cap; /* leave room for the NUL */
}

int dm_query_preview_string(const dm_render_slot_view *view, dm_locale locale, uint32_t event_count,
                            bool typing_enabled, char *buf, size_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    buf[0] = '\0';

    if (!typing_enabled) {
        int n = snprintf(buf, len, "(%u events)", (unsigned)event_count);
        return n < (int)len ? n : (int)len - 1;
    }

    if (!view || view->event_count == 0) {
        return 0;
    }

    struct buf_sink b = {
        .sink = {.emit_char = buf_emit, .space_for = buf_space_for, .ctx = &b},
        .buf = buf,
        .pos = 0,
        .cap = len,
    };
    dm_render_slot(view, locale, &b.sink, NULL);
    buf[b.pos] = '\0';
    return (int)b.pos;
}
