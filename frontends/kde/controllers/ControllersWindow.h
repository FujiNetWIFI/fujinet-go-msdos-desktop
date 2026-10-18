/*
 * The Controllers panel (Ctrl+Shift+K): the machine's own keyboard on
 * screen, the game port's two joysticks, and the gamepads that drive them.
 * Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>
#include <QWidget>
#include <functional>

#include "msdossession.h"

/* What a key typed with the panel active does: the main window's handler
 * (its own keys, else the PC's). True when it was used. */
using MsdosKeyHandler = std::function<bool(QKeyEvent *, bool down)>;

/* Shows the panel, or hides it when it is showing. */
void msdosControllersToggle(QWidget *parent, msdossession *session, const MsdosKeyHandler &onKey);
bool msdosControllersVisible();
/* The machine or the game port changed: rebuild the keyboard and sticks. */
void msdosControllersRefresh();
