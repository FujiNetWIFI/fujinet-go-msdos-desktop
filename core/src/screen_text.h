/*
 * The PC's text screen as UTF-8, read from video memory the way DOS sees
 * it (the BIOS data area says which mode, page and size).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_SCREEN_TEXT_H
#define MSDOS_SCREEN_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Emulation thread only. Lines end in '\n', trailing blanks trimmed.
 * Returns the length written, or -1 when the screen is not a text mode. */
int msdos_screen_text(char *dst, int dstsz);

/* One CP437 character as UTF-8 (up to 3 bytes + NUL); returns the length. */
int msdos_cp437_to_utf8(unsigned char c, char *out);

/* The CP437 code for a Unicode code point, or -1. */
int msdos_utf8_to_cp437(unsigned codepoint);

#ifdef __cplusplus
}
#endif

#endif
