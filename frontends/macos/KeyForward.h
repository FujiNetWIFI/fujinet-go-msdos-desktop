/*
 * AppKit key events to the PC: the hardware key code through the session's
 * table (msdossession_hid_from_macos_keycode) as a USB HID usage, pressed
 * and released. The PC keyboard is positional, so position is what
 * travels: F1-F12, Control, Option (the PC's Alt) and every combination of
 * them go to the PC. Command is the Mac's: anything with it held is a menu
 * shortcut.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "msdossession.h"

/* Where a window's keys go: the main window's own keys (Control+End lets
 * the mouse go, Control+Option+Page Down is full screen), else the PC. The
 * app delegate is one; the Controllers panel hands its keys to it, as the
 * GNOME panel does through msdos_window_key. */
@protocol MSDOSKeyRouter <NSObject>
/* keyDown (down YES) or keyUp: YES when it was used. */
- (BOOL)routeKey:(NSEvent *)event down:(BOOL)down;
/* flagsChanged: Shift, Control, Option and Caps Lock. */
- (void)routeFlags:(NSEvent *)event;
@end

/* The HID usage for a hardware key code, or 0. A PC keyboard on a Mac
 * reports Insert as Help and Print Screen, Scroll Lock and Pause as F13-F15:
 * those come back as the PC's keys. */
uint32_t MSDOSHidFromKeyCode(unsigned short keyCode);

/* A keyDown/keyUp for the PC: YES when it took it. A key going down with
 * Command held is the menus'; a key coming up always goes (it must not
 * stick). Auto-repeats go through: they are the PC keyboard's typematic. */
BOOL MSDOSForwardKey(msdossession *session, NSEvent *event, BOOL down);

/* flagsChanged: Shift, Control and Option (left and right apart) as the
 * PC's Shift, Ctrl and Alt; Caps Lock as a tap of the PC's. Command going
 * down lets go of every key the PC holds (AppKit sends no key-up for a key
 * released while Command is down). */
void MSDOSForwardFlags(msdossession *session, NSEvent *event);

/* The accent (MSDOSSESSION_ACCENT_RGB, the IBM badge's silver): a fill only,
 * with the ink (#1E1E1E) for text on it. */
NSColor *MSDOSAccentColor(void);
NSColor *MSDOSInkColor(void);
