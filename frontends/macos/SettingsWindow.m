/*
 * MSDOSSettingsWindow -- see the header.
 *
 * A tab per page of msdossession_pref, a heading per group, a row per
 * preference, and a ROMs tab listing what each machine and card needs,
 * imported or not, so the four frontends show the same settings. Rows that
 * apply LIVE (the picture, the volume) take effect at once. The machine --
 * model, processor, memory, cards, drives, ports -- is the computer you
 * would switch off to change: those rows wait, and closing the window
 * applies them together with one reset. Rows follow each other as they
 * change: processors and memory by machine, cards by what the machine can
 * take and what has been imported.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"

#import "help/HelpWindow.h"

#include <string.h>

#define PAGE_W 640.0
#define PAGE_H 560.0
#define ROW_H 30.0
#define HELP_H 16.0
#define HEAD_H 30.0
#define LABEL_W 230.0
#define CTRL_X 250.0

/* A page's document view: rows from the top. */
@interface MSDOSFlippedView : NSView
@end
@implementation MSDOSFlippedView
- (BOOL)isFlipped { return YES; }
@end

static NSString *str(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

@implementation MSDOSSettingsWindow {
    msdossession *_session;
    void (^_onLive)(void);
    void (^_onMachine)(void);
    void (^_onImport)(NSString *);
    void (^_onClosed)(int);
    NSWindow *_window;
    NSMutableDictionary<NSNumber *, NSView *> *_controls;  /* by pref */
    NSMutableDictionary<NSNumber *, NSStepper *> *_steppers;
    NSMutableDictionary<NSNumber *, NSTextField *> *_labels;
    NSMutableDictionary<NSNumber *, NSString *> *_choiceLabels;
    NSMutableArray<NSTextField *> *_romStates;
    BOOL _syncing;
    int _pending;               /* the heaviest change not applied yet */
}

- (instancetype)initWithSession:(msdossession *)session
                         onLive:(void (^)(void))onLive
                      onMachine:(void (^)(void))onMachine
                       onImport:(void (^)(NSString *message))onImport
                       onClosed:(void (^)(int applied))onClosed
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _onLive = onLive;
    _onMachine = onMachine;
    _onImport = onImport;
    _onClosed = onClosed;
    return self;
}

- (NSTextField *)label:(NSString *)text frame:(NSRect)r bold:(BOOL)bold
{
    NSTextField *l = [NSTextField labelWithString:text];
    [l setFrame:r];
    if (bold) [l setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
    [l setLineBreakMode:NSLineBreakByTruncatingTail];
    return l;
}

- (NSTextField *)helpLabel:(NSString *)text frame:(NSRect)r
{
    NSTextField *l = [self label:text frame:r bold:NO];
    [l setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [l setTextColor:[NSColor secondaryLabelColor]];
    [l setToolTip:text];
    return l;
}

/* A change to pref i: LIVE ones apply now unless something heavier waits
 * (applying then would reset mid-edit). */
- (void)changed:(int)i
{
    const msdos_pref *p = msdossession_pref(i);
    if (!p) return;
    if (p->applies > _pending) _pending = p->applies;
    if (_pending == MSDOS_APPLY_LIVE) msdossession_apply_settings(_session);
    if (p->applies == MSDOS_APPLY_LIVE && _onLive) _onLive();
    if (p->key && !strcmp(p->key, "machine") && _onMachine) _onMachine();
    [self refresh];
}

/* ---- the rows' actions ---------------------------------------------------- */

- (void)boolChanged:(NSButton *)b
{
    if (_syncing) return;
    const int i = (int)[b tag];
    msdossession_pref_set_int(_session, i, [b state] == NSControlStateValueOn);
    [self changed:i];
}

- (void)choiceChanged:(NSPopUpButton *)b
{
    if (_syncing) return;
    const int i = (int)[b tag];
    const int c = (int)[b indexOfSelectedItem];
    if (c < 0) return;
    if (!msdossession_pref_choice_enabled(_session, i, c)) {
        /* not imported, or not on this machine: put it back */
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Not Available"];
        [a setInformativeText:@"Import its ROM first (the ROMs tab), or choose a machine that takes it."];
        [a beginSheetModalForWindow:_window completionHandler:nil];
        [self refresh];
        return;
    }
    msdossession_pref_set_choice(_session, i, c);
    [self changed:i];
}

/* The field (Return, or leaving it) and its stepper. */
- (void)intChanged:(id)sender
{
    if (_syncing) return;
    const int i = (int)[sender tag];
    int lo = 0, hi = 0, step = 1;
    msdossession_pref_int_range(_session, i, &lo, &hi, &step);
    int v = [sender intValue];
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v == msdossession_pref_get_int(_session, i)) {
        [self refresh];
        return;
    }
    msdossession_pref_set_int(_session, i, v);
    [self changed:i];
}

/* ---- the ROMs page's import ------------------------------------------------- */

- (void)importRoms:(NSButton *)b
{
    const BOOL folder = [b tag] != 0;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (an 86Box ROM set works)"
                             : @"Import BIOS: an image or a zip"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:NO];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || ![panel URL]) return;
        char msg[2048];
        msg[0] = '\0';
        msdossession_rom_import(self->_session, [[panel URL] fileSystemRepresentation], msg, (int)sizeof msg);
        if (self->_onImport) self->_onImport(str(msg));
        [self refresh];
    }];
}

/* ---- everything from the store ---------------------------------------------- */

/* A choice row's labels joined, to see when they change. */
- (NSString *)choiceLabels:(int)i
{
    NSMutableString *all = [NSMutableString string];
    const char *l;
    for (int c = 0; (l = msdossession_pref_choice(_session, i, c)) != NULL; c++) {
        [all appendString:str(l)];
        if (!msdossession_pref_choice_enabled(_session, i, c)) [all appendString:@" (not available)"];
        [all appendString:@"\n"];
    }
    return all;
}

- (void)refresh
{
    if (!_window) return;
    _syncing = YES;
    for (int i = 0; i < msdossession_pref_count(); i++) {
        const msdos_pref *p = msdossession_pref(i);
        NSNumber *key = @(i);
        NSView *ctrl = _controls[key];
        if (!ctrl || !p) continue;
        const BOOL on = msdossession_pref_enabled(_session, i) != 0;
        if ([ctrl isKindOfClass:[NSControl class]]) [(NSControl *)ctrl setEnabled:on];
        [_labels[key] setTextColor:on ? [NSColor labelColor] : [NSColor disabledControlTextColor]];
        switch (p->type) {
        case MSDOS_PREF_BOOL:
            [(NSButton *)ctrl setState:msdossession_pref_get_int(_session, i)
                                           ? NSControlStateValueOn : NSControlStateValueOff];
            break;
        case MSDOS_PREF_INT: {
            int lo = 0, hi = 0, step = 1;
            msdossession_pref_int_range(_session, i, &lo, &hi, &step);
            const int v = msdossession_pref_get_int(_session, i);
            [(NSTextField *)ctrl setIntValue:v];
            NSStepper *st = _steppers[key];
            [st setMinValue:lo];
            [st setMaxValue:hi];
            [st setIncrement:step > 0 ? step : 1];
            [st setIntValue:v];
            [st setEnabled:on];
            break;
        }
        case MSDOS_PREF_CHOICE: {
            NSPopUpButton *pop = (NSPopUpButton *)ctrl;
            NSString *labels = [self choiceLabels:i];
            if (![labels isEqualToString:_choiceLabels[key] ? _choiceLabels[key] : @""]) {
                [pop removeAllItems];
                const char *l;
                for (int c = 0; (l = msdossession_pref_choice(_session, i, c)) != NULL; c++) {
                    const BOOL can = msdossession_pref_choice_enabled(_session, i, c) != 0;
                    NSString *title = can ? str(l) : [str(l) stringByAppendingString:@" (not available)"];
                    /* through the menu: the button's own addItemWithTitle:
                     * drops an earlier item of the same title */
                    NSMenuItem *it = [[pop menu] addItemWithTitle:title action:nil keyEquivalent:@""];
                    [it setEnabled:can];
                }
                _choiceLabels[key] = labels;
            }
            const int sel = msdossession_pref_get_choice(_session, i);
            [pop selectItemAtIndex:sel >= 0 && sel < [pop numberOfItems] ? sel : 0];
            break;
        }
        default:
            break;
        }
    }
    for (NSUInteger r = 0; r < [_romStates count]; r++)
        [_romStates[r] setStringValue:msdossession_rom_item_available(_session, (int)r) ? @"Imported"
                                                                                        : @"Not imported"];
    _syncing = NO;
}

/* ---- building it ------------------------------------------------------------------ */

- (NSScrollView *)scrollFor:(NSView *)doc
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, PAGE_H)];
    [scroll setHasVerticalScroller:YES];
    [scroll setDrawsBackground:NO];
    [scroll setDocumentView:doc];
    return scroll;
}

/* A page's rows, as a scrolling column. */
- (NSScrollView *)pageForRows:(NSArray<NSNumber *> *)rows note:(NSString *)note
{
    MSDOSFlippedView *doc = [[MSDOSFlippedView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 100)];
    CGFloat y = 12;
    const char *lastGroup = NULL;
    if (note) {
        NSTextField *n = [NSTextField wrappingLabelWithString:note];
        [n setFrame:NSMakeRect(16, y, PAGE_W - 32, 34)];
        [n setTextColor:[NSColor secondaryLabelColor]];
        [doc addSubview:n];
        y += 40;
    }
    for (NSNumber *num in rows) {
        const int i = [num intValue];
        const msdos_pref *p = msdossession_pref(i);
        if (!p) continue;
        if (!lastGroup || strcmp(lastGroup, p->group)) {
            [doc addSubview:[self label:str(p->group) frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
            y += HEAD_H;
            lastGroup = p->group;
        }
        NSTextField *label = [self label:str(p->label) frame:NSMakeRect(28, y + 5, LABEL_W - 12, 20) bold:NO];
        if (p->help) [label setToolTip:str(p->help)];
        [doc addSubview:label];
        _labels[num] = label;
        NSView *ctrl = nil;
        CGFloat rowH = ROW_H;
        switch (p->type) {
        case MSDOS_PREF_BOOL: {
            /* the help, if any, is the box's own title */
            NSButton *b = [NSButton checkboxWithTitle:p->help ? str(p->help) : @""
                                               target:self action:@selector(boolChanged:)];
            [b setFrame:NSMakeRect(CTRL_X, y + 3, PAGE_W - CTRL_X - 16, 22)];
            ctrl = b;
            break;
        }
        case MSDOS_PREF_INT: {
            NSTextField *f = [NSTextField textFieldWithString:@""];
            [f setFrame:NSMakeRect(CTRL_X, y + 3, 80, 22)];
            [f setTarget:self];
            [f setAction:@selector(intChanged:)];
            [[f cell] setSendsActionOnEndEditing:YES];
            NSStepper *st = [[NSStepper alloc] initWithFrame:NSMakeRect(CTRL_X + 84, y + 2, 19, 24)];
            [st setTag:i];
            [st setTarget:self];
            [st setAction:@selector(intChanged:)];
            [st setValueWraps:NO];
            [doc addSubview:st];
            _steppers[num] = st;
            if (p->help) {
                [doc addSubview:[self helpLabel:str(p->help)
                                          frame:NSMakeRect(CTRL_X, y + ROW_H - 2, PAGE_W - CTRL_X - 16, HELP_H)]];
                rowH += HELP_H;
            }
            ctrl = f;
            break;
        }
        case MSDOS_PREF_CHOICE: {
            NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(CTRL_X, y + 1, PAGE_W - CTRL_X - 16, 26)
                                                            pullsDown:NO];
            /* unavailable choices stay greyed (refresh sets them) */
            [pop setAutoenablesItems:NO];
            [pop setTarget:self];
            [pop setAction:@selector(choiceChanged:)];
            if (p->help) {
                [doc addSubview:[self helpLabel:str(p->help)
                                          frame:NSMakeRect(CTRL_X + 3, y + ROW_H - 2, PAGE_W - CTRL_X - 19, HELP_H)]];
                rowH += HELP_H;
            }
            ctrl = pop;
            break;
        }
        default: {
            /* MSDOS_PREF_FILE: none in the table yet; shown, as GNOME does */
            NSTextField *none = [self label:@"" frame:NSMakeRect(CTRL_X, y + 5, PAGE_W - CTRL_X - 16, 20) bold:NO];
            ctrl = none;
            break;
        }
        }
        if ([ctrl isKindOfClass:[NSControl class]]) [(NSControl *)ctrl setTag:i];
        [doc addSubview:ctrl];
        _controls[num] = ctrl;
        y += rowH;
    }
    [doc setFrame:NSMakeRect(0, 0, PAGE_W, y + 12)];
    return [self scrollFor:doc];
}

- (NSScrollView *)romsPage
{
    MSDOSFlippedView *doc = [[MSDOSFlippedView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W, 100)];
    CGFloat y = 12;
    const char *lastGroup = NULL;
    NSTextField *n = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:
        @"The machines' BIOS chips and the cards' ROMs are not part of the app. Import them from "
        @"files, a zip or a folder (an 86Box ROM set works); they are recognised by their "
        @"checksums. Kept in %@.", str(msdossession_roms_path(_session))]];
    [n setFrame:NSMakeRect(16, y, PAGE_W - 32, 52)];
    [n setTextColor:[NSColor secondaryLabelColor]];
    [doc addSubview:n];
    y += 58;
    NSButton *files = [NSButton buttonWithTitle:@"Import…" target:self action:@selector(importRoms:)];
    [files setFrame:NSMakeRect(16, y, 110, 28)];
    [files setTag:0];
    [doc addSubview:files];
    NSButton *dir = [NSButton buttonWithTitle:@"Import a Folder…" target:self action:@selector(importRoms:)];
    [dir setFrame:NSMakeRect(132, y, 150, 28)];
    [dir setTag:1];
    [doc addSubview:dir];
    y += 36;
    _romStates = [NSMutableArray array];
    for (int r = 0; r < msdossession_rom_item_count(); r++) {
        const msdos_rom_item *item = msdossession_rom_item(r);
        if (!item) continue;
        if (!lastGroup || strcmp(lastGroup, item->group)) {
            [doc addSubview:[self label:str(item->group) frame:NSMakeRect(16, y + 6, PAGE_W - 32, 20) bold:YES]];
            y += HEAD_H;
            lastGroup = item->group;
        }
        NSTextField *name = [self label:str(item->name) frame:NSMakeRect(28, y + 5, PAGE_W - 180, 20) bold:NO];
        [name setToolTip:str(item->name)];
        [doc addSubview:name];
        NSTextField *state = [self label:@"" frame:NSMakeRect(PAGE_W - 136, y + 5, 120, 20) bold:NO];
        [state setTextColor:[NSColor secondaryLabelColor]];
        [state setAlignment:NSTextAlignmentRight];
        [doc addSubview:state];
        [_romStates addObject:state];
        y += ROW_H - 4;
    }
    [doc setFrame:NSMakeRect(0, 0, PAGE_W, y + 12)];
    return [self scrollFor:doc];
}

- (void)show
{
    if (_window) {
        [self refresh];
        [_window makeKeyAndOrderFront:nil];
        return;
    }
    _controls = [NSMutableDictionary dictionary];
    _steppers = [NSMutableDictionary dictionary];
    _labels = [NSMutableDictionary dictionary];
    _choiceLabels = [NSMutableDictionary dictionary];
    _romStates = [NSMutableArray array];
    _pending = MSDOS_APPLY_LIVE;

    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W + 20, PAGE_H + 40)];
    NSMutableArray<NSNumber *> *rows = [NSMutableArray array];
    const char *page = NULL;
    for (int i = 0; i <= msdossession_pref_count(); i++) {
        const msdos_pref *p = i < msdossession_pref_count() ? msdossession_pref(i) : NULL;
        if (page && (!p || strcmp(page, p->page))) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:str(page)];
            [item setLabel:str(page)];
            [item setView:[self pageForRows:rows note:!strcmp(page, "Machine")
                ? @"The machine's settings apply together when Preferences closes: the PC resets once."
                : nil]];
            [tabs addTabViewItem:item];
            [rows removeAllObjects];
        }
        if (!p) break;
        page = p->page;
        [rows addObject:@(i)];
    }
    /* the ROMs last, as in the other frontends */
    NSTabViewItem *roms = [[NSTabViewItem alloc] initWithIdentifier:@"ROMs"];
    [roms setLabel:@"ROMs"];
    [roms setView:[self romsPage]];
    [tabs addTabViewItem:roms];

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, PAGE_W + 20, PAGE_H + 40)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setTitle:@"Preferences"];
    [_window setReleasedWhenClosed:NO];
    [_window setTabbingMode:NSWindowTabbingModeDisallowed];
    [_window setDelegate:self];
    {
        /* the pages over a strip with the Help button, for the page shown */
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, PAGE_W + 20, PAGE_H + 40 + 36)];
        NSButton *help = MSDOSHelpButton(nil);
        tabs.frame = NSMakeRect(0, 36, PAGE_W + 20, PAGE_H + 40);
        [help setFrameOrigin:NSMakePoint(12, 6)];
        [box addSubview:tabs];
        [box addSubview:help];
        [_window setContentSize:box.frame.size];
        [_window setContentView:box];
        tabs.delegate = self;
        MSDOSSetHelpContext(_window, [@"prefs/" stringByAppendingString:(NSString *)tabs.selectedTabViewItem.identifier]);
    }
    [_window center];
    [self refresh];
    [_window makeKeyAndOrderFront:nil];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    MSDOSSetHelpContext(_window, [@"prefs/" stringByAppendingString:(NSString *)item.identifier]);
}

- (void)windowWillClose:(NSNotification *)note
{
    NSWindow *closing = note.object;
    if (closing != _window) return;
    /* a value still being typed counts */
    [_window makeFirstResponder:nil];
    msdossession_settings_flush(_session);
    const int did = _pending > MSDOS_APPLY_LIVE ? msdossession_apply_settings(_session) : 0;
    _pending = MSDOS_APPLY_LIVE;
    /* built afresh next time (the rows follow the machine); the window
     * itself lives until AppKit is done closing it */
    _window = nil;
    dispatch_async(dispatch_get_main_queue(), ^{ [closing setDelegate:nil]; });
    if (_onClosed) _onClosed(did);
}
@end
