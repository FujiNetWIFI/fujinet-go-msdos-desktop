/*
 * The PC's picture, and its mouse while captured.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "msdossession.h"

@interface MSDOSDisplayView : NSView
- (instancetype)initWithSession:(msdossession *)session;
- (void)setSmooth:(BOOL)smooth;
/* Whole multiples of the picture's size when it fits. */
- (void)setIntegerScale:(BOOL)on;
/* The next refresh takes a frame even if the PC drew nothing new: its
 * pixel aspect follows a 4:3 change at once. */
- (void)refreshFrame;
/* The mouse belongs to the PC: hidden and pinned, its movement sent as
 * mickeys. A click on the picture captures it (when a mouse is fitted);
 * Control+End, the middle button or the window losing the keyboard lets it
 * go. onCaptureChanged says when. */
- (void)setCaptured:(BOOL)on;
- (BOOL)captured;
@property (nonatomic, copy) void (^onCaptureChanged)(BOOL captured);
/* The last frame as a PNG (NO when there is none yet). */
- (BOOL)savePng:(NSString *)path;
- (BOOL)hasFrame;
/* Frames shown since it was made (MSDOS_SCREENSHOT_FRAMES,
 * MSDOS_AUTOQUIT_FRAMES), and the display's refreshes. */
- (uint64_t)frames;
- (uint64_t)ticks;
/* Stops the display link (the app is quitting). */
- (void)stop;
@end
