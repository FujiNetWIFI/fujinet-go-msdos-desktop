/*
 * The debugger window (Ctrl+Shift+D).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "msdossession.h"

/* Shows the debugger (attaching it stops the machine), or hides it
 * (detaching lets it run). */
void msdosDebuggerToggle(QWidget *parent, msdossession *session);
void msdosDebuggerShow(QWidget *parent, msdossession *session);
