/*
 * msdoshelp -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("drives-and-disks"); a Help button names a
 * context instead ("prefs/Storage", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSHELP_H
#define MSDOSHELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MSDOSHELP_GNOME = 1, MSDOSHELP_KDE = 2, MSDOSHELP_WINDOWS = 4, MSDOSHELP_MACOS = 8
};

typedef enum {
    MSDOSHELP_H2 = 0, MSDOSHELP_H3,
    MSDOSHELP_PARA,
    MSDOSHELP_BULLET, MSDOSHELP_BULLET2,    /* "- " and "  - " */
    MSDOSHELP_NUMBERED,                     /* a step; number is its own */
    MSDOSHELP_NOTE,
    MSDOSHELP_PRE,                          /* one TEXT span, lines as they are */
    MSDOSHELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    MSDOSHELP_TABLE_ROW
} msdoshelp_block_kind;

typedef enum {
    MSDOSHELP_TEXT = 0, MSDOSHELP_BOLD, MSDOSHELP_ITALIC, MSDOSHELP_CODE,
    MSDOSHELP_KBD,                          /* keys; mac_text is the Mac's */
    MSDOSHELP_LINK,                         /* target "#slug" or a URL */
    MSDOSHELP_CELL                          /* tables: the next column starts */
} msdoshelp_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} msdoshelp_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* MSDOSHELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const msdoshelp_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} msdoshelp_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const msdoshelp_block *blocks;
    int nblocks;
} msdoshelp_topic;

int msdoshelp_topic_count(void);
const msdoshelp_topic *msdoshelp_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int msdoshelp_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *msdoshelp_span_text(const msdoshelp_span *sp, unsigned platform);

/* A heading block's text. */
const char *msdoshelp_heading(const msdoshelp_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int msdoshelp_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int msdoshelp_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} msdoshelp_hit;
int msdoshelp_search(unsigned platform, const char *query, msdoshelp_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int msdoshelp_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* MSDOSHELP_H */
