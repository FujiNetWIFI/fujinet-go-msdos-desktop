/*
 * The debugger window (Cmd+Shift+D) over the debugger engine (msdosdebug.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "msdossession.h"

@interface MSDOSDebuggerWindow : NSObject
- (instancetype)initWithSession:(msdossession *)session;
/* Show (attaching stops the machine), or hide (detaching lets it run). */
- (void)show;
- (void)hide;
- (void)toggle;
- (BOOL)isVisible;
@end
