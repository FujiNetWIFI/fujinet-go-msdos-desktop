/*
 * The debugger's Video tab (Win32): a thin view over core/include/msdosvidtab.h. It is one child
 * window holding all its own controls, so the debugger shows, hides and places it as a unit.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_WINDOWS_DBG_VIDEO_H
#define MSDOS_WINDOWS_DBG_VIDEO_H

#include <windows.h>

#include "msdossession.h"

/* The tab's page, a child of `parent`, hidden until shown. ui and mono are the fonts it uses. */
HWND dbg_video_create(HWND parent, HFONT ui, HFONT mono, int dpi, msdossession *session);
/* Move and size the page (it lays out its own controls). */
void dbg_video_place(HWND page, int x, int y, int w, int h);
/* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
void dbg_video_refresh(HWND page, int force);

#endif
