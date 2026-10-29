/*
 * MSDOSAppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * The same app as the GNOME frontend (frontends/gnome/window.c): the PC's
 * picture, a toolbar with the Ctrl+Alt+Del and Reset buttons and the status
 * (the FujiNet link, the machine, the drives' lights), menus for the
 * machine, its drives, the clipboard, the view and FujiNet, and the
 * Controllers, Preferences, debugger and FujiNet log windows. With no BIOS
 * imported yet it shows how to import one instead of the picture.
 *
 * Keys go to the PC pressed and released, by physical key: the PC keyboard
 * is positional, F1-F12 and the Control and Option (Alt) combinations
 * included. Command is the Mac's: the app's shortcuts are Command ones
 * (Command+Shift where GNOME's are Ctrl+Shift), plus Control+End (let the
 * mouse go) and Control+Option+Page Down (full screen), as in 86Box.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#import "DisplayView.h"
#import "SettingsWindow.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"
#import "help/HelpWindow.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef MSDOS_VERSION_STRING
#define MSDOS_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go MS-DOS"
#define STATUS_W 460.0          /* the toolbar's status area */
#define STATUS_H 24.0
#define LIGHT_W 26.0
#define LIGHT_STEP 28.0

static NSToolbarItemIdentifier const kCadItem = @"online.fujinet.go.msdos.cad";
static NSToolbarItemIdentifier const kResetItem = @"online.fujinet.go.msdos.reset";
static NSToolbarItemIdentifier const kStatusItem = @"online.fujinet.go.msdos.status";

/* Drive menu items: the tag is the drive and the action. */
enum {
    FD_INSERT = 0, FD_EJECT, FD_PROTECT, FD_BOOT, FD_BLANK,
    HD_ATTACH = 8, HD_DETACH, HD_BLANK,
};
#define DRIVE_TAG(d, action) ((d) * 16 + (action))

static NSString *str(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* MSDOS_OPEN_* hooks: set, and not "0". */
static BOOL envOn(const char *name)
{
    const char *v = getenv(name);
    return v && *v && *v != '0';
}

@interface MSDOSAppDelegate ()
- (BOOL)pcPageShowing;
- (BOOL)pcRunning;
- (void)loadMedia:(NSString *)path;
- (void)copyScreenText:(id)sender;
- (void)pasteToPC:(id)sender;
@end

/* ---- views ---------------------------------------------------------------------- */

/* The content view sits between AppKit and the session: it takes the keys
 * (the window's first responder) and the drop, and leaves the display
 * purely about pixels and the mouse. */
@interface MSDOSContentView : NSView
@property (nonatomic, weak) MSDOSAppDelegate *owner;
@end

@implementation MSDOSContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    /* the no-BIOS page has the keyboard: its buttons, the window's keys */
    if (![self.owner pcPageShowing]) {
        [super keyDown:e];
        return;
    }
    (void)[self.owner routeKey:e down:YES];
}

- (void)keyUp:(NSEvent *)e
{
    (void)[self.owner routeKey:e down:NO];
}

- (void)flagsChanged:(NSEvent *)e
{
    [self.owner routeFlags:e];
    [super flagsChanged:e];
}

/* The Edit menu's own Copy and Paste (Command+C, Command+V), with the main
 * window first: the screen's text, and typing the clipboard. */
- (void)copy:(id)sender { [self.owner copyScreenText:sender]; }
- (void)paste:(id)sender { [self.owner pasteToPC:sender]; }

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(copy:) || item.action == @selector(paste:)) return [self.owner pcRunning];
    return NO;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]]
                                                               options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if ([urls count] == 0) return NO;
    NSString *path = [(NSURL *)urls[0] path];
    MSDOSAppDelegate *owner = self.owner;
    /* after the drop: a question about it must not hold Finder's drag */
    dispatch_async(dispatch_get_main_queue(), ^{ [owner loadMedia:path]; });
    return YES;
}
@end

/* The FujiNet link: the accent when the COM device is connected. */
@interface MSDOSDot : NSView
@property (nonatomic) BOOL on;
@end

@implementation MSDOSDot
- (void)setOn:(BOOL)on
{
    if (on == _on) return;
    _on = on;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *p = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect([self bounds], 1, 1)];
    if (_on) {
        [MSDOSAccentColor() setFill];
        [p fill];
        [[[NSColor labelColor] colorWithAlphaComponent:0.4] setStroke];
        [p setLineWidth:1];
        [p stroke];
    } else {
        [[[NSColor labelColor] colorWithAlphaComponent:0.25] setFill];
        [p fill];
    }
}
@end

/* A drive's light: its letter, filled with the accent while it works. */
@interface MSDOSDriveLight : NSView
@property (nonatomic, copy) NSString *text;
@property (nonatomic) BOOL on;
@end

@implementation MSDOSDriveLight
- (void)setOn:(BOOL)on
{
    if (on == _on) return;
    _on = on;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    NSColor *ink = [NSColor secondaryLabelColor];
    if (_on) {
        [MSDOSAccentColor() setFill];
        [[NSBezierPath bezierPathWithRoundedRect:b xRadius:3 yRadius:3] fill];
        ink = MSDOSInkColor();
    }
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightMedium],
                             NSForegroundColorAttributeName: ink,
                             NSParagraphStyleAttributeName: para };
    const NSSize s = [self.text sizeWithAttributes:attrs];
    [self.text drawInRect:NSMakeRect(0, floor((b.size.height - s.height) / 2), b.size.width, s.height)
           withAttributes:attrs];
}
@end

/* The toolbar's status area: sized by its contents, laid out by hand. */
@interface MSDOSStatusView : NSView
@end

@implementation MSDOSStatusView
- (NSSize)intrinsicContentSize { return NSMakeSize(STATUS_W, STATUS_H); }
@end

/* A passing message over the picture's foot (GNOME's toasts). Clicks go
 * through it. */
@interface MSDOSToast : NSView
- (void)show:(NSString *)text;
@end

@implementation MSDOSToast {
    NSTextField *_label;
    unsigned _serial;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 8;
    self.layer.backgroundColor = [[NSColor colorWithWhite:0.12 alpha:0.9] CGColor];
    _label = [NSTextField wrappingLabelWithString:@""];
    _label.textColor = [NSColor whiteColor];
    _label.alignment = NSTextAlignmentCenter;
    [self addSubview:_label];
    self.hidden = YES;
    return self;
}

- (NSView *)hitTest:(NSPoint)p
{
    (void)p;
    return nil;
}

- (void)show:(NSString *)text
{
    NSView *sup = self.superview;
    if (!sup || text.length == 0) return;
    const NSRect b = sup.bounds;
    const CGFloat maxW = MAX(120.0, MIN(560.0, b.size.width - 80.0));
    _label.stringValue = text;
    NSSize s = [_label.cell cellSizeForBounds:NSMakeRect(0, 0, maxW, 10000)];
    s.width = ceil(MIN(s.width, maxW));
    s.height = ceil(s.height);
    self.frame = NSMakeRect(floor(NSMidX(b) - (s.width + 32) / 2), 24, s.width + 32, s.height + 20);
    _label.frame = NSMakeRect(16, 10, s.width, s.height);
    self.hidden = NO;
    const unsigned serial = ++_serial;
    const double seconds = text.length > 80 ? 6.0 : 3.0;
    __weak MSDOSToast *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        MSDOSToast *t = weakSelf;
        if (t && t->_serial == serial) t.hidden = YES;
    });
}
@end

/* ---- the delegate ------------------------------------------------------------------ */

@implementation MSDOSAppDelegate {
    msdossession *_session;
    const char *_mediaPath;
    NSString *_pendingMedia;     /* a file Finder handed over before launch finished */
    BOOL _launched;
    BOOL _quitting;
    NSWindow *_window;
    MSDOSContentView *_content;
    MSDOSDisplayView *_display;
    NSView *_noBios;
    MSDOSToast *_toast;
    NSButton *_cadButton, *_resetButton;
    MSDOSStatusView *_statusView;
    MSDOSDot *_linkDot;
    NSTextField *_statusText;
    MSDOSDriveLight *_lights[4];
    NSTimer *_statusTimer, *_fastTimer;
    unsigned _padGeneration, _fujinetGeneration;
    NSString *_shotPath, *_helpShotPath;
    uint64_t _shotFrames, _quitFrames;

    NSMenu *_drivesMenu;

    MSDOSControllersWindow *_controllers;
    MSDOSDebuggerWindow *_debugger;
    MSDOSSettingsWindow *_settings;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(msdossession *)session mediaPath:(const char *)mediaPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mediaPath = mediaPath;
    return self;
}

- (BOOL)pcRunning
{
    return _session && msdossession_is_running(_session);
}

- (BOOL)pcPageShowing
{
    return _session && _display && !_display.hidden;
}

- (NSString *)lastError
{
    const char *e = _session ? msdossession_last_error(_session) : NULL;
    return (e && e[0]) ? str(e) : @"The PC is not running.";
}

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text.length) [a setInformativeText:text];
    [a runModal];
}

- (void)toast:(NSString *)text
{
    [_toast show:text];
}

/* NSTimers that keep running while a menu or a modal alert is up. */
- (NSTimer *)timerEvery:(NSTimeInterval)seconds do:(void (^)(void))block
{
    NSTimer *t = [NSTimer timerWithTimeInterval:seconds repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        block();
    }];
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    return t;
}

/* ---- launch ------------------------------------------------------------------- */

- (void)buildToolbarViews
{
    _cadButton = [NSButton buttonWithTitle:@"Ctrl+Alt+Del" target:self action:@selector(ctrlAltDel:)];
    _cadButton.toolTip = @"Send Ctrl+Alt+Del: DOS restarts (a warm boot)";
    [_cadButton setRefusesFirstResponder:YES];
    _resetButton = [NSButton buttonWithTitle:@"Reset" target:self action:@selector(resetPC:)];
    _resetButton.toolTip = @"Press the reset button: the PC starts again from cold (⌘⇧R)";
    [_resetButton setRefusesFirstResponder:YES];

    /* the link dot, the status line, the drives' lights */
    _statusView = [[MSDOSStatusView alloc] initWithFrame:NSMakeRect(0, 0, STATUS_W, STATUS_H)];
    _linkDot = [[MSDOSDot alloc] initWithFrame:NSMakeRect(4, 6, 12, 12)];
    _linkDot.toolTip = @"FujiNet link";
    [_statusView addSubview:_linkDot];
    _statusText = [NSTextField labelWithString:@"Starting…"];
    _statusText.textColor = [NSColor secondaryLabelColor];
    _statusText.lineBreakMode = NSLineBreakByTruncatingTail;
    _statusText.frame = NSMakeRect(22, 3, STATUS_W - 22, 18);
    [_statusView addSubview:_statusText];
    static NSString *const names[4] = { @"A:", @"B:", @"C:", @"D:" };
    for (int i = 0; i < 4; i++) {
        _lights[i] = [[MSDOSDriveLight alloc] initWithFrame:NSMakeRect(0, 4, LIGHT_W, 16)];
        _lights[i].text = names[i];
        _lights[i].toolTip = @"Drive activity";
        _lights[i].hidden = YES;
        [_statusView addSubview:_lights[i]];
    }
}

- (NSView *)buildNoBios:(NSRect)frame
{
    NSImageView *icon = [NSImageView imageViewWithImage:[NSImage imageNamed:NSImageNameComputer]];
    [icon.widthAnchor constraintEqualToConstant:96].active = YES;
    [icon.heightAnchor constraintEqualToConstant:96].active = YES;
    NSTextField *title = [NSTextField labelWithString:@"No PC BIOS"];
    title.font = [NSFont boldSystemFontOfSize:22];
    NSTextField *desc = [NSTextField wrappingLabelWithString:
        @"IBM's and the clone makers' BIOS ROMs are not part of the app. Import the ones you have "
        @"— single images, a zip, or a folder such as an 86Box ROM set — and the PC starts."];
    desc.alignment = NSTextAlignmentCenter;
    desc.textColor = [NSColor secondaryLabelColor];
    desc.preferredMaxLayoutWidth = 460;
    [desc.widthAnchor constraintEqualToConstant:460].active = YES;
    NSButton *files = [NSButton buttonWithTitle:@"Import BIOS…" target:self action:@selector(importBios:)];
    files.bezelColor = [NSColor controlAccentColor];
    NSButton *folder = [NSButton buttonWithTitle:@"Import a Folder…" target:self action:@selector(importBiosFolder:)];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[files, folder, MSDOSHelpButton(@"nobios")]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 12;
    NSButton *get = [NSButton buttonWithTitle:@"Get the 86Box ROM set" target:self action:@selector(openRomSet:)];
    get.bordered = NO;
    get.contentTintColor = [NSColor linkColor];
    NSStackView *column = [NSStackView stackViewWithViews:@[icon, title, desc, buttons, get]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment = NSLayoutAttributeCenterX;
    column.spacing = 14;
    column.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *page = [[NSView alloc] initWithFrame:frame];
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [page addSubview:column];
    [column.centerXAnchor constraintEqualToAnchor:page.centerXAnchor].active = YES;
    [column.centerYAnchor constraintEqualToAnchor:page.centerYAnchor].active = YES;
    return page;
}

- (void)openRomSet:(id)sender
{
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@MSDOSSESSION_ROMS_URL]];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* 640 x 480 at one and a half */
    const NSRect frame = NSMakeRect(0, 0, 960, 720);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setReleasedWhenClosed:NO];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window setTabbingMode:NSWindowTabbingModeDisallowed];
    [_window setContentMinSize:NSMakeSize(800, 480)];

    [self buildToolbarViews];
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"online.fujinet.go.msdos.main"];
    toolbar.delegate = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    toolbar.autosavesConfiguration = NO;
    [_window setToolbar:toolbar];
    [_window setToolbarStyle:NSWindowToolbarStyleUnified];
    /* the toolbar's room is the buttons' and the status's (the title is
     * still the window's name in the Window menu and Mission Control) */
    [_window setTitleVisibility:NSWindowTitleHidden];

    _content = [[MSDOSContentView alloc] initWithFrame:frame];
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    _display = [[MSDOSDisplayView alloc] initWithSession:_session];
    [_display setFrame:frame];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    __weak MSDOSAppDelegate *weakSelf = self;
    _display.onCaptureChanged = ^(BOOL on) {
        if (on) [weakSelf toast:@"Mouse captured: Control+End or the middle button lets it go"];
    };
    [_content addSubview:_display];

    _noBios = [self buildNoBios:frame];
    _noBios.hidden = YES;
    [_content addSubview:_noBios];

    _toast = [[MSDOSToast alloc] initWithFrame:NSMakeRect(0, 24, 100, 40)];
    [_toast setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin];
    [_content addSubview:_toast];

    [_window setContentView:_content];
    [_window center];
    [self buildMenu];
    [self applyView];
    [_window setDelegate:self];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];

    _controllers = [[MSDOSControllersWindow alloc] initWithSession:_session router:self];
    _debugger = [[MSDOSDebuggerWindow alloc] initWithSession:_session];

    /* the current pads and FujiNet state are the baseline: no toasts for them */
    _padGeneration = msdossession_gamepad_generation(_session);
    _fujinetGeneration = msdossession_fujinet_generation(_session);
    _statusTimer = [self timerEvery:1.0 do:^{ [weakSelf updateStatus]; }];
    _fastTimer = [self timerEvery:0.1 do:^{ [weakSelf fastTick]; }];
    [self updateStatus];
    [self updateLights];
    [self showPage];

    /* MSDOS_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch, MSDOS_OPEN_HELP=<slug, context or ?words> the Help ("1": its
     * start). MSDOS_SCREENSHOT=file.png saves the picture after
     * MSDOS_SCREENSHOT_FRAMES frames (300), MSDOS_HELP_SCREENSHOT=file.png the
     * Help then; MSDOS_AUTOQUIT_FRAMES quits after that many. With no PC
     * running (no BIOS imported, as on CI) the frames are the display's
     * refreshes and the screenshot is the window's. */
    if (envOn("MSDOS_OPEN_CONTROLLERS")) [_controllers toggle];
    if (envOn("MSDOS_OPEN_DEBUGGER")) [_debugger show];
    if (envOn("MSDOS_OPEN_SETTINGS")) [self showSettings:nil];
    if (envOn("MSDOS_OPEN_HELP"))
        [[MSDOSHelpWindow shared] showAt:strcmp(getenv("MSDOS_OPEN_HELP"), "1") ? str(getenv("MSDOS_OPEN_HELP")) : nil];
    if (getenv("MSDOS_HELP_SCREENSHOT") && *getenv("MSDOS_HELP_SCREENSHOT"))
        _helpShotPath = str(getenv("MSDOS_HELP_SCREENSHOT"));
    {
        const char *n = getenv("MSDOS_SCREENSHOT_FRAMES");
        _shotFrames = n ? strtoull(n, NULL, 10) : 300;
    }
    if (getenv("MSDOS_SCREENSHOT") && *getenv("MSDOS_SCREENSHOT"))
        _shotPath = str(getenv("MSDOS_SCREENSHOT"));
    if (getenv("MSDOS_AUTOQUIT_FRAMES")) _quitFrames = strtoull(getenv("MSDOS_AUTOQUIT_FRAMES"), NULL, 10);

    /* FujiNet first, then the machine (or the no-BIOS page) */
    [self startPC];
    _launched = YES;

    /* A file from the command line or from Finder goes where Open puts it. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;
    if (media) [self loadMedia:media];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    _quitting = YES;
    [_statusTimer invalidate];
    [_fastTimer invalidate];
    [_logTimer invalidate];
    if (!_session) return;
    [_debugger hide];               /* detaches: the machine is let go first */
    [_display setCaptured:NO];
    [_display stop];
    msdossession_stop(_session);
    msdossession_settings_flush(_session);
    msdossession_free(_session);
    _session = NULL;
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    /* Opening a file from Finder launches the app and delivers the file
     * before applicationDidFinishLaunching:, when there is no window yet:
     * hold it until there is. */
    if (!_launched) {
        _pendingMedia = filename;
        return YES;
    }
    [self loadMedia:filename];
    return YES;
}

/* Focus gone: nothing stays held (the Controllers panel takes typing too),
 * and the mouse is the desktop's again. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object != _window || !_session) return;
    if (![_controllers isVisible]) msdossession_release_keys(_session);
    [_display setCaptured:NO];
}

/* The Controllers, debugger and log windows stay about while hidden;
 * closing the main window is quitting. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _window) {
        if (!_quitting) dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    } else if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
    }
}

/* ---- the keyboard ---------------------------------------------------------------- */

/* The main window's (and the Controllers panel's) keys: Control+End lets
 * the mouse go, Control+Option+Page Down is full screen, the rest are the
 * PC's. */
- (BOOL)routeKey:(NSEvent *)e down:(BOOL)down
{
    const NSEventModifierFlags f = [e modifierFlags];
    const BOOL ctrl = (f & NSEventModifierFlagControl) != 0;
    const BOOL alt = (f & NSEventModifierFlagOption) != 0;
    const BOOL cmd = (f & NSEventModifierFlagCommand) != 0;
    if (!_session) return NO;
    if (down && !cmd) {
        if (ctrl && !alt && [e keyCode] == kVK_End && [_display captured]) {
            [_display setCaptured:NO];
            return YES;
        }
        if (ctrl && alt && [e keyCode] == kVK_PageDown) {
            if (![e isARepeat]) [self toggleFullScreen:nil];
            return YES;
        }
    }
    return MSDOSForwardKey(_session, e, down);
}

- (void)routeFlags:(NSEvent *)e
{
    MSDOSForwardFlags(_session, e);
}

/* ---- starting ------------------------------------------------------------------------ */

/* The picture, or the no-BIOS page; the machine's buttons while it runs. */
- (void)showPage
{
    if (!_session) return;
    const BOOL bios = msdossession_has_bios(_session) != 0;
    const BOOL running = [self pcRunning];
    if (!bios) [_display setCaptured:NO];
    _display.hidden = !bios;
    _noBios.hidden = bios;
    MSDOSSetHelpContext(_window, bios ? @"main" : @"nobios");
    _cadButton.enabled = running;
    _resetButton.enabled = running;
    if (bios && _window.firstResponder != _content && _window.firstResponder == _window)
        [_window makeFirstResponder:_content];
}

- (void)startPC
{
    [self showPage];
    if ([self pcRunning] || !msdossession_has_bios(_session)) return;
    msdossession_start_opts opts;
    msdossession_default_opts(_session, &opts);
    if (msdossession_start(_session, &opts) != 0) {
        NSString *why = [self lastError];
        /* after launch finishes: a modal alert inside it stalls the menu bar */
        dispatch_async(dispatch_get_main_queue(), ^{ [self alert:@"Cannot Start the PC" text:why]; });
        [self updateStatus];
        return;
    }
    [self updateStatus];
    [self updateLights];
    [self showPage];
}

/* ---- status, and news from the other threads ------------------------------------- */

- (void)updateStatus
{
    if (!_session) return;
    NSString *text;
    BOOL link = NO;
    if (![self pcRunning]) {
        text = msdossession_has_bios(_session) ? @"Off" : @"No BIOS";
    } else {
        char buf[512];
        buf[0] = '\0';
        msdossession_status(_session, buf, (int)sizeof buf);
        text = str(buf);
        link = msdossession_fujinet_link(_session) != 0;
    }
    _statusText.stringValue = text;
    _statusText.toolTip = text;
    _linkDot.on = link;
    _cadButton.enabled = [self pcRunning];
    _resetButton.enabled = [self pcRunning];
}

/* The drives' lights, at the status's right end: those the machine has. */
- (void)updateLights
{
    if (!_session) return;
    const BOOL running = [self pcRunning];
    BOOL present[4], on[4];
    int shown = 0;
    for (int i = 0; i < 4; i++) {
        if (i < 2) {
            msdos_floppy_info fi;
            memset(&fi, 0, sizeof fi);
            msdossession_floppy_info(_session, i, &fi);
            present[i] = fi.present != 0;
            on[i] = fi.active != 0;
        } else {
            msdos_hd_info hi;
            memset(&hi, 0, sizeof hi);
            msdossession_hd_info(_session, i - 2, &hi);
            present[i] = hi.path[0] != 0;
            on[i] = hi.active != 0;
        }
        present[i] = present[i] && running;
        if (present[i]) shown++;
    }
    CGFloat x = STATUS_W - shown * LIGHT_STEP;
    for (int i = 0; i < 4; i++) {
        _lights[i].hidden = !present[i];
        _lights[i].on = on[i];
        if (!present[i]) continue;
        _lights[i].frame = NSMakeRect(x + (LIGHT_STEP - LIGHT_W), 4, LIGHT_W, 16);
        x += LIGHT_STEP;
    }
    const CGFloat textW = STATUS_W - 22 - shown * LIGHT_STEP - 4;
    if (fabs(_statusText.frame.size.width - textW) > 0.5)
        _statusText.frame = NSMakeRect(22, 3, textW, 18);
}

/* Gamepads come and go, FujiNet restarts itself, 86Box has something to
 * say: the other threads cannot call into AppKit, so this timer asks. */
- (void)fastTick
{
    if (!_session) return;
    char text[1024];
    unsigned gen = msdossession_gamepad_generation(_session);
    if (gen != _padGeneration) {
        _padGeneration = gen;
        text[0] = '\0';
        if (msdossession_gamepad_last_event(_session, text, (int)sizeof text) > 0) [self toast:str(text)];
    }
    gen = msdossession_fujinet_generation(_session);
    if (gen != _fujinetGeneration) {
        _fujinetGeneration = gen;
        text[0] = '\0';
        if (msdossession_fujinet_last_event(_session, text, (int)sizeof text) > 0) [self toast:str(text)];
        [self updateStatus];
    }
    text[0] = '\0';
    if (msdossession_take_notice(_session, text, (int)sizeof text) > 0) [self toast:str(text)];
    [self updateLights];

    const uint64_t frames = [self pcRunning] ? [_display frames] : [_display ticks];
    if (_shotPath && frames >= _shotFrames) {
        BOOL ok;
        if ([self pcRunning] && [_display hasFrame]) ok = [_display savePng:_shotPath];
        else ok = [self saveWindowPng:_shotPath];
        if (!ok) NSLog(@"MSDOS_SCREENSHOT: could not write %@", _shotPath);
        _shotPath = nil;
    }
    if (_helpShotPath && frames >= _shotFrames) {
        if (![[MSDOSHelpWindow shared] savePng:_helpShotPath])
            NSLog(@"MSDOS_HELP_SCREENSHOT: could not write %@", _helpShotPath);
        _helpShotPath = nil;
    }
    if (_quitFrames && frames >= _quitFrames) {
        _quitFrames = 0;
        [NSApp terminate:nil];
    }
}

/* The window's content as it looks (the no-BIOS page). */
- (BOOL)saveWindowPng:(NSString *)path
{
    NSView *v = _content;
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    if (!rep) return NO;
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

/* ---- opening things -------------------------------------------------------------- */

- (void)rememberFolder:(NSString *)path
{
    msdossession_set_str(_session, "last_dir", [[path stringByDeletingLastPathComponent] fileSystemRepresentation]);
}

/* Where to look for media: the folder the last file came from. */
- (NSURL *)mediaFolder
{
    NSString *last = str(msdossession_get_str(_session, "last_dir", ""));
    BOOL dir = NO;
    if ([last length] && [[NSFileManager defaultManager] fileExistsAtPath:last isDirectory:&dir] && dir)
        return [NSURL fileURLWithPath:last];
    return [NSURL fileURLWithPath:str(msdossession_media_path(_session))];
}

- (void)attachHd:(NSString *)path drive:(int)drive
{
    char msg[1024];
    msg[0] = '\0';
    if (msdossession_hd_attach(_session, drive, [path fileSystemRepresentation], msg, (int)sizeof msg) != 0) {
        [self alert:@"Cannot Attach the Disk" text:str(msg)];
        return;
    }
    [self rememberFolder:path];
    [self toast:msg[0] ? str(msg) : [NSString stringWithFormat:@"%@ is %@ from the next reset",
                                      [path lastPathComponent], drive ? @"D:" : @"C:"]];
    [self updateLights];
}

/* A hard disk image: C: or D:? */
- (void)askHdDrive:(NSString *)path
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:@"Which Drive?"];
    [a setInformativeText:[NSString stringWithFormat:@"%@ is a hard disk image. It takes effect from the "
                                                     @"next reset.", [path lastPathComponent]]];
    [a addButtonWithTitle:@"C:"];
    [a addButtonWithTitle:@"D:"];
    [a addButtonWithTitle:@"Cancel"];
    const NSModalResponse r = [a runModal];
    if (r == NSAlertFirstButtonReturn) [self attachHd:path drive:0];
    else if (r == NSAlertSecondButtonReturn) [self attachHd:path drive:1];
}

/* Open, a drop, Finder, the command line: a floppy goes in A:, a hard disk
 * asks which drive, a BIOS is imported, anything else goes on FujiNet's SD
 * card. */
- (void)loadMedia:(NSString *)path
{
    if (!_session || !path.length) return;
    char msg[2048];
    msg[0] = '\0';
    const int rc = msdossession_open(_session, [path fileSystemRepresentation], msg, (int)sizeof msg);
    if (rc == 1) {
        [self askHdDrive:path];
        return;
    }
    if (rc != 0) {
        [self alert:@"Cannot Open It" text:msg[0] ? str(msg) : [self lastError]];
        return;
    }
    [self rememberFolder:path];
    [self toast:msg[0] ? str(msg) : [path lastPathComponent]];
    [self settingsChanged];
    [self startPC];
}

- (void)openMedia:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:@"Disk images and programs"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = [[panel URL] path];
        dispatch_async(dispatch_get_main_queue(), ^{ [self loadMedia:path]; });
    }];
}

/* ---- the BIOS ----------------------------------------------------------------------- */

- (void)importBiosURLs:(NSArray<NSURL *> *)urls
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSURL *u in urls) {
        char msg[2048];
        msg[0] = '\0';
        if (msdossession_rom_import(_session, [u fileSystemRepresentation], msg, (int)sizeof msg) == 0) imported++;
        if (msg[0]) [log addObject:str(msg)];
    }
    [self settingsChanged];
    [self startPC];
    if ([log count])
        [self alert:imported ? @"BIOS Imported" : @"Nothing Imported" text:[log componentsJoinedByString:@"\n"]];
}

- (void)importBiosFrom:(BOOL)folder
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (an 86Box ROM set works)"
                             : @"Import BIOS: images or zips"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:!folder];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSArray<NSURL *> *urls = [panel URLs];
        /* after the sheet: what came in is an alert of its own */
        dispatch_async(dispatch_get_main_queue(), ^{ [self importBiosURLs:urls]; });
    }];
}

- (void)importBios:(id)sender { (void)sender; [self importBiosFrom:NO]; }
- (void)importBiosFolder:(id)sender { (void)sender; [self importBiosFrom:YES]; }

/* ---- drives ------------------------------------------------------------------------- */

- (void)insertFloppy:(int)drive
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:drive ? @"Insert in B:" : @"Insert in A:"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = [[panel URL] path];
        dispatch_async(dispatch_get_main_queue(), ^{
            char msg[1024];
            msg[0] = '\0';
            if (msdossession_floppy_insert(self->_session, drive, [path fileSystemRepresentation], 0,
                                           msg, (int)sizeof msg) == 0) {
                [self rememberFolder:path];
                [self toast:msg[0] ? str(msg) : [path lastPathComponent]];
            } else {
                [self alert:@"Cannot Insert the Disk" text:str(msg)];
            }
            [self updateLights];
        });
    }];
}

- (void)blankFloppy:(int)drive
{
    NSSavePanel *panel = [NSSavePanel savePanel];
    [panel setMessage:@"New Blank Floppy"];
    [panel setNameFieldStringValue:@"blank.img"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = [[panel URL] path];
        dispatch_async(dispatch_get_main_queue(), ^{
            char msg[512], ins[512];
            msg[0] = ins[0] = '\0';
            const char *p = [path fileSystemRepresentation];
            if (msdossession_new_blank_floppy(self->_session, drive, p, msg, (int)sizeof msg) == 0 &&
                msdossession_floppy_insert(self->_session, drive, p, 0, ins, (int)sizeof ins) == 0)
                [self toast:ins[0] ? str(ins) : [path lastPathComponent]];
            else
                [self alert:@"Cannot Make the Disk" text:str(msg[0] ? msg : ins)];
            [self updateLights];
        });
    }];
}

- (void)chooseHd:(int)drive
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:drive ? @"Attach as D:" : @"Attach as C:"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = [[panel URL] path];
        dispatch_async(dispatch_get_main_queue(), ^{ [self attachHd:path drive:drive]; });
    }];
}

- (void)blankHd:(int)drive
{
    NSSavePanel *panel = [NSSavePanel savePanel];
    [panel setMessage:@"New Blank Hard Disk (the type in Preferences ▸ Storage)"];
    [panel setNameFieldStringValue:drive ? @"drive-d.vhd" : @"drive-c.vhd"];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSString *path = [[panel URL] path];
        dispatch_async(dispatch_get_main_queue(), ^{
            char msg[512];
            msg[0] = '\0';
            if (msdossession_new_blank_hd(self->_session, drive, [path fileSystemRepresentation], msg,
                                          (int)sizeof msg) == 0) {
                [self attachHd:path drive:drive];
                [self alert:@"New Hard Disk" text:str(msg)];
            } else {
                [self alert:@"Cannot Make the Disk" text:str(msg)];
            }
        });
    }];
}

- (void)driveAction:(NSMenuItem *)sender
{
    const int tag = (int)[sender tag];
    const int drive = tag / 16, action = tag % 16;
    switch (action) {
    case FD_INSERT:
        [self insertFloppy:drive];
        break;
    case FD_EJECT:
        msdossession_floppy_eject(_session, drive);
        break;
    case FD_PROTECT: {
        msdos_floppy_info fi;
        memset(&fi, 0, sizeof fi);
        msdossession_floppy_info(_session, drive, &fi);
        msdossession_floppy_set_read_only(_session, drive, !fi.read_only);
        break;
    }
    case FD_BOOT:
        msdossession_floppy_boot_disk(_session, drive);
        [self toast:@"The FujiNet boot floppy is in the drive"];
        break;
    case FD_BLANK:
        [self blankFloppy:drive];
        break;
    case HD_ATTACH:
        [self chooseHd:drive];
        break;
    case HD_DETACH:
        msdossession_hd_detach(_session, drive);
        [self toast:@"Detached from the next reset"];
        break;
    case HD_BLANK:
        [self blankHd:drive];
        break;
    default:
        break;
    }
    [self updateLights];
}

- (void)importToSD:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:@"Import to FujiNet's SD Card"];
    [panel setAllowsMultipleSelection:YES];
    [panel setDirectoryURL:[self mediaFolder]];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSArray<NSURL *> *urls = [panel URLs];
        dispatch_async(dispatch_get_main_queue(), ^{
            int n = 0;
            for (NSURL *u in urls) {
                char dest[1024];
                dest[0] = '\0';
                if (msdossession_import_to_sd(self->_session, [u fileSystemRepresentation], dest,
                                              (int)sizeof dest) == 0)
                    n++;
            }
            if (n)
                [self toast:[NSString stringWithFormat:@"%d file%@ on FujiNet's SD card: mount %@ in CONFIG", n,
                             n == 1 ? @" is" : @"s are", n == 1 ? @"it" : @"them"]];
            else
                [self alert:@"Nothing Imported" text:@"The files could not be copied."];
        });
    }];
}

- (void)openSDFolder:(id)sender
{
    (void)sender;
    NSString *path = str(msdossession_sd_path(_session));
    [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

/* ---- the machine ------------------------------------------------------------------- */

- (void)ctrlAltDel:(id)sender
{
    (void)sender;
    msdossession_ctrl_alt_del(_session);
}

- (void)resetPC:(id)sender
{
    (void)sender;
    if (![self pcRunning]) {
        [self startPC];
        return;
    }
    msdossession_reset(_session);
    [self updateStatus];
}

- (void)togglePause:(id)sender
{
    (void)sender;
    msdossession_set_paused(_session, !msdossession_paused(_session));
    [self updateStatus];
}

- (void)toggleWarp:(id)sender
{
    (void)sender;
    msdossession_set_warp(_session, !msdossession_warp(_session));
    [self updateStatus];
}

/* ---- text in and out ---------------------------------------------------------------- */

- (void)copyScreenText:(id)sender
{
    (void)sender;
    static char buf[16384];
    if (![self pcRunning] || msdossession_copy_screen_text(_session, buf, (int)sizeof buf) <= 0) {
        [self toast:@"No text screen to copy"];
        return;
    }
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str(buf) forType:NSPasteboardTypeString];
    [self toast:@"The screen's text is on the clipboard"];
}

- (void)pasteToPC:(id)sender
{
    (void)sender;
    NSString *t = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if ([t length] && [self pcRunning]) msdossession_paste(_session, [t UTF8String]);
}

/* ---- the view ------------------------------------------------------------------------- */

- (void)applyView
{
    if (!_session) return;
    [_display setSmooth:msdossession_smooth(_session) != 0];
    [_display setIntegerScale:msdossession_integer_scale(_session) != 0];
    [_display refreshFrame];    /* a 4:3 change shows at once */
}

- (BOOL)prefOn:(const char *)key
{
    const int i = msdossession_pref_find(key);
    return i >= 0 && msdossession_pref_get_int(_session, i) != 0;
}

- (void)togglePref:(const char *)key
{
    const int i = msdossession_pref_find(key);
    if (i >= 0) msdossession_pref_set_int(_session, i, !msdossession_pref_get_int(_session, i));
    msdossession_settings_flush(_session);
    [self applyView];
}

- (void)toggleAspect:(id)sender { (void)sender; [self togglePref:"aspect43"]; }
- (void)toggleInteger:(id)sender { (void)sender; [self togglePref:"integer_scale"]; }
- (void)toggleSmooth:(id)sender { (void)sender; [self togglePref:"smooth"]; }

- (void)toggleFullScreen:(id)sender
{
    [_window toggleFullScreen:sender];
}

- (void)toggleControllers:(id)sender
{
    (void)sender;
    [_controllers toggle];
}

- (void)toggleDebugger:(id)sender
{
    (void)sender;
    [_debugger toggle];
}

/* After Preferences or an import: the view, the Controllers panel, the
 * status and the page (picture or no-BIOS) follow what changed. */
- (void)settingsChanged
{
    [self applyView];
    [_controllers refresh];
    [self updateStatus];
    [self updateLights];
    [self showPage];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (!_settings) {
        __weak MSDOSAppDelegate *weakSelf = self;
        _settings = [[MSDOSSettingsWindow alloc] initWithSession:_session
            onLive:^{ [weakSelf applyView]; }
            onMachine:^{
                MSDOSAppDelegate *me = weakSelf;
                if (me) [me->_controllers refresh];
            }
            onImport:^(NSString *message) {
                MSDOSAppDelegate *me = weakSelf;
                if (!me) return;
                if (message.length) [me toast:message];
                [me settingsChanged];
            }
            onClosed:^(int applied) {
                MSDOSAppDelegate *me = weakSelf;
                if (!me) return;
                [me settingsChanged];
                if (applied == MSDOS_APPLY_RESET) [me toast:@"Reset with the new configuration"];
                else if (applied == MSDOS_APPLY_RESTART) [me toast:@"FujiNet restarted"];
                [me startPC];   /* a BIOS imported here: the PC comes up */
            }];
    }
    [_settings show];
}

/* ---- FujiNet ---------------------------------------------------------------------------- */

/* The web UI in the system browser: its Google and OneDrive Authorize
 * buttons open the provider's consent page, which refuses an embedded web
 * view. */
- (void)openWebUI:(id)sender
{
    (void)sender;
    if (msdossession_fujinet_external(_session)) {
        [self toast:@"FujiNet is external: use its own web UI"];
        return;
    }
    if (!msdossession_fujinet_running(_session)) {
        [self toast:@"FujiNet is off (Preferences ▸ FujiNet)"];
        return;
    }
    NSURL *url = [NSURL URLWithString:str(msdossession_fujinet_webui_url(_session))];
    if (url) [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)restartFujiNet:(id)sender
{
    (void)sender;
    if (msdossession_fujinet_restart(_session) != 0)
        [self alert:@"Cannot Restart FujiNet" text:[self lastError]];
    else
        [self toast:@"FujiNet restarted"];
}

/* Follows the tail only while the reader is already at the bottom. */
- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    if (!_session || !_logView) return;
    const int n = msdossession_fujinet_copy_log(_session, buf, (int)sizeof buf);
    if (n >= 0 && n < (int)sizeof buf) buf[n] = '\0';
    else buf[sizeof buf - 1] = '\0';
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf] : nil;
    if (n > 0 && !text) text = [NSString stringWithCString:buf encoding:NSISOLatin1StringEncoding];
    NSString *shown = text ?: @"(no FujiNet output yet)";
    if (![_logView.string isEqualToString:shown]) {
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        [_logView setString:shown];
        if (scroll && !atEnd) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (msdossession_fujinet_external(_session)) {
        [self toast:@"FujiNet is external: see its own console"];
        return;
    }
    if (!_logWindow) {
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 820, 560)];
        scroll.hasVerticalScroller = YES;
        scroll.autohidesScrollers = NO;
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
        _logView.editable = NO;
        _logView.richText = NO;
        _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _logView.autoresizingMask = NSViewWidthSizable;
        scroll.documentView = _logView;
        _logWindow = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 820, 560)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _logWindow.title = @"FujiNet Console Log";
        _logWindow.releasedWhenClosed = NO;
        _logWindow.tabbingMode = NSWindowTabbingModeDisallowed;
        _logWindow.delegate = self;
        {
            /* the log over a strip with the Help button */
            NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 820, 560)];
            NSButton *help = MSDOSHelpButton(@"fujilog");
            scroll.frame = NSMakeRect(0, 36, 820, 524);
            [help setFrameOrigin:NSMakePoint(10, 6)];
            help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
            [box addSubview:scroll];
            [box addSubview:help];
            _logWindow.contentView = box;
        }
        MSDOSSetHelpContext(_logWindow, @"fujilog");
        [_logWindow center];
    }
    if (!_logTimer)
        _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                   selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

/* For the window in front: Preferences' page, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[MSDOSHelpWindow shared] showFor:[NSApp keyWindow]];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSString *credits =
        @"IBM PCs and compatibles, from the 5150 to a Pentium, with a FujiNet on a COM port. "
        @"The emulator is 86Box (GPL-2.0-or-later, the 86Box team), with the FujiNet firmware "
        @"(GPL-3.0-or-later) talking to FUJINET.SYS. The boot floppy is FreeDOS (GPL). The BIOS "
        @"ROMs are imported.\n\nThomas Cherryhomes — GPL-3.0-or-later\n";
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:[NSFont smallSystemFontSize]],
                             NSForegroundColorAttributeName: [NSColor labelColor],
                             NSParagraphStyleAttributeName: para };
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] initWithString:credits attributes:attrs];
    NSDictionary *link = @{ NSFontAttributeName: [NSFont systemFontOfSize:[NSFont smallSystemFontSize]],
                            NSLinkAttributeName: [NSURL URLWithString:@"https://fujinet.online/"],
                            NSParagraphStyleAttributeName: para };
    [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"https://fujinet.online/"
                                                                 attributes:link]];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationName: APP_TITLE,
        NSAboutPanelOptionApplicationVersion: str(MSDOS_VERSION_STRING),
        NSAboutPanelOptionVersion: @"",
        NSAboutPanelOptionCredits: text,
    }];
}

/* ---- the menu bar ------------------------------------------------------------------------- */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

/* Command+Shift: GNOME's Ctrl+Shift shortcuts, the Mac way. */
- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel shiftKey:(NSString *)key
{
    NSMenuItem *it = [self item:menu title:title action:sel key:key];
    [it setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    return it;
}

- (NSMenu *)addMenu:(NSString *)title to:(NSMenu *)bar
{
    NSMenuItem *top = [[NSMenuItem alloc] init];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    [top setSubmenu:m];
    [bar addItem:top];
    return m;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenu *appMenu = [self addMenu:APP_TITLE to:bar];
    [self item:appMenu title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Preferences…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide " APP_TITLE action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                            action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_TITLE action:@selector(terminate:) keyEquivalent:@"q"];

    NSMenu *machine = [self addMenu:@"Machine" to:bar];
    [self item:machine title:@"Open…" action:@selector(openMedia:) shiftKey:@"o"];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Ctrl+Alt+Del" action:@selector(ctrlAltDel:) key:@""];
    [self item:machine title:@"Reset" action:@selector(resetPC:) shiftKey:@"r"];
    [self item:machine title:@"Pause" action:@selector(togglePause:) key:@""];
    [self item:machine title:@"Warp Speed" action:@selector(toggleWarp:) shiftKey:@"w"];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Import BIOS…" action:@selector(importBios:) key:@""];
    [self item:machine title:@"Import BIOS from a Folder…" action:@selector(importBiosFolder:) key:@""];

    _drivesMenu = [self addMenu:@"Drives" to:bar];
    _drivesMenu.delegate = self;
    [self menuNeedsUpdate:_drivesMenu];

    /* The PC's clipboard items, then the standard ones every text field in
     * the app's other windows needs (in the main window they are the PC's
     * too: see MSDOSContentView). */
    NSMenu *edit = [self addMenu:@"Edit" to:bar];
    [self item:edit title:@"Copy Screen Text" action:@selector(copyScreenText:) shiftKey:@"c"];
    [self item:edit title:@"Paste to the PC" action:@selector(pasteToPC:) shiftKey:@"v"];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];

    NSMenu *view = [self addMenu:@"View" to:bar];
    [self item:view title:@"Controllers" action:@selector(toggleControllers:) shiftKey:@"k"];
    [self item:view title:@"Debugger" action:@selector(toggleDebugger:) shiftKey:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    [self item:view title:@"4:3 Picture" action:@selector(toggleAspect:) key:@""];
    [self item:view title:@"Whole-Number Scaling" action:@selector(toggleInteger:) key:@""];
    [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [view addItem:[NSMenuItem separatorItem]];
    [self item:view title:@"Full Screen (⌃⌥Page Down)" action:@selector(toggleFullScreen:) shiftKey:@"f"];

    NSMenu *fuji = [self addMenu:@"FujiNet" to:bar];
    [self item:fuji title:@"FujiNet Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [self item:fuji title:@"Restart FujiNet" action:@selector(restartFujiNet:) key:@""];

    /* Window and Help: AppKit fills these in once registered as such. */
    NSMenu *windows = [self addMenu:@"Window" to:bar];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windows addItem:[NSMenuItem separatorItem]];
    [windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];

    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    NSMenu *help = [self addMenu:@"Help" to:bar];
    [self item:help title:APP_TITLE @" Help" action:@selector(openHelp:) key:@"?"];
    [help addItem:[NSMenuItem separatorItem]];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[MSDOSHelpWindow shared]];
}

/* The Drives menu, rebuilt as it opens: the drives this machine has. */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    if (!_session || menu != _drivesMenu) return;
    [menu removeAllItems];
    /* a heading (no action: shown, never chosen) */
    [menu addItemWithTitle:@"The PC's drives" action:nil keyEquivalent:@""];
    for (int d = 0; d < MSDOS_FLOPPIES; d++) {
        msdos_floppy_info fi;
        memset(&fi, 0, sizeof fi);
        msdossession_floppy_info(_session, d, &fi);
        if (!fi.present) continue;
        NSString *name = fi.boot_disk ? @"FujiNet boot floppy"
                       : fi.path[0] ? [str(fi.path) lastPathComponent] : @"Empty";
        NSMenu *sub = [[NSMenu alloc] initWithTitle:d ? @"B:" : @"A:"];
        [sub addItemWithTitle:fi.read_only ? [name stringByAppendingString:@" (write-protected)"] : name
                       action:nil keyEquivalent:@""];
        [sub addItem:[NSMenuItem separatorItem]];
        [[self item:sub title:@"Insert…" action:@selector(driveAction:) key:@""] setTag:DRIVE_TAG(d, FD_INSERT)];
        [[self item:sub title:@"Eject" action:@selector(driveAction:) key:@""] setTag:DRIVE_TAG(d, FD_EJECT)];
        [[self item:sub title:fi.read_only ? @"Allow Writing" : @"Write-Protect" action:@selector(driveAction:)
                key:@""] setTag:DRIVE_TAG(d, FD_PROTECT)];
        [[self item:sub title:@"FujiNet Boot Floppy" action:@selector(driveAction:) key:@""]
            setTag:DRIVE_TAG(d, FD_BOOT)];
        [[self item:sub title:@"New Blank Disk…" action:@selector(driveAction:) key:@""]
            setTag:DRIVE_TAG(d, FD_BLANK)];
        NSString *title = [NSString stringWithFormat:@"%@ %@", d ? @"B:" : @"A:", str(fi.type)];
        [[menu addItemWithTitle:title action:nil keyEquivalent:@""] setSubmenu:sub];
    }
    for (int d = 0; d < MSDOS_HARD_DISKS; d++) {
        msdos_hd_info hi;
        memset(&hi, 0, sizeof hi);
        msdossession_hd_info(_session, d, &hi);
        if (!hi.present) continue;
        NSMenu *sub = [[NSMenu alloc] initWithTitle:d ? @"D:" : @"C:"];
        [sub addItemWithTitle:hi.path[0] ? [str(hi.path) lastPathComponent] : @"None" action:nil keyEquivalent:@""];
        [sub addItem:[NSMenuItem separatorItem]];
        [[self item:sub title:@"Attach…" action:@selector(driveAction:) key:@""] setTag:DRIVE_TAG(d, HD_ATTACH)];
        [[self item:sub title:@"Detach" action:@selector(driveAction:) key:@""] setTag:DRIVE_TAG(d, HD_DETACH)];
        [[self item:sub title:@"New Blank Disk…" action:@selector(driveAction:) key:@""]
            setTag:DRIVE_TAG(d, HD_BLANK)];
        [[menu addItemWithTitle:d ? @"D: hard disk" : @"C: hard disk" action:nil keyEquivalent:@""] setSubmenu:sub];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"FujiNet's SD card" action:nil keyEquivalent:@""];
    [self item:menu title:@"Import to SD Card…" action:@selector(importToSD:) key:@""];
    [self item:menu title:@"Open SD Folder" action:@selector(openSDFolder:) key:@""];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    const BOOL running = [self pcRunning];
    if (a == @selector(togglePause:)) {
        [item setState:(running && msdossession_paused(_session)) ? NSControlStateValueOn : NSControlStateValueOff];
        return running;
    }
    if (a == @selector(toggleWarp:)) {
        [item setState:(running && msdossession_warp(_session)) ? NSControlStateValueOn : NSControlStateValueOff];
        return running;
    }
    if (a == @selector(toggleAspect:)) {
        [item setState:(_session && [self prefOn:"aspect43"]) ? NSControlStateValueOn : NSControlStateValueOff];
        return _session != NULL;
    }
    if (a == @selector(toggleInteger:)) {
        [item setState:(_session && msdossession_integer_scale(_session)) ? NSControlStateValueOn
                                                                          : NSControlStateValueOff];
        return _session != NULL;
    }
    if (a == @selector(toggleSmooth:)) {
        [item setState:(_session && msdossession_smooth(_session)) ? NSControlStateValueOn : NSControlStateValueOff];
        return _session != NULL;
    }
    if (a == @selector(toggleFullScreen:)) {
        [item setState:([_window styleMask] & NSWindowStyleMaskFullScreen) ? NSControlStateValueOn
                                                                           : NSControlStateValueOff];
        return YES;
    }
    if (a == @selector(openWebUI:))
        return _session && msdossession_fujinet_running(_session);
    if (a == @selector(restartFujiNet:))
        return running && msdossession_get_int(_session, "fujinet", 1) != 0 &&
               !msdossession_fujinet_external(_session);
    if (a == @selector(ctrlAltDel:) || a == @selector(copyScreenText:) || a == @selector(pasteToPC:))
        return running;
    return _session != NULL;
}

/* ---- the toolbar ----------------------------------------------------------------------------- */

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return @[kCadItem, kResetItem, kStatusItem];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    (void)toolbar;
    return @[kCadItem, kResetItem, kStatusItem];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSToolbarItemIdentifier)ident
 willBeInsertedIntoToolbar:(BOOL)flag
{
    (void)toolbar;
    (void)flag;
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    if ([ident isEqualToString:kCadItem]) {
        item.label = @"Ctrl+Alt+Del";
        item.paletteLabel = @"Ctrl+Alt+Del";
        item.toolTip = _cadButton.toolTip;
        item.view = _cadButton;
    } else if ([ident isEqualToString:kResetItem]) {
        item.label = @"Reset";
        item.paletteLabel = @"Reset";
        item.toolTip = _resetButton.toolTip;
        item.view = _resetButton;
    } else if ([ident isEqualToString:kStatusItem]) {
        item.label = @"Status";
        item.paletteLabel = @"Status";
        item.view = _statusView;
    } else {
        return nil;
    }
    return item;
}
@end
