/*
 * What the main window offers the app's other windows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "win_common.h"

/* A key message typed in one of the app's panels, as if typed in the main
 * window: its shortcuts (Ctrl+Shift+letter, Ctrl+End, Ctrl+Alt+PgDn), else
 * the PC's key. 1 when it was used; 0 leaves it to the panel's own
 * DefWindowProc (Alt+F4, Alt+Space). */
int msdos_main_key(UINT msg, WPARAM vk, LPARAM lp);
