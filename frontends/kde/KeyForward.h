/*
 * Qt key events to the PC.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QKeyEvent>
#include <cstdint>

#include "msdossession.h"

/* The physical key as a USB HID usage (from the native scan code: evdev + 8
 * under both xcb and wayland), or 0. */
uint32_t msdosHidFromQt(const QKeyEvent *e);

/* Ctrl+Shift+letter and Super/Meta combinations are the app's (and the
 * desktop's): never the PC's. */
bool msdosAppKey(const QKeyEvent *e);

/* Sends a press (down) or release to the PC by position: true when it took
 * it. Auto-repeated presses go as the keyboard's typematic repeats; their
 * releases are dropped. F1-F12 and the Ctrl and Alt combinations are the
 * PC's. */
bool msdosForwardKey(msdossession *session, const QKeyEvent *e, bool down);
