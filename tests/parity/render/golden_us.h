/*
 * Copyright (c) 2026 Benjamin H
 *
 * SPDX-License-Identifier: MIT
 *
 * US-locale render golden, CAPTURED FROM THE LIVE OLD WALK (do not hand-edit).
 *
 * Source of truth: the native_sim capture test (tests/parity/render/capture_test.c)
 * runs dm_get_preview_string over tests/parity/render_corpus.h and prints
 * "GOLDEN\t<name>\t\"<string>\"" lines. Transcribe those lines here, in corpus
 * order. Re-run the capture test and re-transcribe whenever the corpus changes.
 *
 * Until captured, DM_GOLDEN_US_CAPTURED is 0 and the host parity test SKIPS
 * (it must never assert against invented strings — that would prove nothing).
 */

#ifndef DM_GOLDEN_US_H
#define DM_GOLDEN_US_H

/* Set to 1 once the strings below are transcribed from a capture-test run. */
#define DM_GOLDEN_US_CAPTURED 0

/* { corpus case name, expected preview string } — in dm_render_corpus order.
 * PLACEHOLDERS until captured; values come verbatim from the capture log. */
struct dm_golden_entry {
    const char *name;
    const char *expected;
};

static const struct dm_golden_entry dm_golden_us[] = {
    {"literal_ac",  "<UNCAPTURED>"},
    {"ctrl_c",      "<UNCAPTURED>"},
    {"shift_3",     "<UNCAPTURED>"},
    {"space_ret",   "<UNCAPTURED>"},
    {"ctrl_alt_z",  "<UNCAPTURED>"},
    {"mouse_left",  "<UNCAPTURED>"},
};

#endif /* DM_GOLDEN_US_H */
