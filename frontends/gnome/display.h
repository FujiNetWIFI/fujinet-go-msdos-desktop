/*
 * The PC's picture: a GtkWidget that pulls frames from the session on the
 * compositor's frame clock, and the mouse when it is captured.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "msdossession.h"

#define MSDOS_TYPE_DISPLAY (msdos_display_get_type())
G_DECLARE_FINAL_TYPE(MsdosDisplay, msdos_display, MSDOS, DISPLAY, GtkWidget)

GtkWidget *msdos_display_new(msdossession *session);
void msdos_display_set_smooth(MsdosDisplay *self, gboolean smooth);
void msdos_display_set_integer_scale(MsdosDisplay *self, gboolean on);
/* The mouse belongs to the PC: hidden, its movement sent as mickeys. A click
 * on the picture captures it (when a mouse is fitted); Ctrl+End or the
 * middle button lets it go. "capture-changed" says when. */
void msdos_display_set_captured(MsdosDisplay *self, gboolean on);
gboolean msdos_display_captured(MsdosDisplay *self);
/* Saves the last frame as a PNG; FALSE on failure. */
gboolean msdos_display_save_png(MsdosDisplay *self, const char *path);
/* Frames shown since it was made (MSDOS_AUTOQUIT_FRAMES). */
guint64 msdos_display_frames(MsdosDisplay *self);
