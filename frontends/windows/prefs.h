/*
 * The Win32 Preferences window, built from the session's preference table
 * (msdossession_pref) like the other frontends' Preferences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "win_common.h"

/* Shows the window (raising it when open). Live rows apply at once and
 * call on_live (the view follows; a ROM import too); machine and host rows
 * apply together when it closes, which calls on_closed with the
 * msdos_apply it needed. */
void msdos_prefs_show(HWND parent, msdossession *session, void (*on_live)(void),
                      void (*on_closed)(int applied));
/* Quitting: close it without applying anything (the settings are saved). */
void msdos_prefs_abandon(void);
/* Tab and the arrow keys move between its controls. 1 if consumed. */
int msdos_prefs_pretranslate(MSG *msg);
