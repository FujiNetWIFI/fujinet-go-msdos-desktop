/*
 * The debugger window (Ctrl+Shift+D).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "msdossession.h"

G_BEGIN_DECLS

/* Shows the debugger (attaching it stops the machine), or hides it
 * (detaching lets it run). */
void msdos_debugger_toggle(GtkWindow *parent, msdossession *session);
void msdos_debugger_show(GtkWindow *parent, msdossession *session);

G_END_DECLS
