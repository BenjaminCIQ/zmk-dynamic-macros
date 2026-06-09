/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * Render parity test (host, fast loop) — redesign §5.2.
 *
 * Asserts the NEW dm_render produces, for each shared-corpus case at the US
 * locale, exactly the golden string CAPTURED FROM THE OLD WALK
 * (tests/parity/render/golden_us.h, recorded by the native_sim capture test).
 *
 * This is the parity proof for step 1: new == old, over a corpus, with the old
 * code as oracle. Until the golden is captured (DM_GOLDEN_US_CAPTURED == 0) the
 * test SKIPS rather than asserting against placeholders.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ztest_shim.h"

#include <zmk-behavior-dynamic-macros/dm_event.h>
#include <zmk-behavior-dynamic-macros/dm_render.h>

#include "render_corpus.h"
#include "golden_us.h"

/* buffer sink (same shape as test_render.c) */
struct pbuf {
    char buf[256];
    size_t pos;
};

static void pbuf_emit(void *ctx, char c) {
    struct pbuf *s = ctx;
    if (s->pos + 1 < sizeof(s->buf)) {
        s->buf[s->pos++] = c;
        s->buf[s->pos] = '\0';
    }
}
static bool pbuf_space(void *ctx, uint8_t n) {
    struct pbuf *s = ctx;
    return s->pos + n + 1 < sizeof(s->buf);
}

/* Find a corpus case by name (the golden references cases by name, not index,
 * since the golden is the subset captured from the old walk). */
static const struct dm_parity_case *corpus_by_name(const char *name) {
    for (int i = 0; i < DM_RENDER_CORPUS_LEN; i++) {
        if (strcmp(dm_render_corpus[i].name, name) == 0) {
            return &dm_render_corpus[i];
        }
    }
    return NULL;
}

ZTEST_SUITE(dm_render_parity, NULL, NULL, NULL, NULL, NULL);

/* dm_render must reproduce, for each old-walk-captured golden anchor, exactly
 * the preview the live old walk produced (decoded from the keymap snapshot). */
ZTEST(dm_render_parity, matches_old_walk_us) {
    if (!DM_GOLDEN_US_CAPTURED) {
        ztest_test_skip();
    }

    for (int i = 0; i < DM_GOLDEN_US_LEN; i++) {
        const struct dm_golden_entry *g = &dm_golden_us[i];
        const struct dm_parity_case *c = corpus_by_name(g->name);
        zassert_true(c != NULL, "golden references unknown corpus case %s", g->name);

        struct pbuf s;
        memset(&s, 0, sizeof(s));
        dm_sink sink = {.emit_char = pbuf_emit, .space_for = pbuf_space, .ctx = &s};
        dm_render_slot_view view = {.event_count = c->count, .events = c->events};
        dm_render_slot(&view, DM_LOCALE_US, &sink, NULL);

        zassert_str_equal(s.buf, g->expected,
                          "case %s: dm_render must match old-walk golden", g->name);
    }
}
