/*
 * The Win32 debugger window over the session's debugger (msdosdebug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "../win_common.h"

/* Show: created on first use, hidden (and detached, so the machine runs on)
 * when closed. Showing it attaches the engine, which stops the machine. */
void msdos_debugger_show(HWND parent, msdossession *session);

/* Ctrl+Shift+D: show, or hide and let the machine run. */
void msdos_debugger_toggle(HWND parent, msdossession *session);
int  msdos_debugger_visible(void);

/* Accelerators (F5/F7/F8/Shift+F8/Ctrl+Shift+D) for the main loop's
 * pretranslate hook. Returns 1 if the message was consumed. */
int msdos_debugger_pretranslate(MSG *msg);
