/*
 * Debugger window (AppKit) over the session's debugger engine, via
 * core/include/msdosdebug.h. Mirrors the GNOME one (dbg_window.c) tab for
 * tab.
 *
 * Tabs: the Console (DEBUG.COM's commands a line at a time, with a command
 * history), CPU & Memory (registers, flags, a hex dump), the Disassembly
 * (click a line to select it for Run to Cursor; click its left edge, or
 * double-click it, for its breakpoint), Breakpoints (code, memory, I/O
 * ports, software interrupts), Interrupts (the vector table and a DOS/BIOS
 * call trace) and FujiNet (its UART and the FujiBus packets on its COM
 * port). The toolbar carries the stepping controls and symbol loading; the
 * family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); closing it detaches and lets the machine run on.
 *
 * A short timer polls the engine's generation counter and refreshes the
 * tab showing only when something changed; while the machine runs the
 * status, the registers and the traces refresh twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../KeyForward.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "msdosdebug.h"

#define DISASM_WINDOW 64        /* lines at most; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 64
#define TRACE_MAX 512
#define CONSOLE_KEEP 262144u    /* the console transcript's characters kept */

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_BREAKS, TAB_INTS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names MSDOS_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
static const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "breakpoints", "Breakpoints" }, { "interrupts", "Interrupts" }, { "fujinet", "FujiNet" },
};

/* the general registers, as msdosdebug_set_reg names them */
static const char *const kRegs16[] = { "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "ip" };
static const char *const kRegs32[] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip" };
#define NREGS 9
static const char *const kSregs[] = { "cs", "ds", "es", "ss", "fs", "gs" };
#define NSREGS 6
#define SREG_TAG 100            /* a segment register field's tag: SREG_TAG + index */
/* the flags shown, with their EFLAGS bits */
static const struct { const char *name; int bit; } kFlags[] = {
    { "of", 11 }, { "df", 10 }, { "if", 9 }, { "tf", 8 }, { "sf", 7 }, { "zf", 6 },
    { "af", 4 }, { "pf", 2 }, { "cf", 0 },
};
#define NFLAGS 9

/* ---- helpers --------------------------------------------------------------- */

/* Engine text for a view: control bytes dropped (a label from a map file
 * need not be valid UTF-8). */
static NSString *textFrom(const char *s)
{
    const size_t n = s ? strlen(s) : 0;
    size_t o = 0;
    char *buf = malloc(n + 1);
    if (!buf) return @"";
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 || c == '\n' || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    NSString *r = [NSString stringWithUTF8String:buf];
    if (!r) r = [NSString stringWithCString:buf encoding:NSISOLatin1StringEncoding];
    free(buf);
    return r ?: @"";
}

/* A growing C string the views' text is formatted into. */
typedef struct {
    char *s;
    size_t len, cap;
} Text;

static BOOL tgrow(Text *t, size_t more)
{
    if (t->len + more + 1 <= t->cap) return YES;
    size_t cap = t->cap ? t->cap : 4096;
    while (cap < t->len + more + 1) cap *= 2;
    char *p = realloc(t->s, cap);
    if (!p) return NO;
    t->s = p;
    t->cap = cap;
    return YES;
}

static void tadd(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tadd(Text *t, const char *fmt, ...)
{
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0 && tgrow(t, (size_t)n)) {
        vsnprintf(t->s + t->len, t->cap - t->len, fmt, again);
        t->len += (size_t)n;
    }
    va_end(again);
}

/* The text as a string for a view; the buffer is freed. */
static NSString *tdone(Text *t)
{
    NSString *r = textFrom(t->s ? t->s : "");
    free(t->s);
    t->s = NULL;
    t->len = t->cap = 0;
    return r;
}

static NSString *trimmed(NSString *s)
{
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSString *upper(const char *s)
{
    return [textFrom(s) uppercaseString];
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill (the IBM badge's silver) with the badge's ink on it: the
 * CS:IP line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: MSDOSAccentColor(),
              NSForegroundColorAttributeName: MSDOSInkColor() };
}

/* Hex, as DEBUG.COM takes it ("2F8", "0x2F8", "2F8h"); # for decimal. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = trimmed(text ?: @"");
    const char *s = t.UTF8String;
    const char *digits;
    char *end;
    long v;
    if (!s || !*s) return NO;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        digits = s + 2;
        v = strtol(digits, &end, 16);
    } else if (*s == '#') {
        digits = s + 1;
        v = strtol(digits, &end, 10);
    } else {
        digits = s;
        v = strtol(digits, &end, 16);
    }
    if (end == digits) return NO;
    if (*end == 'h' || *end == 'H') end++;
    while (*end == ' ') end++;
    if (*end) return NO;
    *out = v;
    return YES;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    [s setHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    [s setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    return s;
}

/* A column whose low-hugging member (a scroll view) takes the height left. */
static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.distribution = NSStackViewDistributionFill;
    s.spacing = 6;
    return s;
}

/* A member of a stack as wide as the stack (less its insets). */
static void fillWidth(NSStackView *stack, NSView *view, CGFloat inset)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor constant:-inset].active = YES;
}

/* A member of a row as tall as the row. */
static void fillHeight(NSStackView *stack, NSView *view)
{
    [view.heightAnchor constraintEqualToAnchor:stack.heightAnchor].active = YES;
}

/* A view that takes a stack's spare room along an axis ahead of the rest. */
static void stretchy(NSView *view, NSLayoutConstraintOrientation o)
{
    [view setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:o];
    if ([view isKindOfClass:[NSStackView class]])
        [(NSStackView *)view setHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:o];
}

/* A read-only monospaced text view in a scroll view. The text view starts
 * the size of the clip view, which autoresizing then keeps it (it adds the
 * clip view's changes to its own size). */
static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    const NSSize size = scroll.contentSize;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    view.textColor = NSColor.textColor;
    view.backgroundColor = NSColor.textBackgroundColor;
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    if (wrap) {
        view.horizontallyResizable = NO;
        view.autoresizingMask = NSViewWidthSizable;
        view.textContainer.size = NSMakeSize(size.width, FLT_MAX);
        view.textContainer.widthTracksTextView = YES;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    *scrollOut = scroll;
    return view;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same with attributes (the text is always set again). */
static void setAttributed(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* MSDOS_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory" ...). */
static int tabFromEnv(void)
{
    const char *env = getenv("MSDOS_DEBUGGER_TAB");
    char tab[64];
    if (!env) return -1;
    while (*env == ' ' || *env == '\t') env++;
    snprintf(tab, sizeof tab, "%s", env);
    size_t len = strlen(tab);
    while (len > 0 && (tab[len - 1] == ' ' || tab[len - 1] == '\t')) tab[--len] = '\0';
    if (len == 0) return -1;
    char *end;
    const long n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0') return n >= 0 && n < TAB_COUNT ? (int)n : -1;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < TAB_COUNT; i++) {
            if (pass == 0 && strncasecmp(tab, kTabs[i].name, len) == 0) return i;
            if (pass == 1 && strncasecmp(tab, kTabs[i].label, len) == 0) return i;
            if (pass == 2) {
                const char *p = kTabs[i].label;
                while ((p = strchr(p, ' ')) != NULL)
                    if (strncasecmp(tab, ++p, len) == 0) return i;
            }
        }
    return -1;
}

/* ---- the disassembly's view -------------------------------------------------- */

/* A click reports the line and column clicked (and how many clicks it was),
 * the wheel the rows to move by. */
@interface MSDOSDbgDasmView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScrollRows)(int rows);
@end

@implementation MSDOSDbgDasmView {
    CGFloat _accum;
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSString *text = self.string;
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p], start = 0;
    int line = 0;
    if (idx > text.length) idx = text.length;
    for (NSUInteger i = 0; i < idx; i++)
        if ([text characterAtIndex:i] == '\n') {
            line++;
            start = i + 1;
        }
    if (self.onClick) self.onClick(line, (int)(idx - start), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (!self.onScrollRows) {
        [super scrollWheel:event];
        return;
    }
    /* a trackpad reports points, a wheel lines; down moves on */
    _accum += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 16.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_accum;
    _accum -= rows;
    if (rows != 0) self.onScrollRows(rows);
}
@end

/* ---- the window ----------------------------------------------------------------- */

@interface MSDOSDebuggerWindow () <NSWindowDelegate, NSTextFieldDelegate, NSTabViewDelegate>
@end

@implementation MSDOSDebuggerWindow {
    msdossession *_session;
    msdosdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    BOOL _attachedRunning;      /* attached while the machine was running */
    BOOL _placed;
    int _runningTicks;
    unsigned _messageSerial;

    NSTabView *_tabs;
    NSTextField *_status, *_message;
    NSButton *_runBtn;

    /* symbols */
    NSPopover *_symPopover;
    NSTextField *_symSeg;

    /* console */
    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* cpu + memory */
    NSTextField *_regLabel[NREGS];
    NSTextField *_reg[NREGS];
    NSView *_sregCell[NSREGS];
    NSTextField *_sreg[NSREGS];
    NSTextField *_sbase[NSREGS];
    NSButton *_flag[NFLAGS];
    NSTextField *_cpuInfo;
    NSTextView *_mem;
    NSTextField *_memGoto, *_memAddr, *_memVal;
    uint32_t _memTop;           /* linear */
    int64_t _memMark;           /* the address Go to found; -1 none */
    BOOL _is386;

    /* disassembly */
    MSDOSDbgDasmView *_disasm;
    NSButton *_followPc;
    NSTextField *_jump;
    uint16_t _topSeg;           /* the first line */
    uint32_t _topOff;
    int64_t _selected;          /* the selected line's linear address, or -1 */
    uint32_t _lineLin[DISASM_WINDOW];
    int _lineCount;

    /* breakpoints */
    NSStackView *_bpList;
    NSString *_bpSig;           /* what the list shows, to rebuild on change */
    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpAh, *_bpMsg;

    /* interrupts */
    NSSwitch *_intOn;
    NSTextView *_intText, *_ivtText;
    uint64_t _intLast;
    int _intCount;

    /* fujinet */
    NSTextView *_uartText, *_fujiText;
    NSTextField *_fujiNote;
    uint64_t _fujiLast;
    int _fujiCount;
}

/* ---- lifetime ---------------------------------------------------------------- */

- (instancetype)initWithSession:(msdossession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = msdossession_debugger(session);
    _history = [NSMutableArray array];
    _memMark = -1;
    _topSeg = 0xF000;
    _topOff = 0xFFF0;
    _selected = -1;
    _intCount = -1;
    _fujiCount = -1;
    [self buildWindow];

    __weak MSDOSDebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/Shift+F8 wherever the focus is in this window, the
     * console's field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        MSDOSDebuggerWindow *s = weakSelf;
        if (!s || event.window != s->_window || !s->_window.isVisible) return event;
        return [s handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (void)show
{
    if (!_dbg) return;
    if (!_placed) {
        [_window center];
        _placed = YES;
    }
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    [_window.contentView layoutSubtreeIfNeeded];     /* the disassembly's rows */
    [self attachAndRefresh];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

- (void)hide
{
    if (_window.isVisible) [_window close];         /* windowWillClose: detaches */
}

- (void)toggle
{
    if (_window.isVisible) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return _window.isVisible;
}

/* Closed by Cmd+Shift+D, the close button or the app quitting: the engine
 * lets the machine go. The window is kept, and comes back as it was. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    [_symPopover close];
    if (_dbg) msdosdebug_detach(_dbg);     /* lets it run */
    _attachedRunning = NO;
}

- (void)windowDidResize:(NSNotification *)note
{
    if (note.object == _window && [self currentTab] == TAB_DISASM && msdossession_is_running(_session))
        [self refreshDisasm];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    [item.view layoutSubtreeIfNeeded];
    [self refreshPage:[self currentTab]];
}

- (int)currentTab
{
    return (int)[_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out.
 * Held, the steps repeat; the toggle does not. */
- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    const NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption |
         NSEventModifierFlagCommand);
    if (chars.length != 1) return NO;
    const unichar key = [chars characterAtIndex:0];
    const BOOL shift = mods == NSEventModifierFlagShift;
    if (mods != 0 && !(key == NSF8FunctionKey && shift)) return NO;
    switch (key) {
    case NSF5FunctionKey:
        if (!event.isARepeat) [self toggleRun:nil];
        return YES;
    case NSF7FunctionKey:
        [self step:nil];
        return YES;
    case NSF8FunctionKey:
        if (shift) [self stepOut:nil];
        else [self stepOver:nil];
        return YES;
    default:
        return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

/* Grey, and the first to give way when its row is short of room. */
- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.textColor = NSColor.secondaryLabelColor;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (text.length) l.toolTip = text;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* The Symbols popover: a linker map at a load segment. */
- (NSButton *)buildSymbols
{
    _symSeg = [self field:@"0000" width:70 action:NULL];
    NSTextField *hint = [NSTextField wrappingLabelWithString:
        @"The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h."];
    hint.textColor = NSColor.secondaryLabelColor;
    hint.preferredMaxLayoutWidth = 280;
    [hint.widthAnchor constraintEqualToConstant:280].active = YES;
    NSButton *load = [self button:@"Choose a .map…" action:@selector(loadSymbols:)];
    NSButton *clear = [self button:@"Clear Symbols" action:@selector(clearSymbols:)];
    NSStackView *box = vstack(@[[self heading:@"Symbols"], hstack(@[[self label:@"Load segment"], _symSeg]),
                                hint, load, clear]);
    box.spacing = 8;
    box.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    NSViewController *vc = [[NSViewController alloc] init];
    vc.view = box;
    _symPopover = [[NSPopover alloc] init];
    _symPopover.behavior = NSPopoverBehaviorTransient;
    _symPopover.contentViewController = vc;

    NSButton *b = [self button:@"Symbols" action:@selector(showSymbols:)];
    b.toolTip = @"Labels from an MS LINK or Watcom WLINK .map file";
    return b;
}

- (NSView *)buildToolbar
{
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    _runBtn.toolTip = @"Run, or stop before the next instruction";
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    step.toolTip = @"One instruction";
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    over.toolTip = @"Over a CALL, INT, LOOP or REP, to the instruction after it";
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    outBtn.toolTip = @"On until the routine returns (RET, RETF or IRET)";
    NSButton *runTo = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    runTo.toolTip = @"On to the line selected in the Disassembly";

    /* the stop reason, in the room the buttons leave */
    _status = [self label:@""];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSButton *sym = [self buildSymbols];
    NSStackView *bar = hstack(@[_runBtn, step, over, outBtn, runTo, _status, sym]);
    bar.distribution = NSStackViewDistributionFill;
    return bar;
}

- (NSView *)buildConsole
{
    NSScrollView *scroll;
    _conOut = monoView(&scroll, YES);
    _conOut.string = @"DEBUG.COM's commands: r d e u g t p, plus bp/bpr/bpw/bpi/bpo/bpint, bl, bc, i, o. "
                     @"Type h for the list; Up/Down recall.\n";
    NSTextField *prompt = [self label:@"-"];
    prompt.font = monoFont();
    [prompt setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    /* Enter and Up/Down come through the delegate */
    _conIn = [[NSTextField alloc] init];
    _conIn.font = monoFont();
    _conIn.placeholderString = @"u cs:ip   d ds:0   bpo 2f8   bpint 21 9   g";
    _conIn.delegate = self;
    [_conIn setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[prompt, _conIn]);
    row.distribution = NSStackViewDistributionFill;

    NSStackView *v = vstack(@[scroll, row]);
    fillWidth(v, scroll, 0);
    fillWidth(v, row, 0);
    return v;
}

- (NSView *)buildCpu
{
    /* the general registers across, the segment registers (with their
     * bases) below */
    NSMutableArray<NSView *> *regs = [NSMutableArray array];
    for (int i = 0; i < NREGS; i++) {
        _regLabel[i] = [self label:upper(kRegs16[i])];
        _regLabel[i].textColor = NSColor.secondaryLabelColor;
        _regLabel[i].alignment = NSTextAlignmentRight;
        [_regLabel[i].widthAnchor constraintEqualToConstant:30].active = YES;
        _reg[i] = [self field:@"" width:78 action:@selector(applyRegister:)];
        _reg[i].tag = i;
        _reg[i].toolTip = @"Enter sets it (hex)";
        NSStackView *cell = hstack(@[_regLabel[i], _reg[i]]);
        cell.spacing = 4;
        [regs addObject:cell];
    }
    NSStackView *regRow = hstack(regs);
    regRow.spacing = 8;

    NSMutableArray<NSView *> *segs = [NSMutableArray array];
    for (int i = 0; i < NSREGS; i++) {
        NSTextField *name = [self label:upper(kSregs[i])];
        name.textColor = NSColor.secondaryLabelColor;
        [name.widthAnchor constraintEqualToConstant:24].active = YES;
        _sreg[i] = [self field:@"" width:52 action:@selector(applyRegister:)];
        _sreg[i].tag = SREG_TAG + i;
        _sreg[i].toolTip = @"Enter sets it (hex)";
        _sbase[i] = [self hint:@""];
        _sbase[i].font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
        NSStackView *cell = vstack(@[hstack(@[name, _sreg[i]]), _sbase[i]]);
        cell.spacing = 2;
        _sregCell[i] = cell;
        [segs addObject:cell];
    }
    NSStackView *segRow = hstack(segs);
    segRow.spacing = 18;
    segRow.alignment = NSLayoutAttributeTop;

    NSMutableArray<NSView *> *flags = [NSMutableArray array];
    for (int i = 0; i < NFLAGS; i++) {
        _flag[i] = [NSButton checkboxWithTitle:upper(kFlags[i].name) target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [_flag[i] setRefusesFirstResponder:YES];
        [flags addObject:_flag[i]];
    }
    _cpuInfo = [self hint:@""];
    _cpuInfo.selectable = YES;
    [_cpuInfo setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [flags addObject:_cpuInfo];
    NSStackView *flagRow = hstack(flags);
    flagRow.distribution = NSStackViewDistributionFill;
    [flagRow setCustomSpacing:18 afterView:_flag[NFLAGS - 1]];

    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);

    /* the dump: Go to, a page back or on, and a write */
    _memGoto = [self field:@"ds:si / B800:0 / symbol" width:190 action:@selector(memGoto:)];
    NSButton *back = [self button:@"▲" action:@selector(memPage:)];
    back.tag = -1;
    back.toolTip = @"Back 4K";
    NSButton *on = [self button:@"▼" action:@selector(memPage:)];
    on.tag = 1;
    on.toolTip = @"On 4K";
    _memAddr = [self field:@"0040:0017" width:120 action:@selector(memWrite:)];
    _memVal = [self field:@"00" width:50 action:@selector(memWrite:)];
    NSStackView *nav = hstack(@[[self label:@"Go to"], _memGoto, back, on, [self label:@"Write"], _memAddr,
                                [self label:@"value"], _memVal]);
    [nav setCustomSpacing:18 afterView:on];

    NSTextField *title = [self heading:@"Memory (linear; video RAM at A0000-BFFFF reads as the CPU would)"];
    NSStackView *v = vstack(@[regRow, segRow, flagRow, title, memScroll, nav]);
    fillWidth(v, flagRow, 0);
    fillWidth(v, memScroll, 0);
    stretchy(memScroll, NSLayoutConstraintOrientationVertical);
    _memTop = 0;
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow CS:IP" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    [_followPc setRefusesFirstResponder:YES];
    _jump = [self field:@"F000:FFF0 or _main" width:170 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge or "
                                           @"double-click it for a breakpoint (* on, o off); scroll to browse"]]);
    [row setCustomSpacing:12 afterView:_followPc];
    [row setCustomSpacing:12 afterView:_jump];

    /* no scrollbar: the wheel moves by instructions, as many lines as fit */
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    const NSSize size = scroll.contentSize;
    _disasm = [[MSDOSDbgDasmView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    _disasm.minSize = NSMakeSize(0, 0);
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = monoFont();
    _disasm.backgroundColor = NSColor.textBackgroundColor;
    _disasm.verticallyResizable = YES;
    _disasm.horizontallyResizable = YES;
    _disasm.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak MSDOSDebuggerWindow *weakSelf = self;
    /* A click selects a line (Run to Cursor); a click on its left edge, or
     * a double-click, toggles its breakpoint. */
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        MSDOSDebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        const BOOL gutter = column <= 2;
        s->_selected = s->_lineLin[line];
        if (gutter ? clicks == 1 : clicks == 2) [s toggleBreakpointAt:s->_lineLin[line]];
        else [s refreshDisasm];
    };
    _disasm.onScrollRows = ^(int rows) {
        MSDOSDebuggerWindow *s = weakSelf;
        if (!s || !msdossession_is_running(s->_session)) return;
        if (rows > DISASM_WINDOW) rows = DISASM_WINDOW;
        if (rows < -DISASM_WINDOW) rows = -DISASM_WINDOW;
        s->_followPc.state = NSControlStateValueOff;
        s->_topOff = rows > 0 ? [s rowsOn:s->_topSeg off:s->_topOff rows:rows]
                              : [s rowsBack:s->_topSeg off:s->_topOff rows:-rows];
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fillWidth(v, row, 0);
    fillWidth(v, scroll, 0);
    return v;
}

- (NSView *)buildBreaks
{
    NSTextField *hint = [self hint:@"Execute stops before the instruction runs; memory, ports and INT after "
                                   @"the instruction that touched them."];
    [hint setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *top = hstack(@[hint, [self button:@"Clear All" action:@selector(bpClear:)]]);
    top.distribution = NSStackViewDistributionFill;

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
    _bpList = vstack(@[]);
    _bpList.distribution = NSStackViewDistributionGravityAreas;
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                         forOrientation:NSLayoutConstraintOrientationVertical];

    _bpType = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    /* in msdos_bp_type order */
    [_bpType addItemsWithTitles:@[@"Execute", @"Memory read", @"Memory write", @"Memory read/write",
                                  @"Port IN", @"Port OUT", @"Port IN/OUT", @"INT"]];
    _bpType.target = self;
    _bpType.action = @selector(bpTypeChanged:);
    [_bpType setRefusesFirstResponder:YES];
    _bpStart = [self field:@"1234:0010 / symbol" width:170 action:@selector(bpAdd:)];
    _bpEnd = [self field:@"(end)" width:120 action:@selector(bpAdd:)];
    _bpAh = [self field:@"AH (any)" width:80 action:@selector(bpAdd:)];
    _bpAh.enabled = NO;
    NSStackView *row = hstack(@[_bpType, _bpStart, _bpEnd, _bpAh, [self button:@"Add" action:@selector(bpAdd:)]]);

    _bpMsg = [self label:@""];
    _bpMsg.textColor = NSColor.systemRedColor;
    _bpMsg.lineBreakMode = NSLineBreakByTruncatingTail;
    [_bpMsg setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *v = vstack(@[top, bpScroll, row, _bpMsg]);
    for (NSView *each in @[top, bpScroll, _bpMsg]) fillWidth(v, each, 0);
    return v;
}

/* Two columns, the right one fixed. */
- (NSView *)splitLeft:(NSStackView *)left right:(NSStackView *)right rightWidth:(CGFloat)width
{
    NSStackView *split = hstack(@[left, right]);
    split.distribution = NSStackViewDistributionFill;
    split.spacing = 12;
    stretchy(split, NSLayoutConstraintOrientationVertical);
    stretchy(left, NSLayoutConstraintOrientationHorizontal);
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:width].active = YES;
    fillHeight(split, left);
    fillHeight(split, right);
    return split;
}

- (NSView *)buildInts
{
    NSTextField *title = [self heading:@"INT calls"];
    [title setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    _intOn = [[NSSwitch alloc] init];
    _intOn.target = self;
    _intOn.action = @selector(intSwitched:);
    _intOn.toolTip = @"Record every INT the PC runs (costs speed)";
    NSStackView *row = hstack(@[title, [self hint:@"Trace"], _intOn,
                                [self button:@"Clear" action:@selector(intClear:)]]);
    row.distribution = NSStackViewDistributionFill;
    NSScrollView *intScroll, *ivtScroll;
    _intText = monoView(&intScroll, NO);
    _ivtText = monoView(&ivtScroll, NO);
    NSStackView *left = vstack(@[row, intScroll]);
    fillWidth(left, row, 0);
    fillWidth(left, intScroll, 0);
    NSStackView *right = vstack(@[[self heading:@"Vector table"], ivtScroll]);
    fillWidth(right, ivtScroll, 0);
    return [self splitLeft:left right:right rightWidth:360];
}

- (NSView *)buildFujinet
{
    _fujiNote = [self hint:@""];
    [_fujiNote setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);
    row.distribution = NSStackViewDistributionFill;
    NSScrollView *fujiScroll, *uartScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _uartText = monoView(&uartScroll, NO);
    NSStackView *left = vstack(@[[self heading:@"FujiBus packets (SLIP-decoded)"], row, fujiScroll]);
    fillWidth(left, row, 0);
    fillWidth(left, fujiScroll, 0);
    NSStackView *right = vstack(@[[self heading:@"FujiNet's UART"], uartScroll]);
    fillWidth(right, uartScroll, 0);
    return [self splitLeft:left right:right rightWidth:320];
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1150, 800)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.minSize = NSMakeSize(1000, 620);    /* the registers' row */
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.delegate = self;

    NSView *toolbar = [self buildToolbar];
    NSView *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = [self buildConsole];
    pages[TAB_CPU] = [self buildCpu];
    pages[TAB_DISASM] = [self buildDisasm];
    pages[TAB_BREAKS] = [self buildBreaks];
    pages[TAB_INTS] = [self buildInts];
    pages[TAB_FUJINET] = [self buildFujinet];

    _tabs = [[NSTabView alloc] init];
    for (int i = 0; i < TAB_COUNT; i++) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(kTabs[i].name)];
        item.label = @(kTabs[i].label);
        NSView *content = pages[i];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const int tab = tabFromEnv();
    if (tab >= 0) [_tabs selectTabViewItemAtIndex:tab];
    _tabs.delegate = self;
    [_tabs setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];

    /* messages (Load Symbols, a bad address ...) along the foot */
    _message = [self hint:@""];

    NSStackView *root = vstack(@[toolbar, _tabs, _message]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    fillWidth(root, toolbar, 16);
    fillWidth(root, _tabs, 16);
    fillWidth(root, _message, 16);
    _window.contentView = root;
}

/* ---- refreshing ------------------------------------------------------------------ */

- (void)refreshStatus
{
    const BOOL running = msdossession_is_running(_session) != 0;
    const BOOL stopped = running && msdosdebug_stopped(_dbg);
    NSString *text;
    if (!running) {
        text = @"The PC is not running";
    } else if (stopped) {
        char reason[160];
        reason[0] = '\0';
        msdosdebug_stop_reason(_dbg, reason, (int)sizeof reason);
        text = reason[0] ? [@"Stopped: " stringByAppendingString:textFrom(reason)] : @"Stopped";
    } else {
        text = @"Running";
    }
    _status.stringValue = text;
    _status.toolTip = text;
    /* stopped, Run is lit: the accent with the badge's ink on it */
    NSString *title = stopped ? @"Run (F5)" : @"Stop (F5)";
    if (stopped) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        _runBtn.bezelColor = MSDOSAccentColor();
        _runBtn.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{
            NSForegroundColorAttributeName: MSDOSInkColor(),
            NSFontAttributeName: [NSFont systemFontOfSize:[NSFont systemFontSize]],
            NSParagraphStyleAttributeName: para }];
    } else {
        _runBtn.bezelColor = nil;
        _runBtn.title = title;
    }
}

- (void)refreshCpu
{
    msdos_cpu_regs r;
    char buf[200];
    if (msdosdebug_regs(_dbg, &r) != 0) return;
    const uint32_t vals[NREGS] = { r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip };
    const uint16_t svals[NSREGS] = { r.cs, r.ds, r.es, r.ss, r.fs, r.gs };
    const uint32_t bases[NSREGS] = { r.cs_base, r.ds_base, r.es_base, r.ss_base, r.fs_base, r.gs_base };
    /* a value being typed stays while the machine runs */
    const BOOL running = !msdosdebug_stopped(_dbg);
    _is386 = r.is386 != 0;
    for (int i = 0; i < NREGS; i++) {
        _regLabel[i].stringValue = upper(r.is386 ? kRegs32[i] : kRegs16[i]);
        if (running && _reg[i].currentEditor) continue;
        if (r.is386) snprintf(buf, sizeof buf, "%08X", (unsigned)vals[i]);
        else snprintf(buf, sizeof buf, "%04X", (unsigned)(vals[i] & 0xFFFFu));
        _reg[i].stringValue = textFrom(buf);
    }
    for (int i = 0; i < NSREGS; i++) {
        _sregCell[i].hidden = !(i < 4 || r.is386);
        if (!(running && _sreg[i].currentEditor)) {
            snprintf(buf, sizeof buf, "%04X", (unsigned)svals[i]);
            _sreg[i].stringValue = textFrom(buf);
        }
        snprintf(buf, sizeof buf, "base %05X", (unsigned)bases[i]);
        _sbase[i].stringValue = textFrom(buf);
    }
    for (int i = 0; i < NFLAGS; i++)
        _flag[i].state = ((r.eflags >> kFlags[i].bit) & 1u) ? NSControlStateValueOn : NSControlStateValueOff;

    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    char video[160];
    video[0] = '\0';
    msdosdebug_video_info(_dbg, video, (int)sizeof video);
    size_t vl = strlen(video);
    while (vl > 0 && (video[vl - 1] == '\n' || video[vl - 1] == ' ' || video[vl - 1] == '\r')) video[--vl] = '\0';
    const int mode = r.mode < 0 ? 0 : r.mode > 2 ? 2 : r.mode;
    if (r.is386)
        snprintf(buf, sizeof buf, "%s   %s   CR0 %08X   %s", r.cpu, modes[mode], (unsigned)r.cr0, video);
    else
        snprintf(buf, sizeof buf, "%s   %s", r.cpu, video);
    _cpuInfo.stringValue = textFrom(buf);
    _cpuInfo.toolTip = _cpuInfo.stringValue;
}

- (void)refreshMem
{
    static uint8_t mem[MEM_ROWS * 16];
    Text t = { NULL, 0, 0 };
    size_t markAt = 0, markLen = 0;
    int markRow = -1;
    if (_memMark >= 0 && (uint32_t)_memMark >= _memTop && (uint32_t)_memMark - _memTop < MEM_ROWS * 16)
        markRow = (int)(((uint32_t)_memMark - _memTop) / 16);
    memset(mem, 0, sizeof mem);
    msdosdebug_read(_dbg, _memTop, mem, (int)sizeof mem);
    tadd(&t, "          ");
    for (int col = 0; col < 16; col++) tadd(&t, col == 8 ? " %X  " : "%X  ", col);
    tadd(&t, "\n");
    for (int row = 0; row < MEM_ROWS; row++) {
        const uint8_t *r = mem + row * 16;
        const uint32_t a = _memTop + (uint32_t)row * 16;
        const size_t start = t.len;
        /* real-mode style, segment the address's own 64K */
        tadd(&t, "%04X:%04X ", (unsigned)((a >> 4) & 0xF000u), (unsigned)(a & 0xFFFFu));
        for (int col = 0; col < 16; col++) tadd(&t, col == 8 ? " %02X " : "%02X ", r[col]);
        tadd(&t, " ");
        for (int col = 0; col < 16; col++) tadd(&t, "%c", (r[col] >= 0x20 && r[col] < 0x7F) ? r[col] : '.');
        if (row == markRow) {
            markAt = start;
            markLen = t.len - start;
        }
        if (row < MEM_ROWS - 1) tadd(&t, "\n");
    }
    /* the row Go to found stays marked across refreshes (the dump is ASCII:
     * byte offsets are character offsets) */
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:tdone(&t)
                                                                           attributes:monoAttrs()];
    if (markLen && markAt + markLen <= as.length)
        [as addAttributes:accentAttrs() range:NSMakeRange(markAt, markLen)];
    setAttributed(_mem, as);
}

/* A dump's line (the header is line 0) brought to about a third of the way
 * down. */
- (void)scrollMemToLine:(int)line
{
    NSString *s = _mem.string;
    NSUInteger at = 0;
    for (int l = 0; l < line && at < s.length; l++) {
        const NSRange nl = [s rangeOfString:@"\n" options:0 range:NSMakeRange(at, s.length - at)];
        if (nl.location == NSNotFound) break;
        at = nl.location + 1;
    }
    if (at >= s.length) return;
    NSLayoutManager *lm = _mem.layoutManager;
    NSClipView *clip = _mem.enclosingScrollView.contentView;
    if (!lm || !clip) return;
    [lm ensureLayoutForTextContainer:_mem.textContainer];
    const NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(at, 1) actualCharacterRange:NULL];
    const NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:_mem.textContainer];
    CGFloat y = r.origin.y + _mem.textContainerOrigin.y - clip.bounds.size.height * 0.3;
    const CGFloat maxY = NSHeight(_mem.frame) - clip.bounds.size.height;
    if (y > maxY) y = maxY;
    if (y < 0) y = 0;
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [_mem.enclosingScrollView reflectScrolledClipView:clip];
}

/* The disassembly lines that fit the view. */
- (int)visibleRows
{
    NSClipView *clip = _disasm.enclosingScrollView.contentView;
    const CGFloat line = [_disasm.layoutManager defaultLineHeightForFont:monoFont()];
    const CGFloat height = clip.bounds.size.height - 2 * _disasm.textContainerInset.height;
    if (!clip || line <= 0 || height <= 0) return 32;
    const int rows = (int)(height / line);
    return rows < 4 ? 4 : (rows > DISASM_WINDOW ? DISASM_WINDOW : rows);
}

/* x86 code cannot be read backwards: the offset rows instructions before
 * off is the nearest start whose instructions land on off. */
- (uint32_t)rowsBack:(uint16_t)seg off:(uint32_t)off rows:(int)rows
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int cap = (int)(sizeof lines / sizeof lines[0]);
    const int count = rows * 7 + 1 < cap ? rows * 7 + 1 : cap;
    if (rows <= 0) return off;
    for (int k = rows; k <= rows * 7 && (uint32_t)k <= off; k++) {
        const int n = msdosdebug_disassemble(_dbg, seg, off - (uint32_t)k, count, lines);
        int i = 0;
        while (i < n && lines[i].off < off) i++;
        if (i < n && lines[i].off == off && i >= rows) return lines[i - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

- (uint32_t)rowsOn:(uint16_t)seg off:(uint32_t)off rows:(int)rows
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    const int want = rows + 1 < DISASM_WINDOW + 1 ? rows + 1 : DISASM_WINDOW + 1;
    const int n = msdosdebug_disassemble(_dbg, seg, off, want, lines);
    return n > rows ? lines[rows].off : off;
}

- (void)refreshDisasm
{
    static msdos_dis_line lines[DISASM_WINDOW];
    int bp[DISASM_WINDOW];
    msdos_cpu_regs r;
    const int rows = [self visibleRows];
    const BOOL haveRegs = msdosdebug_regs(_dbg, &r) == 0;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (haveRegs && _followPc.state == NSControlStateValueOn) {
        const uint32_t pc = msdosdebug_linear(_dbg, r.cs, r.eip);
        BOOL shows = NO;
        for (int i = 1; i < _lineCount && i < rows - 2; i++)
            if (_lineLin[i] == pc) shows = YES;
        if (!shows || _topSeg != r.cs) {
            _topSeg = r.cs;
            _topOff = [self rowsBack:r.cs off:r.eip rows:rows / 3];
        }
    }

    int n = msdosdebug_disassemble(_dbg, _topSeg, _topOff, rows + 1 < DISASM_WINDOW ? rows + 1 : DISASM_WINDOW,
                                   lines);
    if (n < 0) n = 0;
    if (n > DISASM_WINDOW) n = DISASM_WINDOW;

    NSMutableArray<NSString *> *texts = [NSMutableArray array];
    NSUInteger width = 0;
    int pcLine = -1;
    _lineCount = n;
    for (int i = 0; i < n; i++) {
        const msdos_dis_line *l = &lines[i];
        const BOOL isPc = haveRegs && l->seg == r.cs && l->off == r.eip;
        bp[i] = msdosdebug_bp_at(_dbg, l->linear);      /* 1 on, 2 off */
        _lineLin[i] = l->linear;
        if (isPc) pcLine = i;
        Text t = { NULL, 0, 0 };
        tadd(&t, "%c%c %04X:%0*X  %-20s %s%s%s", bp[i] == 2 ? 'o' : bp[i] ? '*' : ' ', isPc ? '>' : ' ',
             (unsigned)l->seg, l->off > 0xFFFF ? 8 : 4, (unsigned)l->off, l->bytes, l->symbol,
             l->symbol[0] ? ": " : "", l->text);
        NSString *s = tdone(&t);
        if (s.length > width) width = s.length;
        [texts addObject:s];
    }

    /* each line padded to the widest, so the CS:IP bar is one width */
    NSMutableString *all = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        NSString *padded = [texts[(NSUInteger)i] stringByPaddingToLength:width withString:@" " startingAtIndex:0];
        ranges[i] = NSMakeRange(all.length, padded.length);
        [all appendString:padded];
        if (i < n - 1) [all appendString:@"\n"];
    }
    if (n == 0) [all appendString:@"(no disassembly)"];

    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:all attributes:monoAttrs()];
    NSFont *bold = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightBold];
    for (int i = 0; i < n; i++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bp[i] == 2)
            [as addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor
                       range:NSMakeRange(ranges[i].location, 1)];
        else if (bp[i])
            [as addAttributes:@{ NSForegroundColorAttributeName: NSColor.systemRedColor, NSFontAttributeName: bold }
                        range:NSMakeRange(ranges[i].location, 1)];
        /* the selected line underlined */
        if ((int64_t)lines[i].linear == _selected && i != pcLine)
            [as addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle), NSFontAttributeName: bold }
                        range:ranges[i]];
    }
    /* the CS:IP line in the accent colour, last so its colours win */
    if (pcLine >= 0 && pcLine < n) {
        [as addAttributes:accentAttrs() range:ranges[pcLine]];
        if ((int64_t)lines[pcLine].linear == _selected)
            [as addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[pcLine]];
    }
    [_disasm.textStorage setAttributedString:as];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)refreshBps
{
    static msdos_breakpoint bps[BP_MAX];
    int n = msdosdebug_bp_list(_dbg, bps, BP_MAX);
    if (n < 0) n = 0;
    if (n > BP_MAX) n = BP_MAX;

    /* rebuilt only when it changed (each step refreshes) */
    Text sig = { NULL, 0, 0 };
    for (int i = 0; i < n; i++)
        tadd(&sig, "%d %d %u %s\n", bps[i].id, bps[i].enabled, bps[i].hits, bps[i].text);
    NSString *s = tdone(&sig);
    if (_bpSig && [s isEqualToString:_bpSig]) return;
    _bpSig = s;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0)
        [_bpList addArrangedSubview:[self hint:@"No breakpoints. Click the left edge of a disassembly line, "
                                               @"or add one below."]];
    for (int i = 0; i < n; i++) {
        Text t = { NULL, 0, 0 };
        tadd(&t, "#%-4d %-40s hits %u", bps[i].id, bps[i].text, bps[i].hits);
        NSButton *box = [NSButton checkboxWithTitle:tdone(&t) target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        box.toolTip = @"Enabled";
        [box setRefusesFirstResponder:YES];
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:hstack(@[box, remove])];
    }
}

- (void)refreshInts:(BOOL)force
{
    static msdos_int_event e[TRACE_MAX];
    int n = msdosdebug_int_trace(_dbg, e, TRACE_MAX);
    if (n < 0) n = 0;
    if (n > TRACE_MAX) n = TRACE_MAX;
    const BOOL tracing = msdosdebug_int_trace_enabled(_dbg) != 0;
    _intOn.state = tracing ? NSControlStateValueOn : NSControlStateValueOff;

    /* the vector table: changes rarely, cheap to read */
    Text t = { NULL, 0, 0 };
    for (int i = 0; i < 256; i++) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(_dbg, i, &seg, &off);
        tadd(&t, "%02X %04X:%04X%s", i, (unsigned)seg, (unsigned)off, (i & 3) == 3 ? "\n" : "   ");
    }
    setText(_ivtText, tdone(&t), NO);

    /* a log: set again only when it moved */
    if (!force && n == _intCount && (n == 0 || e[n - 1].serial == _intLast)) return;
    _intCount = n;
    _intLast = n > 0 ? e[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_intText);
    tadd(&t, "From       AX   BX   CX   DX   DS   ES    Call");
    for (int i = 0; i < n; i++)
        tadd(&t, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", (unsigned)e[i].cs,
             (unsigned)(e[i].ip & 0xFFFFu), (unsigned)e[i].ax, (unsigned)e[i].bx, (unsigned)e[i].cx,
             (unsigned)e[i].dx, (unsigned)e[i].ds, (unsigned)e[i].es, e[i].text);
    if (n == 0)
        tadd(&t, "%s", tracing ? "\n(no INT calls since the trace was cleared)"
                               : "\n(turn the trace on to record INT calls; it costs speed)");
    setText(_intText, tdone(&t), follow);
}

- (void)refreshFujinet:(BOOL)force
{
    static msdos_fuji_packet p[TRACE_MAX];
    static char uart[2048];
    int n = msdosdebug_fujinet_trace(_dbg, p, TRACE_MAX);
    if (n < 0) n = 0;
    if (n > TRACE_MAX) n = TRACE_MAX;
    const BOOL fuji = msdossession_fujinet_running(_session) != 0;

    if (!fuji)
        _fujiNote.stringValue = @"FujiNet is off (Preferences ▸ FujiNet).";
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on COM%d%@.",
                                 msdossession_fujinet_com(_session),
                                 msdossession_fujinet_link(_session) ? @", connected" : @", not connected"];
    uart[0] = '\0';
    msdosdebug_uart(_dbg, uart, (int)sizeof uart);
    setText(_uartText, textFrom(uart), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    Text t = { NULL, 0, 0 };
    for (int i = 0; i < n; i++) {
        tadd(&t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++) tadd(&t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) tadd(&t, "…");
    }
    if (n == 0) tadd(&t, "(no FujiBus packets since the trace was cleared)");
    setText(_fujiText, tdone(&t), follow);
}

- (void)refreshPage:(int)page
{
    if (!msdossession_is_running(_session)) return;
    switch (page) {
    case TAB_CPU: [self refreshCpu]; [self refreshMem]; break;
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_INTS: [self refreshInts:NO]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshPage:[self currentTab]];
}

/* After anything that may have moved the engine: the console takes the
 * registers at a new stop, then the views follow. */
- (void)resync
{
    const BOOL stopped = msdosdebug_stopped(_dbg) != 0;
    const unsigned gen = msdosdebug_generation(_dbg);
    if (stopped && (!_wasStopped || gen != _seenGen)) [self consoleStop];
    _wasStopped = stopped;
    _seenGen = gen;
    _runningTicks = 0;
    [self refreshAll];
}

/* From inside a breakpoint row's own action: the list rebuilds after. */
- (void)resyncLater
{
    __weak MSDOSDebuggerWindow *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf resync];
    });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
- (void)settleAndSync
{
    for (int i = 0; i < 40 && !msdosdebug_stopped(_dbg); i++) usleep(1000);
    [self resync];
}

- (void)attachAndRefresh
{
    msdosdebug_attach(_dbg);     /* stops the machine */
    _attachedRunning = msdossession_is_running(_session) != 0;
    _wasStopped = NO;
    [self settleAndSync];
}

- (void)onTick
{
    if (!_window.isVisible || !_dbg) return;
    if (!msdossession_is_running(_session)) {
        if (_attachedRunning) {
            _attachedRunning = NO;
            [self refreshStatus];
        }
        return;
    }
    /* Opened before the PC started, or showing across a restart: the
     * engine attaches (stopping the machine) as it comes up. */
    if (!_attachedRunning) {
        msdosdebug_detach(_dbg);
        [self attachAndRefresh];
        return;
    }
    const unsigned gen = msdosdebug_generation(_dbg);
    const BOOL stopped = msdosdebug_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        [self resync];
    } else if (!stopped && ++_runningTicks >= 5) {
        /* live values twice a second while the machine runs */
        const int page = [self currentTab];
        _runningTicks = 0;
        [self refreshStatus];
        if (page == TAB_CPU) [self refreshCpu];
        if (page == TAB_INTS) [self refreshInts:NO];
        if (page == TAB_FUJINET) [self refreshFujinet:NO];
    }
}

/* ---- the console ------------------------------------------------------------------ */

/* Onto the console's transcript, which keeps its last CONSOLE_KEEP
 * characters, whole lines, and follows its end while the reader is there
 * (or always, force). */
- (void)consoleAppend:(NSString *)text force:(BOOL)force
{
    const BOOL follow = force || atEnd(_conOut);
    NSTextStorage *ts = _conOut.textStorage;
    [ts appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (ts.length > CONSOLE_KEEP) {
        NSString *s = ts.string;
        NSUInteger cut = s.length - CONSOLE_KEEP * 3 / 4;
        const NSRange nl = [s rangeOfString:@"\n" options:0 range:NSMakeRange(cut, s.length - cut)];
        if (nl.location != NSNotFound) cut = nl.location + 1;
        [ts deleteCharactersInRange:NSMakeRange(0, cut)];
    }
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(ts.length, 0)];
}

/* Command output, ended with a newline. */
- (void)consoleOutput:(const char *)text
{
    NSString *t = textFrom(text);
    if (t.length == 0) return;
    [self consoleAppend:[t hasSuffix:@"\n"] ? t : [t stringByAppendingString:@"\n"] force:NO];
}

/* Messages from Load Symbols land in the console and, since it may not be
 * the tab showing, along the foot. */
- (void)say:(NSString *)msg
{
    [self consoleAppend:[msg stringByAppendingString:@"\n"] force:NO];
    [self message:msg];
}

/* Along the window's foot for a few seconds. */
- (void)message:(NSString *)msg
{
    const unsigned serial = ++_messageSerial;
    _message.stringValue = msg;
    _message.toolTip = msg;
    __weak MSDOSDebuggerWindow *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        MSDOSDebuggerWindow *s = weakSelf;
        if (s && s->_messageSerial == serial) s->_message.stringValue = @"";
    });
}

/* At a stop the console shows where, as DEBUG's R does. */
- (void)consoleStop
{
    static char out[4096];
    char reason[160];
    if (!msdossession_is_running(_session)) return;
    reason[0] = '\0';
    msdosdebug_stop_reason(_dbg, reason, (int)sizeof reason);
    if (reason[0]) [self consoleOutput:[[@"— " stringByAppendingString:textFrom(reason)] UTF8String]];
    out[0] = '\0';
    msdosdebug_command(_dbg, "r", out, (int)sizeof out);
    [self consoleOutput:out];
}

/* The console's line. Enter and the arrows arrive mid-edit, so it is read
 * and written through the field editor, which keeps the focus. */
- (NSString *)consoleLine
{
    NSText *ed = _conIn.currentEditor;
    return ed ? ed.string : _conIn.stringValue;
}

- (void)setConsoleLine:(NSString *)text
{
    NSText *ed = _conIn.currentEditor;
    if (ed) {
        ed.string = text;
        [ed setSelectedRange:NSMakeRange(text.length, 0)];
    } else {
        _conIn.stringValue = text;
    }
}

- (void)runConsole
{
    static char out[65536];
    NSString *cmd = trimmed([self consoleLine]);
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    /* a command brings the reader back down */
    [self consoleAppend:[NSString stringWithFormat:@"-%@\n", cmd] force:YES];
    [self setConsoleLine:@""];

    out[0] = '\0';
    msdosdebug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    [self consoleOutput:out];
    /* a t, p or g that stops again reports where, as resync sees the stop */
    _wasStopped = NO;
    [self settleAndSync];
}

- (void)showHistory
{
    [self setConsoleLine:_historyPos < (NSInteger)_history.count ? _history[(NSUInteger)_historyPos] : @""];
}

/* In the console, Enter runs the line, and Up and Down walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(insertNewline:)) {
        [self runConsole];
        return YES;
    }
    if (sel == @selector(moveUp:)) {
        if (_historyPos > 0) {
            _historyPos--;
            [self showHistory];
        }
        return YES;
    }
    if (sel == @selector(moveDown:)) {
        if (_historyPos < (NSInteger)_history.count) {
            _historyPos++;
            [self showHistory];
        }
        return YES;
    }
    return NO;
}

/* ---- the rest of the handlers ---------------------------------------------------- */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (msdosdebug_stopped(_dbg)) msdosdebug_resume(_dbg);
    else msdosdebug_stop(_dbg);
    [self settleAndSync];
}

- (void)step:(id)sender { (void)sender; msdosdebug_step(_dbg); [self settleAndSync]; }
- (void)stepOver:(id)sender { (void)sender; msdosdebug_step_over(_dbg); [self settleAndSync]; }
- (void)stepOut:(id)sender { (void)sender; msdosdebug_step_out(_dbg); [self settleAndSync]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to"];
        return;
    }
    msdosdebug_run_to(_dbg, (uint32_t)_selected);
    [self settleAndSync];
}

- (void)toggleBreakpointAt:(uint32_t)linear
{
    if (msdosdebug_bp_toggle(_dbg, linear) < 0) [self message:@"No room for another breakpoint"];
    [self resync];
}

/* seg:off, a register pair, a linear address or a symbol. */
- (BOOL)parseAddress:(NSString *)text seg:(uint16_t *)seg off:(uint32_t *)off linear:(uint32_t *)lin
{
    NSString *t = trimmed(text ?: @"");
    if (t.length == 0) return NO;
    return msdosdebug_parse_address(_dbg, t.UTF8String, seg, off, lin) == 0;
}

- (void)applyRegister:(NSTextField *)sender
{
    const NSInteger tag = sender.tag;
    const char *name;
    char full[8];
    long v;
    if (tag >= SREG_TAG && tag < SREG_TAG + NSREGS) {
        name = kSregs[tag - SREG_TAG];
    } else if (tag >= 0 && tag < NREGS) {
        name = kRegs16[tag];
        /* the 32-bit name when the 32-bit value shows */
        if (_is386) {
            snprintf(full, sizeof full, "e%s", name);
            name = full;
        }
    } else {
        return;
    }
    if (!parseNum(sender.stringValue, &v))
        [self message:@"Not a number (hex, or # decimal)"];
    else if (!msdosdebug_stopped(_dbg))
        [self message:@"Stop the PC first"];
    else if (msdosdebug_set_reg(_dbg, name, (uint32_t)v) != 0)
        [self message:@"That register cannot be set here"];
    [self resync];
}

- (void)flagClicked:(NSButton *)sender
{
    const NSInteger i = sender.tag;
    if (i >= 0 && i < NFLAGS && msdosdebug_stopped(_dbg))
        msdosdebug_set_reg(_dbg, kFlags[i].name, sender.state == NSControlStateValueOn ? 1u : 0u);
    [self resync];
}

/* Go to: the dump from just above an address (seg:off, a register pair, a
 * linear address or a symbol), its row marked and scrolled to. */
- (void)memGoto:(id)sender
{
    (void)sender;
    uint16_t seg;
    uint32_t off, lin;
    if (![self parseAddress:_memGoto.stringValue seg:&seg off:&off linear:&lin]) {
        _memMark = -1;
        [self refreshMem];
        [self message:@"Not an address (seg:off, ds:si, a linear address, a symbol)"];
        return;
    }
    _memMark = lin;
    _memTop = (lin > 0x100 ? lin - 0x100 : 0) & ~0xFu;
    [self refreshMem];
    [self scrollMemToLine:1 + (int)((lin - _memTop) / 16)];
}

/* The dump a page of MEM_ROWS back or on. */
- (void)memPage:(NSButton *)sender
{
    if (sender.tag < 0)
        _memTop = _memTop > MEM_ROWS * 16 ? _memTop - MEM_ROWS * 16 : 0;
    else
        _memTop += MEM_ROWS * 16;
    [self refreshMem];
}

- (void)memWrite:(id)sender
{
    (void)sender;
    uint16_t seg;
    uint32_t off, lin;
    long v;
    if (![self parseAddress:_memAddr.stringValue seg:&seg off:&off linear:&lin]) {
        [self message:@"Not an address"];
        return;
    }
    if (!parseNum(_memVal.stringValue, &v) || v < 0 || v > 0xFF) {
        [self message:@"The value is a byte (00-FF)"];
        return;
    }
    const uint8_t byte = (uint8_t)v;
    msdosdebug_write(_dbg, lin, &byte, 1);
    [self resync];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    if (msdossession_is_running(_session)) [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    uint16_t seg;
    uint32_t off, lin;
    if (![self parseAddress:_jump.stringValue seg:&seg off:&off linear:&lin]) {
        [self message:@"Not an address (seg:off, cs:ip, a linear address, a symbol)"];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _topSeg = seg;
    _topOff = off;
    _selected = lin;
    [self refreshDisasm];
}

/* The list follows a moment later: these come from its own rows. */
- (void)bpEnabled:(NSButton *)sender
{
    msdosdebug_bp_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self resyncLater];
}

- (void)bpRemove:(NSButton *)sender
{
    msdosdebug_bp_remove(_dbg, (int)sender.tag);
    [self resyncLater];
}

- (void)bpClear:(id)sender
{
    (void)sender;
    msdosdebug_bp_clear(_dbg);
    [self resync];
}

- (void)bpTypeChanged:(id)sender
{
    (void)sender;
    const NSInteger type = _bpType.indexOfSelectedItem;
    _bpAh.enabled = type == MSDOS_BP_INT;
    _bpEnd.enabled = type != MSDOS_BP_INT;
    _bpStart.placeholderString = type <= MSDOS_BP_ACCESS ? @"1234:0010 / symbol"
                               : type == MSDOS_BP_INT ? @"21" : @"2F8";
    _bpEnd.placeholderString = type <= MSDOS_BP_ACCESS ? @"(end)" : @"(last port)";
}

/* Execute and memory breakpoints take addresses; port and INT ones
 * numbers. The end is optional; AH only means something for an INT. */
- (void)bpAdd:(id)sender
{
    (void)sender;
    NSInteger type = _bpType.indexOfSelectedItem;
    NSString *start = trimmed(_bpStart.stringValue);
    NSString *end = trimmed(_bpEnd.stringValue);
    NSString *ahs = trimmed(_bpAh.stringValue);
    uint32_t a, b;
    long ah = -1;
    if (type < 0 || type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    if (type <= MSDOS_BP_ACCESS) {
        uint16_t seg;
        uint32_t off;
        if (![self parseAddress:start seg:&seg off:&off linear:&a]) {
            _bpMsg.stringValue = @"Not an address (seg:off, a linear address, a symbol)";
            return;
        }
        b = a;
        if (end.length && ![self parseAddress:end seg:&seg off:&off linear:&b]) {
            _bpMsg.stringValue = @"The end is not an address";
            return;
        }
    } else {
        long v, e;
        if (!parseNum(start, &v) || v < 0 || v > (type == MSDOS_BP_INT ? 0xFF : 0xFFFF)) {
            _bpMsg.stringValue = type == MSDOS_BP_INT ? @"An interrupt is 00-FF" : @"A port is 0000-FFFF";
            return;
        }
        e = v;
        if (end.length && type != MSDOS_BP_INT && (!parseNum(end, &e) || e < v || e > 0xFFFF)) {
            _bpMsg.stringValue = @"The last port is a port at or after the first";
            return;
        }
        a = (uint32_t)v;
        b = (uint32_t)e;
        if (type == MSDOS_BP_INT && ahs.length && (!parseNum(ahs, &ah) || ah < 0 || ah > 0xFF)) {
            _bpMsg.stringValue = @"AH is 00-FF, or empty for any";
            return;
        }
    }
    if (b < a) {
        _bpMsg.stringValue = @"The end is before the start";
        return;
    }
    if (msdosdebug_bp_add(_dbg, (int)type, a, b, (int)ah) < 0) {
        _bpMsg.stringValue = @"No room for another breakpoint";
        return;
    }
    _bpMsg.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpAh.stringValue = @"";
    [self resync];
}

- (void)intSwitched:(NSSwitch *)sender
{
    msdosdebug_int_trace_enable(_dbg, sender.state == NSControlStateValueOn);
    [self refreshInts:YES];
}

- (void)intClear:(id)sender
{
    (void)sender;
    msdosdebug_int_trace_clear(_dbg);
    [self refreshInts:YES];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    msdosdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)showSymbols:(NSButton *)sender
{
    if (_symPopover.shown) {
        [_symPopover close];
        return;
    }
    NSView *box = _symPopover.contentViewController.view;
    [box layoutSubtreeIfNeeded];
    _symPopover.contentSize = box.fittingSize;
    [_symPopover showRelativeToRect:sender.bounds ofView:sender preferredEdge:NSRectEdgeMaxY];
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    long seg = 0;
    if (!parseNum(_symSeg.stringValue, &seg) || seg < 0 || seg > 0xFFFF) seg = 0;
    [_symPopover close];
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols";
    p.message = @"An MS LINK or Watcom WLINK .map file";
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = NO;
    if ([p runModal] != NSModalResponseOK || !p.URL) return;
    char msg[512];
    msg[0] = '\0';
    if (msdosdebug_load_symbols(_dbg, p.URL.fileSystemRepresentation, (uint16_t)seg, msg, (int)sizeof msg) < 0
        && !msg[0])
        snprintf(msg, sizeof msg, "No symbols found");
    [self say:textFrom(msg)];
    [self resync];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    [_symPopover close];
    msdosdebug_clear_symbols(_dbg);
    [self say:@"Symbols cleared"];
    [self resync];
}
@end
