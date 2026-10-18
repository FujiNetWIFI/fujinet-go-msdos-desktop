/*
 * FujiNet Go MS-DOS -- the macOS (AppKit) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include <signal.h>

#include "msdossession.h"

int main(int argc, const char **argv)
{
    @autoreleasepool {
        /* The View menu has its own Full Screen item (Command+Shift+F, as
         * the other frontends' Ctrl+Shift+F): AppKit is not to add another. */
        [[NSUserDefaults standardUserDefaults] setBool:NO forKey:@"NSFullScreenMenuItemEverywhere"];

        /* The runtime is packed beside the executable (see CMakeLists.txt);
         * tell the session where, so a fresh install provisions its tree
         * from the bundle rather than hunting. */
        msdossession_paths paths = { NULL, NULL, NULL, NULL };
        NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        NSString *runtime = [exeDir stringByAppendingPathComponent:@"fujinet"];
        if (exeDir && [[NSFileManager defaultManager] fileExistsAtPath:runtime])
            paths.fujinet_runtime_src = [runtime fileSystemRepresentation];

        msdossession *session = msdossession_new(&paths);
        if (!session) {
            NSLog(@"Could not create the session (unusable config or data directories?)");
            return 1;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* SIGTERM (a logout, a CI job's kill) and SIGINT (Ctrl+C in the
         * terminal) quit the way the menu does, so the session stops the
         * machine and FujiNet and flushes the settings instead of dying
         * mid-write. */
        static dispatch_source_t sigterm, sigint;
        signal(SIGTERM, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        sigterm = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        sigint = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sigterm, ^{ [NSApp terminate:nil]; });
        dispatch_source_set_event_handler(sigint, ^{ [NSApp terminate:nil]; });
        dispatch_resume(sigterm);
        dispatch_resume(sigint);

        /* A path on the command line is routed as a dropped file would be:
         * a floppy goes in A:, a hard disk asks for its drive, a BIOS is
         * imported. macOS adds -psn_ and -NS* arguments of its own: those
         * are not files. */
        /* NSApplication holds its delegate weakly: this keeps it */
        static MSDOSAppDelegate *delegate;
        delegate = [[MSDOSAppDelegate alloc]
            initWithSession:session mediaPath:(argc > 1 && argv[1][0] != '-' ? argv[1] : NULL)];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
