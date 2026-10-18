/*
 * The Preferences dialog.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "msdossession.h"
#include "window.h"

G_BEGIN_DECLS

/* Shows the Preferences (built from msdossession_pref). Live rows apply at
 * once; machine and host rows apply together when it closes. */
void msdos_prefs_show(MsdosWindow *parent, msdossession *session);

G_END_DECLS
