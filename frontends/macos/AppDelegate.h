/*
 * The application delegate: the window, the menu bar, and the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#import "KeyForward.h"

#include "msdossession.h"

@interface MSDOSAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate,
                                        NSToolbarDelegate, MSDOSKeyRouter>
/* mediaPath: a file named on the command line (a floppy, a hard disk, a
 * BIOS ...), or NULL. */
- (instancetype)initWithSession:(msdossession *)session mediaPath:(const char *)mediaPath;
@end
