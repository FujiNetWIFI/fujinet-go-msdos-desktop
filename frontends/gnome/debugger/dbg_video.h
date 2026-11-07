/*
 * The debugger's Video tab (GTK4): a thin view over core/include/msdosvidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_GNOME_DBG_VIDEO_H
#define MSDOS_GNOME_DBG_VIDEO_H

#include <gtk/gtk.h>

#include "msdossession.h"

/* The tab's page. It owns its controller and frees it when destroyed. */
GtkWidget *dbg_video_new(msdossession *session);
/* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
void dbg_video_refresh(GtkWidget *page, gboolean force);

#endif
