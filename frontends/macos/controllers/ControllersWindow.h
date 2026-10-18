/*
 * The Controllers panel (Cmd+Shift+K): the machine's own keyboard on screen,
 * the game port's two joysticks, and the gamepads that drive them. Hidden
 * at start; a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#import "../KeyForward.h"

#include "msdossession.h"

@interface MSDOSControllersWindow : NSObject
/* router: where keys typed with the panel key go (the main window's). */
- (instancetype)initWithSession:(msdossession *)session router:(id<MSDOSKeyRouter>)router;
/* Show, or hide when showing. */
- (void)toggle;
- (BOOL)isVisible;
/* The machine or a port's device changed: rebuild the keyboard and ports. */
- (void)refresh;
@end
