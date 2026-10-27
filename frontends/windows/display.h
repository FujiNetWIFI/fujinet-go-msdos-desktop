/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_WIN_DISPLAY_H
#define MSDOS_WIN_DISPLAY_H

#include <windows.h>
#include <stdint.h>

#include "msdossession.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND msdos_display_create(HWND parent, msdossession *session);
/* Stops the render thread; call before the parent is destroyed. */
void msdos_display_destroy(void);
/* integer: whole-number scale when it fits; smooth: bilinear (else sharp
 * pixels). Safe from any thread. */
void msdos_display_set_options(int integer, int smooth);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *msdos_display_renderer(void);
/* Where the picture is, in the display window's client coordinates;
 * 0 before the first frame. */
int msdos_display_shown(RECT *out);
/* New frames drawn since start (for the screenshot / autoquit hooks). */
long msdos_display_frames(void);
/* The last frame, locked: *w x *h pixels of 0x00RRGGBB at stride *w. Call
 * msdos_display_unlock when done. */
const uint32_t *msdos_display_lock(int *w, int *h);
void msdos_display_unlock(void);

#endif
