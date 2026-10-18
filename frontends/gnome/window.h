/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "msdossession.h"

#define MSDOS_TYPE_WINDOW (msdos_window_get_type())
G_DECLARE_FINAL_TYPE(MsdosWindow, msdos_window, MSDOS, WINDOW, AdwApplicationWindow)

GtkWidget *msdos_window_new(AdwApplication *app, msdossession *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *msdos_icon_name(void);
void msdos_window_toast(MsdosWindow *self, const char *text);

/* Starts the PC (FujiNet first), or shows the no-BIOS page. */
void msdos_window_start(MsdosWindow *self);

/* A key pressed or released with this window, or one of its panels,
 * active: the window's own keys (the Ctrl+Shift shortcuts, Ctrl+End,
 * Ctrl+Alt+PgDn), else the PC's. keycode is GDK's hardware keycode
 * (evdev + 8). TRUE when it was used. */
gboolean msdos_window_key(MsdosWindow *self, guint keyval, guint keycode,
                          GdkModifierType state, gboolean down);

/* A dropped or command-line file: Open's routing (a floppy goes in A:, a
 * hard disk asks which drive, a BIOS is imported, anything else goes on
 * FujiNet's SD card). */
void msdos_window_load_media(MsdosWindow *self, const char *path);

/* The view settings (smooth, whole-number scaling) from the store. */
void msdos_window_apply_view(MsdosWindow *self);

/* After Preferences or an import: the menus, the status and the page
 * (picture or no-BIOS) follow what changed. */
void msdos_window_settings_changed(MsdosWindow *self);

/* The accent colour (MSDOSSESSION_ACCENT_RGB) as CSS classes every window
 * can use: ".msdos-accent" fills a widget with it (dark text),
 * ".msdos-dot" with "-on"/"-off" a status light, ".msdos-held" a pressed
 * on-screen key. Installed by main.c. */
void msdos_install_accent_css(void);
