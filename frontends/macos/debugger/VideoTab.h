/*
 * The debugger's Video tab (AppKit): a thin view over core/include/msdosvidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "msdossession.h"

@interface MSDOSVideoTab : NSObject

/* The tab's content view; it owns the controller, which is freed with the object. */
@property (nonatomic, readonly) NSView *view;

- (instancetype)initWithSession:(msdossession *)session;
/* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
- (void)refreshForce:(BOOL)force;

@end
