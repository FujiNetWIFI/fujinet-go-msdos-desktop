/*
 * The Win32 Controllers panel (Ctrl+Shift+K): the machine's own keyboard on
 * screen, the game port's two joysticks and the gamepads that drive them,
 * in a fixed-size tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "../win_common.h"

/* Show / hide. Created on first use, hidden not destroyed after. Keys typed
 * with the panel active go to parent (the main window) as its own. */
void msdos_controllers_toggle(HWND parent, msdossession *session);
int  msdos_controllers_visible(void);

/* The machine or a port's device changed: rebuild the keyboard and ports. */
void msdos_controllers_refresh(void);

/* Give the panel first refusal on a message from the main loop (a gamepad
 * combo box has the focus: its keys are the PC's). 1 if consumed. */
int msdos_controllers_pretranslate(MSG *msg);
