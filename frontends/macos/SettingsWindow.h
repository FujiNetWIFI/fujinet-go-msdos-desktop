/*
 * The Preferences window, built from the session's preference table
 * (msdossession_pref) like the other frontends' Preferences.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "msdossession.h"

@interface MSDOSSettingsWindow : NSObject <NSWindowDelegate, NSTabViewDelegate>
/* onLive runs after each live change (the view follows); onMachine when
 * the machine changed (the Controllers panel's keyboard follows); onImport
 * after a ROM import, with what it said; onClosed when the window closes,
 * with the msdos_apply that applying the rest needed. */
- (instancetype)initWithSession:(msdossession *)session
                         onLive:(void (^)(void))onLive
                      onMachine:(void (^)(void))onMachine
                       onImport:(void (^)(NSString *message))onImport
                       onClosed:(void (^)(int applied))onClosed;
- (void)show;
@end
