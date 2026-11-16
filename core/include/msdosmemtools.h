/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a symbol, an expression, seg:off,
 * or a range low-high / low..high / low l len) shown with its live value. It
 * is stored as text and resolved every time it is shown, so loading symbols
 * later makes a label start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSMEMTOOLS_H
#define MSDOSMEMTOOLS_H

#include <stdint.h>

#include "msdosdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "low..high", "low-high", "low l len" (DEBUG's), or one address expression
 * (hex linear, seg:off, symbol, symbol+3, [ptr], a register pair). A bare
 * token of hex digits ("B800") is hex unless it names a symbol; "$B800:0" and
 * expressions work as in the console. Returns 1 and fills low and high
 * (inclusive, low <= high), 0 if the text means nothing. */
int msdossym_parse_range(msdosdebug *dbg, const char *text, uint32_t *low, uint32_t *high);

/* ---- watches ---- */
#define MSDOSWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  msdoswatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  msdoswatch_add(const char *text);
void msdoswatch_remove(int index);
void msdoswatch_clear(void);
/* Copies into dst (the list may change under another thread). Returns the length, 0 past the end. */
int  msdoswatch_text(int index, char *dst, int dstsz);
unsigned msdoswatch_generation(void);
/* "screen  0B8000  $41 (65)". While the PC runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  msdoswatch_format(msdosdebug *dbg, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  msdoswatch_range(msdosdebug *dbg, int index, uint32_t *low, uint32_t *high);

/* ---- memory search ---- */
enum { MSDOSSEARCH_HEX = 0, MSDOSSEARCH_TEXT };
#define MSDOSSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, count, mask (0xFF) and flags for
 * msdosdebug_search. Returns 1, or 0 for empty / malformed text. */
int msdossearch_parse(int kind, const char *text, uint8_t *pat, int *count, uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  0B8000-0B8FFF (video)  hits 3  if value != old". Returns the length. */
int msdosbp_describe(msdosdebug *dbg, const msdos_breakpoint *bpt, char *dst, int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from msdosdebug_memory_map) holding linear,
 * for labelling Memory rows. Fetch the map once per refresh. Returns the
 * length, 0 and "" when no region covers it. */
int msdosmem_region_name(const msdos_region *map, int count, uint32_t linear, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* MSDOSMEMTOOLS_H */
