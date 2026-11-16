/*
 * Debugger window (AppKit) over the session's debugger engine, via
 * core/include/msdosdebug.h. Mirrors the GNOME one (dbg_window.c) tab for
 * tab, and the finished family windows (Adam, Coleco) in how it works.
 *
 * Thirteen tabs: Console (DEBUG.COM's commands and the family's, a line at a
 * time, with a history and Tab completion), Disassembly (click a line to
 * select it for Run to Cursor; click its left edge, or double-click it, or
 * press F9, for its breakpoint), Memory (a hex dump by linear address in 4K
 * pages, Go to, Poke, Search), Breakpoints (all eight types, each with an
 * expression condition, hits and an enable box), Watches, Machine & I/O (the
 * CPU, interrupt controller, timer, DMA, ports, drives and the memory map),
 * Trace (live, one record per instruction), History (what ran before the
 * stop), Video (the card's picture decoded from its memory, registers,
 * palette, memory layout and beam; see VideoTab.m), Sound (the PC speaker and
 * the FM chip), Symbols (a linker map's labels), Interrupts (the vector table
 * and a DOS/BIOS call trace) and FujiNet (its UART and the FujiBus packets on
 * its COM port). Under every tab sits the editable register strip: the
 * general registers (32-bit on a 386 or later), the segment registers with
 * their bases, the flags and the CPU's mode.
 *
 * Showing the window attaches (which stops the machine, as on every sibling);
 * closing it detaches and lets the machine run on. The engine's stop callback
 * would fire on the emulation thread, so the window does not use it: a 100 ms
 * timer polls msdosdebug_stop_snapshot() and msdosdebug_generation() and
 * refreshes only when something changed. While the machine runs the status
 * and the live tabs refresh twice a second, the Video tab every tick and the
 * Trace tab every 0.4 s; the tabs that read the machine's memory or code
 * (Disassembly, Memory, Machine & I/O, Sound) refresh on a stop, a command or
 * when they are shown, never per tick.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, F9 the selected disassembly line's breakpoint.
 *
 * Environment hooks, for tests and captures: MSDOS_DEBUGGER_TAB (an index, a
 * tab's name or label, or an older name: cpu, registers, io),
 * MSDOS_DEBUGGER_WATCH, MSDOS_DEBUGGER_WATCH_BREAK, MSDOS_DEBUGGER_MEM_GOTO,
 * MSDOS_DEBUGGER_HISTORY, MSDOS_DEBUGGER_POPOUT (a tab), and
 * MSDOS_DEBUGGER_VIDEO_* (read by the Video tab). MSDOS_OPEN_DEBUGGER opens
 * the window at launch.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"
#import "VideoTab.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "msdosdebug.h"
#include "msdosmemtools.h"
#include "msdossnd.h"

#define DISASM_WINDOW 64        /* lines at most; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define BP_MAX 128
#define TRACE_MAX 512           /* INT calls and FujiNet packets shown */
#define CONSOLE_KEEP 262144u    /* the console transcript's characters kept */
#define LIVE_TICKS 5            /* 5 x 100 ms: the live refresh while running */
#define TRACE_TICKS 4           /* the Trace tab, every 0.4 s */
#define HIST_BUF 4000           /* records read from the ring */
#define HIST_SHOWN 400          /* lines shown */
#define POKE_MAX 64
#define SYMBOL_LIST_MAX 20000   /* rows the Symbols tab lists */

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_MACHINE, TAB_TRACE, TAB_HISTORY, TAB_VIDEO,
    TAB_SOUND, TAB_SYMBOLS, TAB_INTS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names MSDOS_DEBUGGER_TAB takes (they end the help
 * contexts, debugger/<name>), and their labels (which it takes too). */
static const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "machine", "Machine & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "interrupts", "Interrupts" }, { "fujinet", "FujiNet" },
};

/* The older tab names the tests and captures still use */
static const struct { const char *alias; int tab; } kTabAliases[] = {
    { "cpu", TAB_DISASM }, { "registers", TAB_MEMORY }, { "io", TAB_MACHINE }, { "cart", TAB_MACHINE },
};

/* the general registers, as the engine names them */
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

#define NUM_FILE_KINDS 3
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "mem", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Memory (first megabyte)…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = { @"disassembly.asm", @"memory.bin", @"registers.txt" };

/* ---- helpers --------------------------------------------------------------- */

/* Engine text for a view: control bytes dropped (a label from a map file
 * need not be valid UTF-8). */
static NSString *textFrom(const char *source)
{
    const size_t length = source ? strlen(source) : 0;
    size_t outPos = 0;
    char *buf = malloc(length + 1);
    if (!buf) return @"";
    for (size_t idx = 0; idx < length; idx++) {
        const unsigned char chr = (unsigned char)source[idx];
        if (chr >= 0x20 || chr == '\n' || chr == '\t') buf[outPos++] = (char)chr;
    }
    buf[outPos] = '\0';
    NSString *result = [NSString stringWithUTF8String:buf];
    if (!result) result = [[NSString alloc] initWithBytes:buf length:outPos encoding:NSISOLatin1StringEncoding];
    free(buf);
    return result ?: @"";
}

/* A growing C string the views' text is formatted into. */
typedef struct {
    char *buf;
    size_t len, cap;
} Text;

static BOOL tgrow(Text *txt, size_t more)
{
    if (txt->len + more + 1 <= txt->cap) return YES;
    size_t cap = txt->cap ? txt->cap : 4096;
    while (cap < txt->len + more + 1) cap *= 2;
    char *grown = realloc(txt->buf, cap);
    if (!grown) return NO;
    txt->buf = grown;
    txt->cap = cap;
    return YES;
}

static void tadd(Text *txt, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tadd(Text *txt, const char *fmt, ...)
{
    va_list args, again;
    va_start(args, fmt);
    va_copy(again, args);
    const int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed > 0 && tgrow(txt, (size_t)needed)) {
        vsnprintf(txt->buf + txt->len, txt->cap - txt->len, fmt, again);
        txt->len += (size_t)needed;
    }
    va_end(again);
}

/* The text as a string for a view; the buffer is freed. */
static NSString *tdone(Text *txt)
{
    NSString *result = textFrom(txt->buf ? txt->buf : "");
    free(txt->buf);
    txt->buf = NULL;
    txt->len = txt->cap = 0;
    return result;
}

static NSString *trimmed(NSString *text)
{
    return [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSString *upper(const char *text)
{
    return [textFrom(text) uppercaseString];
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
 * CS:IP line, a marked row, a lit button. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: MSDOSAccentColor(),
              NSForegroundColorAttributeName: MSDOSInkColor() };
}

/* A value that moved since the last look: the accent's light variant on a dark
 * theme, where it reads; on a light one the accent darkened, since the badge's
 * silver is too pale for text on white. */
static NSColor *changedColor(void)
{
    NSString *name = [NSApp.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    if ([name isEqualToString:NSAppearanceNameDarkAqua]) return MSDOSAccentLightColor();
    return [MSDOSAccentColor() blendedColorWithFraction:0.55 ofColor:NSColor.blackColor] ?: NSColor.systemOrangeColor;
}

/* Hex, as DEBUG.COM takes it ("2F8", "0x2F8", "2F8h"); # for decimal. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *token = trimmed(text ?: @"");
    const char *chars = token.UTF8String;
    const char *digits;
    char *end;
    long value;
    if (!chars || !*chars) return NO;
    if (chars[0] == '0' && (chars[1] == 'x' || chars[1] == 'X')) {
        digits = chars + 2;
        value = strtol(digits, &end, 16);
    } else if (*chars == '#') {
        digits = chars + 1;
        value = strtol(digits, &end, 10);
    } else {
        digits = chars;
        value = strtol(digits, &end, 16);
    }
    if (end == digits) return NO;
    if (*end == 'h' || *end == 'H') end++;
    while (*end == ' ') end++;
    if (*end) return NO;
    *out = value;
    return YES;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    [stack setHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    [stack setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    return stack;
}

/* A column whose low-hugging member (a scroll view) takes the height left. */
static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing = 6;
    return stack;
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
static void stretchy(NSView *view, NSLayoutConstraintOrientation orientation)
{
    [view setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:orientation];
    if ([view isKindOfClass:[NSStackView class]])
        [(NSStackView *)view setHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:orientation];
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
        const NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:origin];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same with attributes (the text is always set again). */
static void setAttributed(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:origin];
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

/* MSDOS_DEBUGGER_TAB, MSDOS_DEBUGGER_POPOUT: a page number, an older name,
 * or a tab's name or label (any case), or the start of one, or of a word of a
 * label. -1 for none. */
static int tabFromText(const char *env)
{
    char tab[64];
    if (!env) return -1;
    while (*env == ' ' || *env == '\t') env++;
    snprintf(tab, sizeof tab, "%s", env);
    size_t len = strlen(tab);
    while (len > 0 && (tab[len - 1] == ' ' || tab[len - 1] == '\t')) tab[--len] = '\0';
    if (len == 0) return -1;
    char *end;
    const long number = strtol(tab, &end, 10);
    if (end != tab && *end == '\0') return number >= 0 && number < TAB_COUNT ? (int)number : -1;
    for (size_t idx = 0; idx < sizeof kTabAliases / sizeof kTabAliases[0]; idx++)
        if (strcasecmp(tab, kTabAliases[idx].alias) == 0) return kTabAliases[idx].tab;
    for (int pass = 0; pass < 3; pass++)
        for (int idx = 0; idx < TAB_COUNT; idx++) {
            if (pass == 0 && strncasecmp(tab, kTabs[idx].name, len) == 0) return idx;
            if (pass == 1 && strncasecmp(tab, kTabs[idx].label, len) == 0) return idx;
            if (pass == 2) {
                const char *word = kTabs[idx].label;
                while ((word = strchr(word, ' ')) != NULL)
                    if (strncasecmp(tab, ++word, len) == 0) return idx;
            }
        }
    return -1;
}

/* A linear address as the disassembly's segment and offset (the segment's own
 * 64K below a megabyte). */
static void segOffFromLinear(uint32_t linear, uint16_t *seg, uint32_t *off)
{
    *seg = (uint16_t)((linear >> 4) & 0xF000u);
    *off = linear & 0xFFFFu;
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
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSString *text = self.string;
    NSUInteger found = [self characterIndexForInsertionAtPoint:point], start = 0;
    int line = 0;
    if (found > text.length) found = text.length;
    for (NSUInteger pos = 0; pos < found; pos++)
        if ([text characterAtIndex:pos] == '\n') {
            line++;
            start = pos + 1;
        }
    if (self.onClick) self.onClick(line, (int)(found - start), event.clickCount);
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

/* A list of lines the reader picks one of by clicking (the Watches tab, and
 * the Symbols tab, where a double-click goes to the address). The picked line
 * is kept in `picked`, -1 for none, and drawn on the accent by whoever owns
 * the text. */
@interface MSDOSDbgPickView : NSTextView
@property (nonatomic) int picked;
@property (nonatomic, copy) void (^onPick)(int line, NSInteger clicks);
@end

@implementation MSDOSDbgPickView
- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSString *text = self.string;
    NSUInteger found = [self characterIndexForInsertionAtPoint:point];
    int line = 0;
    if (found > text.length) found = text.length;
    for (NSUInteger pos = 0; pos < found; pos++)
        if ([text characterAtIndex:pos] == '\n') line++;
    self.picked = line;
    if (self.onPick) self.onPick(line, event.clickCount);
}
@end

/* Text for a pick list, the picked line drawn on the accent. */
static void setPickText(MSDOSDbgPickView *view, NSArray<NSString *> *lines)
{
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    int line = 0;
    for (NSString *source in lines) {
        NSMutableAttributedString *entry = [[NSMutableAttributedString alloc]
            initWithString:[source stringByAppendingString:@"\n"] attributes:monoAttrs()];
        if (line == view.picked) [entry addAttributes:accentAttrs() range:NSMakeRange(0, entry.length)];
        [text appendAttributedString:entry];
        line++;
    }
    setAttributed(view, text);
}

static MSDOSDbgPickView *makePickView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    const NSSize size = scroll.contentSize;
    MSDOSDbgPickView *view = [[MSDOSDbgPickView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    view.editable = NO;
    view.richText = YES;
    view.selectable = NO;
    view.font = monoFont();
    view.backgroundColor = NSColor.textBackgroundColor;
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    view.picked = -1;
    scroll.documentView = view;
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    *scrollOut = scroll;
    return view;
}

/* One breakpoint-table cell: a fixed-width monospace label, or (width 0) the
 * flexible one that takes what is left. */
static NSTextField *bpCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [label.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [label setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return label;
}

/* ---- the window ----------------------------------------------------------------- */

@interface MSDOSDebuggerWindow () <NSWindowDelegate, NSTextFieldDelegate, NSTabViewDelegate>
@end

@implementation MSDOSDebuggerWindow {
    msdossession *_session;
    msdosdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    uint64_t _seenSerial;
    unsigned _seenGen;
    BOOL _wasStopped;
    BOOL _attachedRunning;      /* attached while the machine was running */
    BOOL _placed;
    BOOL _envApplied;
    int _runningTicks, _traceTicks;
    unsigned _messageSerial;

    NSTabView *_tabs;
    NSTextField *_status, *_message;
    NSButton *_runBtn;

    /* Tabs peeled off into windows of their own */
    NSTabViewItem *_items[TAB_COUNT];
    NSWindow *_popped[TAB_COUNT];

    /* console */
    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* the register strip under every tab */
    NSTextField *_regLabel[NREGS];
    NSTextField *_reg[NREGS];
    NSView *_sregCell[NSREGS];
    NSTextField *_sreg[NSREGS];
    NSTextField *_sbase[NSREGS];
    NSButton *_flag[NFLAGS];
    NSTextField *_cpuInfo;
    BOOL _is386;
    BOOL _havePrev;
    uint32_t _prevReg[NREGS], _prevSreg[NSREGS], _prevFlags;

    /* disassembly */
    MSDOSDbgDasmView *_disasm;
    NSButton *_followPc;
    NSTextField *_jump;
    uint16_t _topSeg;           /* the first line */
    uint32_t _topOff;
    int64_t _selected;          /* the selected line's linear address, or -1 */
    uint32_t _lineLin[DISASM_WINDOW];
    int _lineCount;

    /* memory: the dump, the marked range, the Poke bar and the search */
    NSTextView *_mem;
    NSTextField *_memGoto, *_memWrite, *_memNote;
    uint32_t _memTop;           /* linear, 16-aligned */
    BOOL _memHl;
    uint32_t _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    /* breakpoints */
    NSStackView *_bpList;
    NSString *_bpSig;           /* what the list shows, to rebuild on change */
    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpAh, *_bpCond, *_bpMsg, *_bpStop;

    /* watches */
    NSTextField *_wtEntry;
    MSDOSDbgPickView *_wtList;

    /* machine & I/O, sound */
    NSTextView *_machineText, *_soundText;

    /* trace (0, live) and history (1, when stopped) share one ring */
    NSButton *_hRec[2];
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];
    uint64_t _hSeen[2];

    /* symbols */
    MSDOSDbgPickView *_symList;
    NSTextField *_symFilter, *_symCount, *_symSeg;
    NSString *_symSig;
    NSMutableArray<NSNumber *> *_symAddrs;

    /* interrupts */
    NSSwitch *_intOn;
    NSTextView *_intText, *_ivtText;
    uint64_t _intLast;
    int _intCount;

    /* video */
    MSDOSVideoTab *_video;

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
    _symAddrs = [NSMutableArray array];
    _topSeg = 0xF000;
    _topOff = 0xFFF0;
    _selected = -1;
    _intCount = -1;
    _fujiCount = -1;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self buildWindow];

    __weak MSDOSDebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/Shift+F8/F9 wherever the focus is in this window (or a tab
     * peeled off into its own), the console's field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        MSDOSDebuggerWindow *owner = weakSelf;
        if (!owner || ![owner ownsWindow:event.window]) return event;
        return [owner handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window) return NO;
    if (window == _window) return _window.isVisible;
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (window == _popped[slot]) return YES;
    return NO;
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
    for (int slot = 0; slot < TAB_COUNT; slot++) [_popped[slot] orderFront:nil];
    [_window.contentView layoutSubtreeIfNeeded];     /* the disassembly's rows */
    [self attachAndRefresh];
    [self applyEnvironment];
    [self resync];
    if ([self tabVisible:TAB_CONSOLE]) [_window makeFirstResponder:_conIn];
}

/* The captures' hooks, once the window is up and the machine attached
 * (closing it turns the Trace and History records off, so they could not be
 * set before): MSDOS_DEBUGGER_WATCH="x" adds a watch,
 * MSDOS_DEBUGGER_WATCH_BREAK="x" a breakpoint on a write that changes it,
 * MSDOS_DEBUGGER_MEM_GOTO="x" shows it in Memory, MSDOS_DEBUGGER_HISTORY
 * records the History ring, MSDOS_DEBUGGER_POPOUT=<tab> pops that tab out into
 * a window of its own. */
- (void)applyEnvironment
{
    if (_envApplied) return;
    _envApplied = YES;
    const char *watchText = getenv("MSDOS_DEBUGGER_WATCH"), *breakText = getenv("MSDOS_DEBUGGER_WATCH_BREAK"),
               *gotoText = getenv("MSDOS_DEBUGGER_MEM_GOTO");
    uint32_t first, last;
    if (watchText && *watchText) msdoswatch_add(watchText);
    if (breakText && *breakText && msdossym_parse_range(_dbg, breakText, &first, &last))
        [self breakOnChangeFrom:first to:last];
    if (gotoText && *gotoText && msdossym_parse_range(_dbg, gotoText, &first, &last))
        [self showInMemoryFrom:first last:last];
    if (getenv("MSDOS_DEBUGGER_HISTORY")) msdosdebug_history_enable(_dbg, 1);
    const int pop = tabFromText(getenv("MSDOS_DEBUGGER_POPOUT"));
    if (pop >= 0) [self popOutTab:pop];
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

/* The main window's close control and Window > Close: a peeled-off tab's
 * window docks it instead. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (sender == _popped[slot]) {
            [self dockTab:slot];
            return NO;
        }
    return YES;
}

/* Closed by Cmd+Shift+D, the close button or the app quitting: the records go
 * off and the engine lets the machine go. The window is kept, and comes back
 * as it was. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    for (int slot = 0; slot < TAB_COUNT; slot++) [_popped[slot] orderOut:nil];
    if (_dbg) {
        msdosdebug_trace_enable(_dbg, 0);
        msdosdebug_history_enable(_dbg, 0);
        msdosdebug_detach(_dbg);     /* lets it run; breakpoints are kept */
    }
    _attachedRunning = NO;
    _envApplied = YES;      /* the hooks are for the first opening only */
}

- (void)windowDidResize:(NSNotification *)note
{
    if (note.object == _window && [self tabVisible:TAB_DISASM] && msdossession_is_running(_session) &&
        msdosdebug_stopped(_dbg))
        [self refreshDisasm];
}

/* On screen: the tab view's selected page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || _tabs.selectedTabViewItem == _items[tab];
}

/* Command+? and the Help button: the tab's section */
- (void)setHelpContext
{
    NSString *ident = _tabs.selectedTabViewItem.identifier;
    if (ident) MSDOSSetHelpContext(_window, [@"debugger/" stringByAppendingString:ident]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    [item.view layoutSubtreeIfNeeded];
    [self setHelpContext];
    [self refreshVisibleForce:YES];
    if ([self tabVisible:TAB_CONSOLE]) [_window makeFirstResponder:_conIn];
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out, F9
 * the selected line's breakpoint. Held, the steps repeat; the toggle does not. */
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
    case NSF9FunctionKey:
        if (!event.isARepeat) [self toggleSelected:nil];
        return YES;
    default:
        return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *button = [NSButton buttonWithTitle:title target:self action:sel];
    [button setRefusesFirstResponder:YES];
    return button;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

/* Grey, and the first to give way when its row is short of room. */
- (NSTextField *)hint:(NSString *)text
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.textColor = NSColor.secondaryLabelColor;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (text.length) label.toolTip = text;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return label;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.font = monoFont();
    field.target = self;
    field.action = sel;
    if (width > 0) [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [popup addItemWithTitle:title];
    popup.lastItem.tag = -1;
    for (NSUInteger idx = 0; idx < items.count; idx++) {
        [popup addItemWithTitle:items[idx]];
        popup.lastItem.tag = (NSInteger)idx;
    }
    popup.target = self;
    popup.action = sel;
    [popup setRefusesFirstResponder:YES];
    return popup;
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

    NSMutableArray<NSString *> *saveTitles = [NSMutableArray array];
    for (int kind = 0; kind < NUM_FILE_KINDS; kind++) [saveTitles addObject:kFileTitles[kind]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    save.toolTip = @"Save the disassembly from CS:IP, the first megabyte or the registers to a file";
    NSButton *popOut = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popOut.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";

    /* the stop reason, in the room the buttons leave */
    _status = [self label:@""];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_status setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSButton *help = MSDOSHelpButton(nil);
    [help setRefusesFirstResponder:YES];
    NSStackView *bar = hstack(@[_runBtn, step, over, outBtn, runTo, _status, save, popOut, help]);
    bar.distribution = NSStackViewDistributionFill;
    return bar;
}

- (NSView *)buildConsole
{
    NSScrollView *scroll;
    _conOut = monoView(&scroll, YES);
    _conOut.string = @"DEBUG.COM's commands: r d e u g t p, plus bp/bpr/bpw/bpa/bpin/bpout/bpint, bl, bc, i, o, and "
                     @"the family's (print, set, find, watch, trace, symbols, map, io, sound, video). "
                     @"Type h for the list; Tab completes; Up/Down recall.\n";
    NSTextField *prompt = [self label:@"-"];
    prompt.font = monoFont();
    [prompt setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    /* Enter, Tab and Up/Down come through the delegate */
    _conIn = [[NSTextField alloc] init];
    _conIn.font = monoFont();
    _conIn.placeholderString = @"u cs:ip   d ds:0   bpo 2f8   bpint 21 9   bpw 46c if value > old   g";
    _conIn.delegate = self;
    [_conIn setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[prompt, _conIn]);
    row.distribution = NSStackViewDistributionFill;

    NSStackView *column = vstack(@[scroll, row]);
    fillWidth(column, scroll, 0);
    fillWidth(column, row, 0);
    return column;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow CS:IP" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    [_followPc setRefusesFirstResponder:YES];
    _jump = [self field:@"F000:FFF0 or _main" width:170 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge, "
                                           @"double-click it or press F9 for a breakpoint (* on, o off); scroll to browse"]]);
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
        MSDOSDebuggerWindow *owner = weakSelf;
        if (!owner || line < 0 || line >= owner->_lineCount) return;
        const BOOL gutter = column <= 2;
        owner->_selected = owner->_lineLin[line];
        if (gutter ? clicks == 1 : clicks == 2) [owner toggleBreakpointAt:owner->_lineLin[line]];
        else [owner refreshDisasm];
    };
    _disasm.onScrollRows = ^(int rows) {
        MSDOSDebuggerWindow *owner = weakSelf;
        if (!owner || !msdossession_is_running(owner->_session) || !msdosdebug_stopped(owner->_dbg)) return;
        if (rows > DISASM_WINDOW) rows = DISASM_WINDOW;
        if (rows < -DISASM_WINDOW) rows = -DISASM_WINDOW;
        owner->_followPc.state = NSControlStateValueOff;
        owner->_topOff = rows > 0 ? [owner rowsOn:owner->_topSeg off:owner->_topOff rows:rows]
                                  : [owner rowsBack:owner->_topSeg off:owner->_topOff rows:-rows];
        [owner refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *column = vstack(@[row, scroll]);
    fillWidth(column, row, 0);
    fillWidth(column, scroll, 0);
    return column;
}

/* The address space by linear address: a go-to bar, a search, the dump (4K at
 * a time) and a Poke bar. */
- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memGoto = [self field:@"ds:si / B800:0 / B8000 / symbol" width:220 action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"▲" action:@selector(memPrev:)];
    prev.toolTip = @"Back 4K";
    NSButton *next = [self button:@"▼" action:@selector(memNext:)];
    next.toolTip = @"On 4K";
    NSStackView *memHead = hstack(@[[self label:@"Address:"], _memGoto, [self button:@"Go" action:@selector(gotoMem:)],
                                    prev, next,
                                    [self hint:@"linear, as the CPU reads it (paging honoured); video RAM at A0000-BFFFF too"]]);
    memHead.distribution = NSStackViewDistributionFill;

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex bytes", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (B8 34 12) or text, one character per byte";
    [_srchKind setRefusesFirstResponder:YES];
    _srchEntry = [self field:@"B8 34 12, or some text" width:0 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);
    searchRow.distribution = NSStackViewDistributionFill;

    _memWrite = [self field:@"address value ... (hex; several values go to consecutive addresses)" width:0
                     action:@selector(writeMem:)];
    _memNote = [self hint:@""];
    NSStackView *pokeRow = hstack(@[[self label:@"Poke"], _memWrite, [self button:@"Write" action:@selector(writeMem:)],
                                    _memNote]);
    pokeRow.distribution = NSStackViewDistributionFill;

    NSStackView *column = vstack(@[memHead, searchRow, memScroll, pokeRow]);
    fillWidth(column, memHead, 0);
    fillWidth(column, searchRow, 0);
    fillWidth(column, memScroll, 0);
    fillWidth(column, pokeRow, 0);
    stretchy(memScroll, NSLayoutConstraintOrientationVertical);
    return column;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to the
     * top and to the clip view's width so rows keep the tab's width. */
    NSStackView *header = hstack(@[bpCell(@"On  #", 70, YES), bpCell(@"Breakpoint", 330, YES),
                                   bpCell(@"Condition", 0, YES), bpCell(@"Hits", 60, YES), bpCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
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
    _bpType.toolTip = @"Execute stops before the instruction runs; memory, ports and INT after the instruction "
                      @"that touched them";
    [_bpType setRefusesFirstResponder:YES];
    _bpStart = [self field:@"1234:0010 / symbol" width:170 action:@selector(bpAdd:)];
    _bpEnd = [self field:@"(end)" width:120 action:@selector(bpAdd:)];
    _bpAh = [self field:@"AH (any)" width:80 action:@selector(bpAdd:)];
    _bpAh.enabled = NO;
    _bpCond = [self field:@"condition (optional): value != old, ax == 5, [46C] > 3" width:0 action:@selector(bpAdd:)];
    _bpCond.toolTip = @"An expression: the registers and flags (ax, eax, cs, cf ...), [addr] the byte and {addr} the "
                      @"word, labels, seg:off, frame, cycles and, on a memory or port access, value, addr and old "
                      @"(the byte a write replaced).";
    [_bpCond setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_bpType, _bpStart, _bpEnd, _bpAh, [self label:@"If"], _bpCond,
                                [self button:@"Add" action:@selector(bpAdd:)],
                                [self button:@"Clear Hits" action:@selector(bpClearHits:)],
                                [self button:@"Clear All" action:@selector(bpClear:)]]);
    row.distribution = NSStackViewDistributionFill;

    _bpMsg = [self label:@""];
    _bpMsg.textColor = NSColor.systemRedColor;
    _bpMsg.lineBreakMode = NSLineBreakByTruncatingTail;
    [_bpMsg setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    _bpStop = [self hint:@""];
    NSTextField *hint = [self hint:@"Untick to disable; edit a condition and press Enter (empty removes it). A port is "
                                   @"0-FFFF, an INT 00-FF; the end defaults to the start."];

    NSStackView *column = vstack(@[header, bpScroll, hint, row, _bpMsg, _bpStop]);
    for (NSView *each in @[header, bpScroll, hint, row, _bpMsg, _bpStop]) fillWidth(column, each, 0);
    return column;
}

- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, symbol, expression or low-high" width:0 action:@selector(addWatch:)];
    NSStackView *form = hstack(@[[self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    form.distribution = NSStackViewDistributionFill;

    NSScrollView *scroll;
    _wtList = makePickView(&scroll);
    __weak MSDOSDebuggerWindow *weakSelf = self;
    _wtList.onPick = ^(int line, NSInteger clicks) {
        (void)line;
        (void)clicks;
        [weakSelf refreshWatches];
    };

    NSStackView *foot = hstack(@[[self button:@"Memory" action:@selector(wtMemory:)],
                                 [self button:@"Break on Change" action:@selector(wtBreak:)],
                                 [self button:@"Remove" action:@selector(wtRemove:)],
                                 [self hint:@"Values show while the PC is stopped"]]);

    NSStackView *column = vstack(@[form, scroll, foot]);
    fillWidth(column, form, 0);
    fillWidth(column, scroll, 0);
    return column;
}

/* A page that is one read-only text view. */
- (NSView *)buildTextTab:(NSTextView *__strong *)view
{
    NSScrollView *scroll;
    *view = monoView(&scroll, NO);
    return scroll;
}

- (NSView *)buildHist:(int)which
{
    _hRec[which] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[which] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[which].tag = which;
    _hRec[which].toolTip = @"Record every instruction the CPU runs (the interpreter is slower still while recording)";
    _hFilter[which] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[which] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[which].target = self;
    _hFilter[which].action = @selector(histChanged:);
    [_hFilter[which] setRefusesFirstResponder:YES];
    _hRange[which] = [self field:@"range: low-high or a symbol" width:200 action:@selector(histChanged:)];
    _hNote[which] = [self hint:@""];
    NSButton *clear = [self button:@"Clear" action:@selector(histClear:)];
    NSStackView *bar = hstack(@[_hRec[which], _hFilter[which], _hRange[which], clear, _hNote[which]]);
    bar.distribution = NSStackViewDistributionFill;
    NSScrollView *scroll;
    _hText[which] = monoView(&scroll, NO);
    NSStackView *column = vstack(@[bar, scroll]);
    fillWidth(column, bar, 0);
    fillWidth(column, scroll, 0);
    return column;
}

- (NSView *)buildSymbols
{
    _symFilter = [self field:@"Filter (Enter to apply)" width:0 action:@selector(symFilterChanged:)];
    _symCount = [self hint:@""];
    NSStackView *filterRow = hstack(@[_symFilter, _symCount]);
    filterRow.distribution = NSStackViewDistributionFill;

    _symSeg = [self field:@"0000" width:70 action:nil];
    _symSeg.toolTip = @"The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h (hex)";
    NSButton *load = [self button:@"Load…" action:@selector(loadSymbols:)];
    load.toolTip = @"Labels from an MS LINK or Watcom WLINK .map file";
    NSStackView *loadRow = hstack(@[[self label:@"Load segment"], _symSeg, load,
                                    [self button:@"Clear" action:@selector(clearSymbols:)],
                                    [self button:@"Break at" action:@selector(symBreak:)],
                                    [self hint:@"Double-click a row to show it in the Disassembly"]]);
    loadRow.distribution = NSStackViewDistributionFill;

    NSScrollView *scroll;
    _symList = makePickView(&scroll);
    __weak MSDOSDebuggerWindow *weakSelf = self;
    _symList.onPick = ^(int line, NSInteger clicks) {
        MSDOSDebuggerWindow *owner = weakSelf;
        if (!owner || line < 0 || line >= (int)owner->_symAddrs.count) return;
        owner->_symSig = nil;       /* redraw with the pick */
        [owner refreshSymbols];
        if (clicks >= 2) {      /* activating a row goes to that address in the Disassembly */
            const uint32_t linear = owner->_symAddrs[(NSUInteger)line].unsignedIntValue;
            [owner showInDisassembly:linear];
        }
    };

    NSStackView *column = vstack(@[filterRow, loadRow, scroll]);
    fillWidth(column, filterRow, 0);
    fillWidth(column, loadRow, 0);
    fillWidth(column, scroll, 0);
    return column;
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

/* The registers under every tab, each a field that takes a value (hex, a
 * label or an expression) while the PC is stopped; Enter applies it. The
 * segment registers show their bases, the flags are checkboxes. */
- (NSView *)buildRegStrip
{
    /* the general registers across, the segment registers (with their
     * bases) below */
    NSMutableArray<NSView *> *regs = [NSMutableArray array];
    for (int idx = 0; idx < NREGS; idx++) {
        _regLabel[idx] = [self label:upper(kRegs16[idx])];
        _regLabel[idx].textColor = NSColor.secondaryLabelColor;
        _regLabel[idx].alignment = NSTextAlignmentRight;
        [_regLabel[idx].widthAnchor constraintEqualToConstant:28].active = YES;
        _reg[idx] = [self field:@"" width:70 action:@selector(applyRegister:)];
        _reg[idx].tag = idx;
        _reg[idx].toolTip = @"Edit while the PC is stopped (hex, a label or an expression), then Enter";
        NSStackView *cell = hstack(@[_regLabel[idx], _reg[idx]]);
        cell.spacing = 4;
        [regs addObject:cell];
    }
    NSStackView *regRow = hstack(regs);
    regRow.spacing = 8;

    NSMutableArray<NSView *> *segs = [NSMutableArray array];
    for (int idx = 0; idx < NSREGS; idx++) {
        NSTextField *name = [self label:upper(kSregs[idx])];
        name.textColor = NSColor.secondaryLabelColor;
        [name.widthAnchor constraintEqualToConstant:24].active = YES;
        _sreg[idx] = [self field:@"" width:52 action:@selector(applyRegister:)];
        _sreg[idx].tag = SREG_TAG + idx;
        _sreg[idx].toolTip = @"Edit while the PC is stopped (hex), then Enter; real mode only";
        _sbase[idx] = [self hint:@""];
        _sbase[idx].font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
        NSStackView *cell = hstack(@[name, _sreg[idx], _sbase[idx]]);
        cell.spacing = 4;
        _sregCell[idx] = cell;
        [segs addObject:cell];
    }
    NSStackView *segRow = hstack(segs);
    segRow.spacing = 14;

    NSMutableArray<NSView *> *flags = [NSMutableArray array];
    for (int idx = 0; idx < NFLAGS; idx++) {
        _flag[idx] = [NSButton checkboxWithTitle:upper(kFlags[idx].name) target:self action:@selector(flagClicked:)];
        _flag[idx].tag = idx;
        [_flag[idx] setRefusesFirstResponder:YES];
        [flags addObject:_flag[idx]];
    }
    _cpuInfo = [self hint:@""];
    _cpuInfo.selectable = YES;
    _cpuInfo.font = monoFont();
    [_cpuInfo setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [flags addObject:_cpuInfo];
    NSStackView *flagRow = hstack(flags);
    flagRow.distribution = NSStackViewDistributionFill;
    [flagRow setCustomSpacing:18 afterView:_flag[NFLAGS - 1]];

    NSStackView *strip = vstack(@[regRow, segRow, flagRow]);
    fillWidth(strip, flagRow, 0);
    return strip;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 860)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.minSize = NSMakeSize(1040, 700);    /* the registers' row and the tabs */
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.delegate = self;

    NSView *toolbar = [self buildToolbar];
    _video = [[MSDOSVideoTab alloc] initWithSession:_session];
    NSView *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = [self buildConsole];
    pages[TAB_DISASM] = [self buildDisasm];
    pages[TAB_MEMORY] = [self buildMemory];
    pages[TAB_BREAKS] = [self buildBreaks];
    pages[TAB_WATCHES] = [self buildWatches];
    pages[TAB_MACHINE] = [self buildTextTab:&_machineText];
    pages[TAB_TRACE] = [self buildHist:0];
    pages[TAB_HISTORY] = [self buildHist:1];
    pages[TAB_VIDEO] = _video.view;
    pages[TAB_SOUND] = [self buildTextTab:&_soundText];
    pages[TAB_SYMBOLS] = [self buildSymbols];
    pages[TAB_INTS] = [self buildInts];
    pages[TAB_FUJINET] = [self buildFujinet];

    _tabs = [[NSTabView alloc] init];
    for (int idx = 0; idx < TAB_COUNT; idx++) {   /* each keeps its place when moved around */
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(kTabs[idx].name)];
        item.label = @(kTabs[idx].label);
        NSView *content = pages[idx];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        _items[idx] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("MSDOS_DEBUGGER_TAB"));
    if (first >= 0) [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self setHelpContext];
    [_tabs setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];

    /* the registers sit under every tab */
    NSView *strip = [self buildRegStrip];

    /* messages (Load Symbols, a bad address ...) along the foot */
    _message = [self hint:@""];

    NSStackView *root = vstack(@[toolbar, _tabs, strip, _message]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    fillWidth(root, toolbar, 16);
    fillWidth(root, _tabs, 16);
    fillWidth(root, strip, 16);
    fillWidth(root, _message, 16);
    _window.contentView = root;
}

/* ---- refreshing ------------------------------------------------------------------ */

/* Why the machine stopped, worded by the core, first letter up. */
static NSString *stopText(const msdos_stop_snapshot *snap)
{
    NSString *why = textFrom(snap->text);
    if (why.length == 0) return @"";
    return [[[why substringToIndex:1] uppercaseString] stringByAppendingString:[why substringFromIndex:1]];
}

- (void)refreshStatus
{
    msdos_stop_snapshot snap;
    const BOOL running = msdossession_is_running(_session) != 0;
    msdosdebug_stop_snapshot(_dbg, &snap);
    const BOOL stopped = running && snap.paused != 0;
    NSString *text;
    if (!running) {
        text = @"The PC is not running";
    } else if (stopped) {
        NSString *why = stopText(&snap);
        text = why.length ? why : @"Stopped";
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

/* The strip. It reads the CPU only while the PC is stopped: a running one
 * keeps the last values and says so. */
- (void)refreshRegs
{
    msdos_cpu_regs regs;
    char buf[200];
    const BOOL running = msdossession_is_running(_session) != 0;
    if (!running) {
        _cpuInfo.stringValue = @"The PC is not running";
        return;
    }
    if (!msdosdebug_stopped(_dbg)) {
        _cpuInfo.stringValue = @"running; stop it (F5) to read and edit the registers";
        _cpuInfo.toolTip = _cpuInfo.stringValue;
        return;
    }
    if (msdosdebug_regs(_dbg, &regs) != 0) return;
    const uint32_t vals[NREGS] = { regs.eax, regs.ebx, regs.ecx, regs.edx, regs.esi, regs.edi, regs.ebp, regs.esp,
                                   regs.eip };
    const uint32_t svals[NSREGS] = { regs.cs, regs.ds, regs.es, regs.ss, regs.fs, regs.gs };
    const uint32_t bases[NSREGS] = { regs.cs_base, regs.ds_base, regs.es_base, regs.ss_base, regs.fs_base,
                                     regs.gs_base };
    NSColor *moved = changedColor();
    _is386 = regs.is386 != 0;
    for (int idx = 0; idx < NREGS; idx++) {
        const uint32_t shown = regs.is386 ? vals[idx] : (vals[idx] & 0xFFFFu);
        _regLabel[idx].stringValue = upper(regs.is386 ? kRegs32[idx] : kRegs16[idx]);
        _reg[idx].textColor = _havePrev && _prevReg[idx] != shown ? moved : NSColor.textColor;
        _prevReg[idx] = shown;
        if (_reg[idx].currentEditor) continue;      /* never over a field being edited */
        if (regs.is386) snprintf(buf, sizeof buf, "%08X", (unsigned)shown);
        else snprintf(buf, sizeof buf, "%04X", (unsigned)shown);
        _reg[idx].stringValue = textFrom(buf);
    }
    for (int idx = 0; idx < NSREGS; idx++) {
        _sregCell[idx].hidden = !(idx < 4 || regs.is386);
        _sreg[idx].textColor = _havePrev && _prevSreg[idx] != svals[idx] ? moved : NSColor.textColor;
        _prevSreg[idx] = svals[idx];
        if (!_sreg[idx].currentEditor) {
            snprintf(buf, sizeof buf, "%04X", (unsigned)svals[idx]);
            _sreg[idx].stringValue = textFrom(buf);
        }
        snprintf(buf, sizeof buf, "base %05X", (unsigned)bases[idx]);
        _sbase[idx].stringValue = textFrom(buf);
    }
    for (int idx = 0; idx < NFLAGS; idx++) {
        const uint32_t bit = (regs.eflags >> kFlags[idx].bit) & 1u;
        const BOOL changed = _havePrev && ((_prevFlags >> kFlags[idx].bit) & 1u) != bit;
        _flag[idx].state = bit ? NSControlStateValueOn : NSControlStateValueOff;
        _flag[idx].attributedTitle = [[NSAttributedString alloc] initWithString:upper(kFlags[idx].name) attributes:@{
            NSForegroundColorAttributeName: changed ? moved : NSColor.textColor }];
    }
    _prevFlags = regs.eflags;
    _havePrev = YES;

    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    char video[160];
    video[0] = '\0';
    msdosdebug_video_info(_dbg, video, (int)sizeof video);
    size_t vlen = strlen(video);
    while (vlen > 0 && (video[vlen - 1] == '\n' || video[vlen - 1] == ' ' || video[vlen - 1] == '\r'))
        video[--vlen] = '\0';
    const int mode = regs.mode < 0 ? 0 : regs.mode > 2 ? 2 : regs.mode;
    if (regs.is386)
        snprintf(buf, sizeof buf, "%s   %s   CR0 %08X   %s", regs.cpu, modes[mode], (unsigned)regs.cr0, video);
    else
        snprintf(buf, sizeof buf, "%s   %s", regs.cpu, video);
    _cpuInfo.stringValue = textFrom(buf);
    _cpuInfo.toolTip = _cpuInfo.stringValue;
}

- (void)refreshMem
{
    static uint8_t mem[MEM_BYTES];
    msdos_region map[16];
    char region[64], lastRegion[64];
    Text txt = { NULL, 0, 0 };
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];
    if (_memTop > 0xFFFFFFFFu - MEM_BYTES) _memTop = 0xFFFFFFFFu - MEM_BYTES + 1;
    _memTop &= ~0xFu;
    memset(mem, 0, sizeof mem);
    msdosdebug_read(_dbg, _memTop, mem, MEM_BYTES);
    int regions = msdosdebug_memory_map(_dbg, map, 16);
    if (regions < 0) regions = 0;
    lastRegion[0] = '\0';
    tadd(&txt, "        ");
    for (int col = 0; col < 16; col++) tadd(&txt, col == 8 ? " %X  " : "%X  ", col);
    tadd(&txt, "\n");
    for (int row = 0; row < MEM_ROWS; row++) {
        const uint8_t *bytes = mem + row * 16;
        const uint32_t base = _memTop + (uint32_t)row * 16;
        tadd(&txt, "%06X  ", (unsigned)base);
        for (int col = 0; col < 16; col++) {
            const uint32_t addr = base + (uint32_t)col;
            if (col == 8) tadd(&txt, " ");
            if (_memHl && addr >= _memHlLo && addr <= _memHlHi)
                [marks addObject:[NSValue valueWithRange:NSMakeRange(txt.len, 2)]];
            tadd(&txt, "%02X ", bytes[col]);
        }
        tadd(&txt, " ");
        for (int col = 0; col < 16; col++) tadd(&txt, "%c", (bytes[col] >= 0x20 && bytes[col] < 0x7F) ? bytes[col] : '.');
        /* label a row where the region it is in changes */
        region[0] = '\0';
        if (msdosmem_region_name(map, regions, base, region, (int)sizeof region) > 0 &&
            strcmp(region, lastRegion) != 0)
            tadd(&txt, "  ; %s", region);
        snprintf(lastRegion, sizeof lastRegion, "%s", region);
        if (row < MEM_ROWS - 1) tadd(&txt, "\n");
    }
    /* the dump is ASCII: byte offsets are character offsets */
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:tdone(&txt)
                                                                               attributes:monoAttrs()];
    for (NSValue *span in marks)
        if (NSMaxRange(span.rangeValue) <= styled.length) [styled addAttributes:accentAttrs() range:span.rangeValue];
    setAttributed(_mem, styled);
}

/* A dump's line (the header is line 0) brought to about a third of the way
 * down. */
- (void)scrollMemToLine:(int)line
{
    NSString *text = _mem.string;
    NSUInteger offset = 0;
    for (int count = 0; count < line && offset < text.length; count++) {
        const NSRange newline = [text rangeOfString:@"\n" options:0 range:NSMakeRange(offset, text.length - offset)];
        if (newline.location == NSNotFound) break;
        offset = newline.location + 1;
    }
    if (offset >= text.length) return;
    NSLayoutManager *layout = _mem.layoutManager;
    NSClipView *clip = _mem.enclosingScrollView.contentView;
    if (!layout || !clip) return;
    [layout ensureLayoutForTextContainer:_mem.textContainer];
    const NSRange glyphs = [layout glyphRangeForCharacterRange:NSMakeRange(offset, 1) actualCharacterRange:NULL];
    const NSRect rect = [layout boundingRectForGlyphRange:glyphs inTextContainer:_mem.textContainer];
    CGFloat ypos = rect.origin.y + _mem.textContainerOrigin.y - clip.bounds.size.height * 0.3;
    const CGFloat maxY = NSHeight(_mem.frame) - clip.bounds.size.height;
    if (ypos > maxY) ypos = maxY;
    if (ypos < 0) ypos = 0;
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, ypos)];
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
    for (int back = rows; back <= rows * 7 && (uint32_t)back <= off; back++) {
        const int total = msdosdebug_disassemble(_dbg, seg, off - (uint32_t)back, count, lines);
        int found = 0;
        while (found < total && lines[found].off < off) found++;
        if (found < total && lines[found].off == off && found >= rows) return lines[found - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

- (uint32_t)rowsOn:(uint16_t)seg off:(uint32_t)off rows:(int)rows
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    const int want = rows + 1 < DISASM_WINDOW + 1 ? rows + 1 : DISASM_WINDOW + 1;
    const int total = msdosdebug_disassemble(_dbg, seg, off, want, lines);
    return total > rows ? lines[rows].off : off;
}

- (void)refreshDisasm
{
    static msdos_dis_line lines[DISASM_WINDOW];
    int bpState[DISASM_WINDOW];
    msdos_cpu_regs regs;
    const int rows = [self visibleRows];
    const BOOL haveRegs = msdosdebug_regs(_dbg, &regs) == 0;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (haveRegs && _followPc.state == NSControlStateValueOn) {
        const uint32_t pcLinear = msdosdebug_linear(_dbg, regs.cs, regs.eip);
        BOOL shows = NO;
        for (int idx = 1; idx < _lineCount && idx < rows - 2; idx++)
            if (_lineLin[idx] == pcLinear) shows = YES;
        if (!shows || _topSeg != regs.cs) {
            _topSeg = regs.cs;
            _topOff = [self rowsBack:regs.cs off:regs.eip rows:rows / 3];
        }
    }

    int total = msdosdebug_disassemble(_dbg, _topSeg, _topOff, rows + 1 < DISASM_WINDOW ? rows + 1 : DISASM_WINDOW,
                                       lines);
    if (total < 0) total = 0;
    if (total > DISASM_WINDOW) total = DISASM_WINDOW;

    msdos_breakpoint bpList[MSDOS_MAX_BREAKPOINTS];
    const int bpCount = msdosdebug_bp_list(_dbg, bpList, MSDOS_MAX_BREAKPOINTS);
    NSMutableArray<NSString *> *texts = [NSMutableArray array];
    NSUInteger width = 0;
    int pcLine = -1;
    _lineCount = total;
    for (int idx = 0; idx < total; idx++) {
        const msdos_dis_line *line = &lines[idx];
        const BOOL isPc = haveRegs && line->seg == regs.cs && line->off == regs.eip;
        bpState[idx] = 0;                                         /* 1 on, 2 off */
        for (int slot = 0; slot < bpCount; slot++)
            if (bpList[slot].type == MSDOS_BP_EXEC && bpList[slot].start == line->linear &&
                bpList[slot].end == line->linear && bpState[idx] != 1)
                bpState[idx] = bpList[slot].enabled ? 1 : 2;
        _lineLin[idx] = line->linear;
        if (isPc) pcLine = idx;
        Text txt = { NULL, 0, 0 };
        tadd(&txt, "%c%c %04X:%0*X  %-20s %s%s%s", bpState[idx] == 2 ? 'o' : bpState[idx] ? '*' : ' ',
             isPc ? '>' : ' ', (unsigned)line->seg, line->off > 0xFFFF ? 8 : 4, (unsigned)line->off, line->bytes,
             line->symbol, line->symbol[0] ? ": " : "", line->text);
        NSString *entry = tdone(&txt);
        if (entry.length > width) width = entry.length;
        [texts addObject:entry];
    }

    /* each line padded to the widest, so the CS:IP bar is one width */
    NSMutableString *all = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW];
    for (int idx = 0; idx < total; idx++) {
        NSString *padded = [texts[(NSUInteger)idx] stringByPaddingToLength:width withString:@" " startingAtIndex:0];
        ranges[idx] = NSMakeRange(all.length, padded.length);
        [all appendString:padded];
        if (idx < total - 1) [all appendString:@"\n"];
    }
    if (total == 0) [all appendString:@"(no disassembly)"];

    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:all attributes:monoAttrs()];
    NSFont *bold = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightBold];
    for (int idx = 0; idx < total; idx++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bpState[idx] == 2)
            [styled addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor
                           range:NSMakeRange(ranges[idx].location, 1)];
        else if (bpState[idx])
            [styled addAttributes:@{ NSForegroundColorAttributeName: NSColor.systemRedColor, NSFontAttributeName: bold }
                            range:NSMakeRange(ranges[idx].location, 1)];
        /* the selected line underlined */
        if ((int64_t)lines[idx].linear == _selected && idx != pcLine)
            [styled addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle), NSFontAttributeName: bold }
                            range:ranges[idx]];
    }
    /* the CS:IP line in the accent colour, last so its colours win */
    if (pcLine >= 0 && pcLine < total) {
        [styled addAttributes:accentAttrs() range:ranges[pcLine]];
        if ((int64_t)lines[pcLine].linear == _selected)
            [styled addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[pcLine]];
    }
    [_disasm.textStorage setAttributedString:styled];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

/* Whether a breakpoint row's condition field is being edited: the list is
 * then left alone, or the edit would be thrown away. */
- (BOOL)editingBreakpointRow
{
    NSResponder *focus = _window.firstResponder;
    if (![focus isKindOfClass:[NSTextView class]]) return NO;
    NSView *owner = (NSView *)[(NSTextView *)focus delegate];
    return [owner isKindOfClass:[NSView class]] && [owner isDescendantOf:_bpList];
}

- (void)refreshBps
{
    static msdos_breakpoint bps[BP_MAX];
    int count = msdosdebug_bp_list(_dbg, bps, BP_MAX);
    if (count < 0) count = 0;
    if (count > BP_MAX) count = BP_MAX;

    /* why the machine last stopped, worded by the core */
    {
        msdos_stop_snapshot snap;
        msdosdebug_stop_snapshot(_dbg, &snap);
        NSString *why = stopText(&snap);
        _bpStop.stringValue = !snap.paused ? @"Running."
                              : why.length ? [@"Stopped: " stringByAppendingString:why] : @"Stopped.";
    }

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click or an edit) */
    Text sig = { NULL, 0, 0 };
    for (int idx = 0; idx < count; idx++)
        tadd(&sig, "%d %d %u %s|%s\n", bps[idx].id, bps[idx].enabled, bps[idx].hits, bps[idx].text,
             bps[idx].condition);
    NSString *sigText = tdone(&sig);
    if (_bpSig && [sigText isEqualToString:_bpSig]) return;
    if ([self editingBreakpointRow]) return;
    _bpSig = sigText;

    for (NSView *old in [_bpList.arrangedSubviews copy]) [_bpList removeView:old];
    if (count == 0)
        [_bpList addArrangedSubview:[self hint:@"No breakpoints. Click the left edge of a disassembly line, "
                                               @"or add one below."]];
    for (int idx = 0; idx < count; idx++) {
        char described[320];
        msdosbp_describe(_dbg, &bps[idx], described, (int)sizeof described);
        char label[64];
        label[0] = '\0';
        Text what = { NULL, 0, 0 };
        tadd(&what, "%s", bps[idx].text);
        if (bps[idx].type <= MSDOS_BP_ACCESS && msdosdebug_symbol_at(_dbg, bps[idx].start, label, sizeof label) > 0 &&
            label[0])
            tadd(&what, " (%s)", label);

        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[idx].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[idx].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[idx].id;
        box.font = monoFont();
        box.toolTip = @"Enabled";
        [box setRefusesFirstResponder:YES];
        [box.widthAnchor constraintEqualToConstant:70].active = YES;

        NSTextField *condition = [self field:@"(none)" width:0 action:@selector(bpCondEdited:)];
        condition.stringValue = textFrom(bps[idx].condition);
        condition.tag = bps[idx].id;
        condition.toolTip = @"The condition: an expression, evaluated when the breakpoint fires. Enter applies it; "
                            @"empty removes it.";
        [condition setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[idx].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;

        NSTextField *whatLabel = bpCell(tdone(&what), 330, NO);
        whatLabel.toolTip = textFrom(described);
        NSStackView *row = hstack(@[box, whatLabel, condition,
                                    bpCell([NSString stringWithFormat:@"%u", bps[idx].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int count = msdoswatch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (int idx = 0; idx < count; idx++) {
        char line[256];
        line[0] = '\0';
        msdoswatch_format(_dbg, idx, line, (int)sizeof line);
        [lines addObject:textFrom(line)];
    }
    if (count == 0) [lines addObject:@"No watches. Type an address, symbol, expression or low-high range above."];
    if (_wtList.picked >= count) _wtList.picked = -1;
    setPickText(_wtList, lines);
}

/* The CPU, the interrupt controller, the timer, DMA, the ports and drives, the
 * FujiNet link, and the memory map: what the machine answers where. */
- (void)refreshMachine
{
    static char text[16384];
    msdos_region map[16];
    NSMutableString *out = [NSMutableString string];
    text[0] = '\0';
    msdosdebug_io_text(_dbg, text, (int)sizeof text);
    [out appendString:textFrom(text)];
    if (![out hasSuffix:@"\n"]) [out appendString:@"\n"];
    [out appendString:@"\nMemory map\n"];
    const int regions = msdosdebug_memory_map(_dbg, map, 16);
    if (regions <= 0) [out appendString:@"  The PC is not running.\n"];
    for (int idx = 0; idx < regions; idx++)
        [out appendFormat:@"  %08X-%08X  %c%c  %@\n", (unsigned)map[idx].low, (unsigned)map[idx].high,
                          map[idx].readable ? 'r' : '-', map[idx].writable ? 'w' : '-', textFrom(map[idx].name)];
    setText(_machineText, out, NO);
}

/* Trace (live) and History (when stopped), from the one ring. */
- (void)refreshHist:(int)which force:(BOOL)force
{
    static msdos_trace_entry buf[HIST_BUF];
    const BOOL live = which == 0;
    const BOOL stopped = msdosdebug_stopped(_dbg) != 0;
    const BOOL traceOn = msdosdebug_trace_enabled(_dbg) != 0, histOn = msdosdebug_history_enabled(_dbg) != 0;
    const BOOL recording = live ? traceOn : histOn;
    const uint64_t total = live ? msdosdebug_trace_total(_dbg) : msdosdebug_history_count(_dbg);
    uint32_t low = 0, high = 0xFFFFFFFFu;
    int filter = MSDOS_TRACE_ALL;

    for (int pair = 0; pair < 2; pair++) {      /* the Record buttons follow the engine */
        const BOOL pairOn = pair == 0 ? traceOn : histOn;
        _hRec[pair].state = pairOn ? NSControlStateValueOn : NSControlStateValueOff;
        _hRec[pair].bezelColor = pairOn ? MSDOSAccentColor() : nil;
        _hRec[pair].contentTintColor = pairOn ? MSDOSInkColor() : nil;
    }
    if (!force && total == _hSeen[which] && (live || stopped)) return;
    _hSeen[which] = total;
    if (!live && !stopped) {
        _hNote[which].stringValue = @"Stop the PC (F5) to read its history.";
        return;
    }
    switch (_hFilter[which].indexOfSelectedItem) {
    case 1: filter = MSDOS_TRACE_BRANCHES; break;
    case 2:
        if (msdossym_parse_range(_dbg, trimmed(_hRange[which].stringValue).UTF8String, &low, &high))
            filter = MSDOS_TRACE_RANGE;
        break;
    default: break;
    }
    /* the newest matches, scanning back from the newest; shown oldest first */
    int hit[HIST_SHOWN], shown = 0, count;
    if (live) {
        count = msdosdebug_trace_read(_dbg, buf, HIST_BUF);                  /* newest first */
        for (int rec = 0; rec < count && shown < HIST_SHOWN; rec++)
            if (msdosdebug_trace_match(&buf[rec], filter, low, high)) hit[shown++] = rec;
    } else {
        count = msdosdebug_history_copy(_dbg, buf, HIST_BUF);                /* oldest first */
        for (int rec = count - 1; rec >= 0 && shown < HIST_SHOWN; rec--)
            if (msdosdebug_trace_match(&buf[rec], filter, low, high)) hit[shown++] = rec;
    }
    Text txt = { NULL, 0, 0 };
    if (!recording && count == 0) tadd(&txt, "Recording is off. Press Record, then let the PC run.");
    for (int pair = shown - 1; pair >= 0; pair--) {
        char line[512];
        line[0] = '\0';
        msdosdebug_trace_format(&buf[hit[pair]], line, (int)sizeof line);
        tadd(&txt, "%s\n", line);
    }
    setText(_hText[which], tdone(&txt), YES);
    _hNote[which].stringValue = [NSString stringWithFormat:@"%@%llu instructions recorded, last %d shown",
                                                           recording ? @"Recording. " : @"",
                                                           (unsigned long long)total, shown];
}

/* The PC speaker, the FM chip and the Sound Blaster's DSP as far as they are
 * decoded, and every configured card by name. Only a stopped PC is read. */
- (void)refreshSound
{
    static msdos_snd_state snd;
    if (!msdossession_is_running(_session) || !msdosdebug_stopped(_dbg)) {
        setText(_soundText, @"Stop the PC (F5) to read the sound devices.", NO);
        return;
    }
    if (msdosdebug_sound_get(_dbg, &snd) != 0) {
        setText(_soundText, @"The sound devices could not be read.", NO);
        return;
    }
    char note[8];
    Text txt = { NULL, 0, 0 };
    tadd(&txt, "PC speaker (decoded, %s)\n", snd.speaker_direct ? "read from the PIT and PPI" : "from observed writes");
    tadd(&txt, "  port 61h %02X   gate %d   speaker data %d   PIT 2 mode %d   reload %u\n", snd.port61, snd.port61 & 1,
         (snd.port61 >> 1) & 1, snd.pit_mode, (unsigned)snd.pit_reload);
    if (snd.speaker_on) {
        msdos_snd_note_name(msdos_snd_hz_to_note(snd.speaker_hz), note, sizeof note);
        tadd(&txt, "  tone on: %.1f Hz (%s)\n", snd.speaker_hz, note);
    } else if (snd.speaker_dac) {
        tadd(&txt, "  not a tone; bit 1 is toggled by hand (clicks or samples)\n");
    } else {
        tadd(&txt, "  silent\n");
    }
    tadd(&txt, "  writes seen since attach: port 61h %u, PIT 2 %u\n\n", (unsigned)snd.port61_writes,
         (unsigned)snd.pit2_writes);

    tadd(&txt, "FM chip (decoded from the writes seen since the Debugger attached)\n");
    if (!snd.opl_seen) {
        tadd(&txt, "  no OPL write seen at 388h-38Bh or 220h-223h/228h-229h\n");
    } else {
        tadd(&txt, "  %s, %u writes   AM depth %d   vibrato depth %d%s\n", snd.opl3 ? "OPL3" : "OPL2",
             (unsigned)snd.opl_writes, snd.tremolo_depth, snd.vibrato_depth, (snd.rhythm & 0x20) ? "   rhythm on" : "");
        if (snd.rhythm & 0x20)
            tadd(&txt, "  drums: bass %d snare %d tom %d cymbal %d hi-hat %d\n", (snd.rhythm >> 4) & 1,
                 (snd.rhythm >> 3) & 1, (snd.rhythm >> 2) & 1, (snd.rhythm >> 1) & 1, snd.rhythm & 1);
        tadd(&txt, "  Ch  Key  FNum Blk        Hz  Note  FB  Mode      Pan\n");
        for (int chan = 0; chan < snd.opl_channels && chan < MSDOS_SND_OPL_CHANNELS; chan++) {
            const msdos_snd_channel *voice = &snd.channel[chan];
            msdos_snd_note_name(voice->note, note, sizeof note);
            tadd(&txt, "  %2d  %-3s  %4d  %d  %9.2f  %-4s  %d  %-8s  %s%s\n", chan, voice->keyon ? "KEY" : "off",
                 voice->fnum, voice->block, voice->hz, note, voice->feedback, voice->additive ? "additive" : "FM",
                 voice->pan == 3 ? "L+R" : voice->pan == 1 ? "L" : voice->pan == 2 ? "R" : "-",
                 voice->four_op ? "  4-operator lead" : "");
            if (voice->keyon) {
                const msdos_snd_operator *ops[2] = { &voice->mod, &voice->car };
                static const char *const names[2] = { "mod", "car" };
                for (int which = 0; which < 2; which++)
                    tadd(&txt, "        %s  mult %2d  level %2d  attack %2d  decay %2d  sustain %2d  release %2d  wave %d\n",
                         names[which], ops[which]->multiple, ops[which]->total_level, ops[which]->attack,
                         ops[which]->decay, ops[which]->sustain_level, ops[which]->release, ops[which]->wave);
            }
        }
    }

    tadd(&txt, "\nSound Blaster DSP (decoded at the default base 220h only)\n");
    if (!snd.sb_seen) {
        tadd(&txt, "  no DSP or mixer write seen\n");
    } else {
        tadd(&txt, "  commands %u, last %02Xh   speaker %s", (unsigned)snd.sb_commands, snd.sb_last_command,
             snd.sb_speaker_on < 0 ? "unknown" : snd.sb_speaker_on ? "on" : "off");
        if (snd.sb_sample_rate) tadd(&txt, "   sample rate %d Hz", snd.sb_sample_rate);
        if (snd.sb_time_constant >= 0) tadd(&txt, " (time constant %d)", snd.sb_time_constant);
        tadd(&txt, "\n");
    }

    tadd(&txt, "\nSound cards (from the configuration)\n");
    if (snd.card_count == 0) {
        tadd(&txt, "  no sound device configured; only the PC speaker can sound\n");
    } else {
        for (int card = 0; card < snd.card_count && card < MSDOS_SND_CARDS; card++)
            tadd(&txt, "  %s: %s\n", snd.card[card].name,
                 snd.card[card].decoded ? "FM chip and DSP decoded above" : "listed only, not decoded");
    }
    setText(_soundText, tdone(&txt), NO);
}

/* The list is rebuilt only when the symbols or the filter changed. */
- (void)refreshSymbols
{
    NSString *filter = _symFilter.stringValue ?: @"";
    const int total = msdosdebug_symbols_count(_dbg);
    NSString *sig = [NSString stringWithFormat:@"%d|%u|%d|%@", total, msdosdebug_symbols_generation(_dbg),
                                               _symList.picked, filter];
    if ([sig isEqualToString:_symSig]) return;
    _symSig = sig;

    const int room = total < SYMBOL_LIST_MAX ? total : SYMBOL_LIST_MAX;
    int *index = room > 0 ? malloc((size_t)room * sizeof *index) : NULL;
    const int shown = index ? msdosdebug_symbols_filter(_dbg, trimmed(filter).UTF8String, index, room) : 0;
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    [_symAddrs removeAllObjects];
    for (int idx = 0; idx < shown; idx++) {
        uint32_t linear;
        char name[96];
        name[0] = '\0';
        if (!msdosdebug_symbols_get(_dbg, index[idx], &linear, name, (int)sizeof name)) continue;
        [lines addObject:[NSString stringWithFormat:@"%06X  %@", (unsigned)linear, textFrom(name)]];
        [_symAddrs addObject:@(linear)];
    }
    free(index);
    if (lines.count == 0) [lines addObject:total ? @"(no symbols match)" : @"(no symbols: Load… reads a linker .map)"];
    if (_symList.picked >= (int)_symAddrs.count) _symList.picked = -1;
    setPickText(_symList, lines);
    _symCount.stringValue = [NSString stringWithFormat:@"%d symbol%s%s", (int)_symAddrs.count,
                                                       _symAddrs.count == 1 ? "" : "s",
                                                       trimmed(filter).length > 0 ? " (filtered)" : ""];
}

- (void)refreshInts:(BOOL)force
{
    static msdos_int_event events[TRACE_MAX];
    int total = msdosdebug_int_trace(_dbg, events, TRACE_MAX);
    if (total < 0) total = 0;
    if (total > TRACE_MAX) total = TRACE_MAX;
    const BOOL tracing = msdosdebug_int_trace_enabled(_dbg) != 0;
    _intOn.state = tracing ? NSControlStateValueOn : NSControlStateValueOff;

    /* the vector table: changes rarely, 256 reads, so only for a stopped PC
     * or when asked */
    Text txt = { NULL, 0, 0 };
    if (force || msdosdebug_stopped(_dbg)) {
        for (int vec = 0; vec < 256; vec++) {
            uint16_t seg = 0, off = 0;
            msdosdebug_ivt(_dbg, vec, &seg, &off);
            tadd(&txt, "%02X %04X:%04X%s", vec, (unsigned)seg, (unsigned)off, (vec & 3) == 3 ? "\n" : "   ");
        }
        setText(_ivtText, tdone(&txt), NO);
    }

    /* a log: set again only when it moved */
    if (!force && total == _intCount && (total == 0 || events[total - 1].serial == _intLast)) return;
    _intCount = total;
    _intLast = total > 0 ? events[total - 1].serial : 0;
    const BOOL follow = force || atEnd(_intText);
    tadd(&txt, "From       AX   BX   CX   DX   DS   ES    Call");
    for (int idx = 0; idx < total; idx++)
        tadd(&txt, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", (unsigned)events[idx].cs,
             (unsigned)(events[idx].ip & 0xFFFFu), (unsigned)events[idx].ax, (unsigned)events[idx].bx,
             (unsigned)events[idx].cx, (unsigned)events[idx].dx, (unsigned)events[idx].ds, (unsigned)events[idx].es,
             events[idx].text);
    if (total == 0)
        tadd(&txt, "%s", tracing ? "\n(no INT calls since the trace was cleared)"
                                 : "\n(turn the trace on to record INT calls; it costs speed)");
    setText(_intText, tdone(&txt), follow);
}

- (void)refreshFujinet:(BOOL)force
{
    static msdos_fuji_packet packets[TRACE_MAX];
    static char uart[2048];
    int total = msdosdebug_fujinet_trace(_dbg, packets, TRACE_MAX);
    if (total < 0) total = 0;
    if (total > TRACE_MAX) total = TRACE_MAX;
    const BOOL external = msdossession_fujinet_external(_session) != 0;
    const BOOL fuji = msdossession_fujinet_running(_session) != 0;

    if (external)
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s on COM%d%@.",
                                 msdossession_boip_endpoint(_session),
                                 msdossession_fujinet_com(_session),
                                 msdossession_fujinet_link(_session) ? @", connected" : @", not connected"];
    else if (!fuji)
        _fujiNote.stringValue = @"FujiNet is off (Preferences ▸ FujiNet).";
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on COM%d%@.",
                                 msdossession_fujinet_com(_session),
                                 msdossession_fujinet_link(_session) ? @", connected" : @", not connected"];
    uart[0] = '\0';
    msdosdebug_uart(_dbg, uart, (int)sizeof uart);
    setText(_uartText, textFrom(uart), NO);

    if (!force && total == _fujiCount && (total == 0 || packets[total - 1].serial == _fujiLast)) return;
    _fujiCount = total;
    _fujiLast = total > 0 ? packets[total - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    Text txt = { NULL, 0, 0 };
    for (int idx = 0; idx < total; idx++) {
        tadd(&txt, "%s%s\n     ", idx ? "\n" : "", packets[idx].text);
        for (int pos = 0; pos < packets[idx].nbytes && pos < (int)sizeof packets[idx].data; pos++)
            tadd(&txt, "%02X ", packets[idx].data[pos]);
        if (packets[idx].nbytes > (int)sizeof packets[idx].data) tadd(&txt, "…");
    }
    if (total == 0) tadd(&txt, "(no FujiBus packets since the trace was cleared)");
    setText(_fujiText, tdone(&txt), follow);
}

/* Only the tabs on show: the others refresh when they are selected. A running
 * PC skips the tabs that read its memory or code (they refresh on a stop, a
 * command or when shown), and a PC that is off has only what needs no
 * machine. */
- (void)refreshVisibleForce:(BOOL)force
{
    [self refreshRegs];
    const BOOL running = msdossession_is_running(_session) != 0;
    const BOOL liveRead = !force && running && !msdosdebug_stopped(_dbg);
    for (int slot = 0; slot < TAB_COUNT; slot++) {
        if (![self tabVisible:slot]) continue;
        if (!running && slot != TAB_BREAKS && slot != TAB_WATCHES && slot != TAB_SYMBOLS && slot != TAB_TRACE &&
            slot != TAB_HISTORY)
            continue;
        if (liveRead && (slot == TAB_DISASM || slot == TAB_MEMORY || slot == TAB_MACHINE || slot == TAB_SOUND ||
                         slot == TAB_SYMBOLS))
            continue;
        switch (slot) {
        case TAB_DISASM: [self refreshDisasm]; break;
        case TAB_MEMORY: [self refreshMem]; break;
        case TAB_BREAKS: [self refreshBps]; break;
        case TAB_WATCHES: [self refreshWatches]; break;
        case TAB_MACHINE: [self refreshMachine]; break;
        case TAB_TRACE: [self refreshHist:0 force:force]; break;
        case TAB_HISTORY: [self refreshHist:1 force:force]; break;
        case TAB_VIDEO: [_video refreshForce:force]; break;
        case TAB_SOUND: [self refreshSound]; break;
        case TAB_SYMBOLS: [self refreshSymbols]; break;
        case TAB_INTS: [self refreshInts:force]; break;
        case TAB_FUJINET: [self refreshFujinet:force]; break;
        default: break;
        }
    }
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshVisibleForce:YES];
}

/* After anything that may have moved the engine: the console takes the
 * registers at a new stop, then the views follow. */
- (void)resync
{
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(_dbg, &snap);
    const BOOL stopped = snap.paused != 0 && msdossession_is_running(_session);
    if (stopped && (!_wasStopped || snap.serial != _seenSerial)) [self consoleStop:&snap];
    _wasStopped = stopped;
    _seenSerial = snap.serial;
    _seenGen = msdosdebug_generation(_dbg);
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

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the PC reads as stopped. A longer run (Step
 * Out, Run to Cursor) is picked up by the poll when it stops. */
- (void)settleFrom:(unsigned)gen
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (msdosdebug_generation(_dbg) != gen && msdosdebug_stopped(_dbg)) break;
        usleep(2000);
    }
}

/* An action that moves the machine: the stop settles, then everything shown
 * is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self resync];
}

- (void)attachAndRefresh
{
    const unsigned gen = msdosdebug_generation(_dbg);
    msdosdebug_attach(_dbg);     /* stops the machine */
    _attachedRunning = msdossession_is_running(_session) != 0;
    _wasStopped = NO;
    [self settleFrom:gen];
    [self resync];
}

/* The poll: refresh on a stop (its serial), a resume or a command (the
 * generation). The live tabs follow a running PC twice a second, the Video tab
 * every tick and the Trace tab every 0.4 s. */
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
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(_dbg, &snap);
    const unsigned gen = msdosdebug_generation(_dbg);
    const BOOL stopped = snap.paused != 0;
    if (snap.serial != _seenSerial || gen != _seenGen || stopped != _wasStopped) {
        [self resync];
        return;
    }
    if (stopped) return;
    /* the Video tab follows the beam: every tick */
    if ([self tabVisible:TAB_VIDEO]) [_video refreshForce:NO];
    if ([self tabVisible:TAB_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;
        [self refreshHist:0 force:NO];
    }
    if (++_runningTicks >= LIVE_TICKS) {
        _runningTicks = 0;
        [self refreshStatus];
        [self refreshVisibleForce:NO];
    }
}

/* ---- the console ------------------------------------------------------------------ */

/* Onto the console's transcript, which keeps its last CONSOLE_KEEP
 * characters, whole lines, and follows its end while the reader is there
 * (or always, force). */
- (void)consoleAppend:(NSString *)text force:(BOOL)force
{
    const BOOL follow = force || atEnd(_conOut);
    NSTextStorage *storage = _conOut.textStorage;
    [storage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (storage.length > CONSOLE_KEEP) {
        NSString *all = storage.string;
        NSUInteger cut = all.length - CONSOLE_KEEP * 3 / 4;
        const NSRange newline = [all rangeOfString:@"\n" options:0 range:NSMakeRange(cut, all.length - cut)];
        if (newline.location != NSNotFound) cut = newline.location + 1;
        [storage deleteCharactersInRange:NSMakeRange(0, cut)];
    }
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(storage.length, 0)];
}

/* Command output, ended with a newline. */
- (void)consoleOutput:(const char *)text
{
    NSString *line = textFrom(text);
    if (line.length == 0) return;
    [self consoleAppend:[line hasSuffix:@"\n"] ? line : [line stringByAppendingString:@"\n"] force:NO];
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
        MSDOSDebuggerWindow *owner = weakSelf;
        if (owner && owner->_messageSerial == serial) owner->_message.stringValue = @"";
    });
}

/* At a stop the console shows where, as DEBUG's R does. */
- (void)consoleStop:(const msdos_stop_snapshot *)snap
{
    static char out[4096];
    if (!msdossession_is_running(_session)) return;
    if (snap->text[0]) [self consoleOutput:[[@"— " stringByAppendingString:textFrom(snap->text)] UTF8String]];
    out[0] = '\0';
    msdosdebug_command(_dbg, "r", out, (int)sizeof out);
    [self consoleOutput:out];
}

/* The console's line. Enter and the arrows arrive mid-edit, so it is read
 * and written through the field editor, which keeps the focus. */
- (NSString *)consoleLine
{
    NSText *editor = _conIn.currentEditor;
    return editor ? editor.string : _conIn.stringValue;
}

- (void)setConsoleLine:(NSString *)text
{
    NSText *editor = _conIn.currentEditor;
    if (editor) {
        editor.string = text;
        [editor setSelectedRange:NSMakeRange(text.length, 0)];
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
    const unsigned gen = msdosdebug_generation(_dbg);
    msdosdebug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    [self consoleOutput:out];
    /* a t, p or g that stops again reports where, as resync sees the new stop */
    [self moved:gen];
}

- (void)showHistory
{
    [self setConsoleLine:_historyPos < (NSInteger)_history.count ? _history[(NSUInteger)_historyPos] : @""];
}

/* Tab: the engine takes the whole line and completes its last word. */
- (void)completeConsole
{
    char comps[4096];
    NSString *text = [self consoleLine];
    const NSRange space = [text rangeOfCharacterFromSet:[NSCharacterSet whitespaceCharacterSet]
                                                options:NSBackwardsSearch];
    comps[0] = '\0';
    const int matches = msdosdebug_completions(_dbg, text.UTF8String, comps, (int)sizeof comps);
    NSString *list = textFrom(comps);
    if (matches == 1) {
        NSString *first = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        [self setConsoleLine:[NSString stringWithFormat:@"%@%@ ", head, first]];
    } else if (matches > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self consoleAppend:list force:NO];
    }
}

/* In the console, Enter runs the line, Tab completes it, and Up and Down walk
 * the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(insertNewline:)) {
        [self runConsole];
        return YES;
    }
    if (sel == @selector(insertTab:)) {
        [self completeConsole];
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
    const unsigned gen = msdosdebug_generation(_dbg);
    if (msdosdebug_stopped(_dbg)) msdosdebug_resume(_dbg);
    else msdosdebug_stop(_dbg);
    [self moved:gen];
}

- (void)step:(id)sender
{
    (void)sender;
    const unsigned gen = msdosdebug_generation(_dbg);
    msdosdebug_step(_dbg);
    [self moved:gen];
}

- (void)stepOver:(id)sender
{
    (void)sender;
    const unsigned gen = msdosdebug_generation(_dbg);
    msdosdebug_step_over(_dbg);
    [self moved:gen];
}

- (void)stepOut:(id)sender
{
    (void)sender;
    const unsigned gen = msdosdebug_generation(_dbg);
    msdosdebug_step_out(_dbg);
    [self moved:gen];
}

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to"];
        return;
    }
    const unsigned gen = msdosdebug_generation(_dbg);
    msdosdebug_run_to(_dbg, (uint32_t)_selected);
    [self moved:gen];
}

- (void)toggleBreakpointAt:(uint32_t)linear
{
    if (msdosdebug_bp_toggle(_dbg, linear) < 0) [self message:@"No room for another breakpoint"];
    [self resync];
}

/* F9 */
- (void)toggleSelected:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to toggle its breakpoint"];
        return;
    }
    [self toggleBreakpointAt:(uint32_t)_selected];
}

/* seg:off, a register pair, a linear address, a symbol or an expression. */
- (BOOL)resolveAddress:(NSString *)text seg:(uint16_t *)seg off:(uint32_t *)off linear:(uint32_t *)linear
{
    NSString *clean = trimmed(text ?: @"");
    int64_t value;
    char err[160];
    if (clean.length == 0) return NO;
    if (msdosdebug_parse_address(_dbg, clean.UTF8String, seg, off, linear) == 0) return YES;
    err[0] = '\0';
    if (msdosdebug_eval(_dbg, clean.UTF8String, &value, err, (int)sizeof err) == 0 && value >= 0 &&
        value <= 0xFFFFFFFFLL) {
        *linear = (uint32_t)value;
        segOffFromLinear(*linear, seg, off);
        return YES;
    }
    return NO;
}

/* A bare run of hex digits is hex (the fields show hex); anything else is an
 * expression for the core to evaluate (a symbol, 0x1234, ax + 1). */
- (BOOL)evaluate:(NSString *)text into:(int64_t *)out why:(NSString **)why
{
    NSString *clean = trimmed(text ?: @"");
    const char *chars = clean.UTF8String;
    char err[160];
    if (!chars || !*chars) {
        *why = @"nothing to apply";
        return NO;
    }
    char *end;
    const unsigned long long hex = strtoull(chars, &end, 16);
    if (*end == '\0') {
        *out = (int64_t)hex;
        return YES;
    }
    err[0] = '\0';
    if (msdosdebug_eval(_dbg, chars, out, err, (int)sizeof err) == 0) return YES;
    *why = err[0] ? textFrom(err) : @"does not parse";
    return NO;
}

/* A register field's text applied through msdosdebug_register_set. */
- (void)applyRegister:(NSTextField *)sender
{
    const NSInteger tag = sender.tag;
    const char *name;
    char full[8], err[160];
    int64_t value = 0;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
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
    err[0] = '\0';
    if (!msdosdebug_stopped(_dbg))
        [self message:@"Stop the PC (F5) to edit registers"];
    else if (![self evaluate:sender.stringValue into:&value why:&why])
        [self message:[NSString stringWithFormat:@"Register: %@", why]];
    else if (!msdosdebug_register_set(_dbg, name, value, err, (int)sizeof err))
        [self message:[NSString stringWithFormat:@"Register: %@", err[0] ? textFrom(err) : @"refused"]];
    [self resync];
}

- (void)flagClicked:(NSButton *)sender
{
    const NSInteger idx = sender.tag;
    char err[160];
    err[0] = '\0';
    if (idx < 0 || idx >= NFLAGS) return;
    if (msdosdebug_stopped(_dbg))
        msdosdebug_register_set(_dbg, kFlags[idx].name, sender.state == NSControlStateValueOn ? 1 : 0, err,
                                (int)sizeof err);
    else
        [self message:@"Stop the PC (F5) to edit flags"];
    [self resync];
}

/* Shows a range in the Memory tab, marked, a couple of rows down from the top. */
- (void)showInMemoryFrom:(uint32_t)first last:(uint32_t)last
{
    _memTop = (first & ~0xFu) > 2 * 16 ? (first & ~0xFu) - 2 * 16 : 0;
    _memHl = YES;
    _memHlLo = first;
    _memHlHi = last;
    [self refreshMem];
}

- (void)selectTab:(int)tab
{
    if (_popped[tab]) [_popped[tab] makeKeyAndOrderFront:nil];
    else [_tabs selectTabViewItem:_items[tab]];
}

/* The Disassembly at an address, following off. */
- (void)showInDisassembly:(uint32_t)linear
{
    uint16_t seg;
    uint32_t off;
    segOffFromLinear(linear, &seg, &off);
    _followPc.state = NSControlStateValueOff;
    _topSeg = seg;
    _topOff = off;
    _selected = linear;
    [self selectTab:TAB_DISASM];
    [self refreshDisasm];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint32_t first, last;
    if (!msdossym_parse_range(_dbg, trimmed(_memGoto.stringValue).UTF8String, &first, &last)) {
        _memHl = NO;
        [self refreshMem];
        [self message:[NSString stringWithFormat:@"Not an address (seg:off, ds:si, a linear address, a symbol): %@",
                                                 _memGoto.stringValue]];
        return;
    }
    [self showInMemoryFrom:first last:last];
    [self scrollMemToLine:1 + (int)((first - _memTop) / 16)];
}

- (void)memPrev:(id)sender
{
    (void)sender;
    _memTop = _memTop > MEM_BYTES ? _memTop - MEM_BYTES : 0;
    [self refreshMem];
}

- (void)memNext:(id)sender
{
    (void)sender;
    _memTop += MEM_BYTES;
    [self refreshMem];
}

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@" \t,"]];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    uint16_t seg;
    uint32_t off, address = 0;
    for (NSString *piece in words)
        if (piece.length) [tokens addObject:piece];
    const BOOL haveAddress = tokens.count > 0 && [self resolveAddress:tokens[0] seg:&seg off:&off linear:&address];
    if (tokens.count < 2 || !haveAddress || tokens.count > POKE_MAX + 1) {
        _memNote.stringValue = @"Poke: address value [value...]";
        return;
    }
    if (!msdosdebug_stopped(_dbg)) {
        _memNote.stringValue = @"Stop the PC (F5) to poke it";
        return;
    }
    uint8_t bytes[POKE_MAX];
    for (NSUInteger idx = 1; idx < tokens.count; idx++) {
        long value;
        if (!parseNum(tokens[idx], &value) || value < 0 || value > 0xFF) {
            _memNote.stringValue = [NSString stringWithFormat:@"Not a byte: %@", tokens[idx]];
            return;
        }
        bytes[idx - 1] = (uint8_t)value;
    }
    msdosdebug_write(_dbg, address, bytes, (int)tokens.count - 1);
    _memWrite.stringValue = @"";
    _memNote.stringValue = [NSString stringWithFormat:@"Wrote %d byte%s at %06X (RAM takes it; ROM ignores it)",
                            (int)tokens.count - 1, tokens.count == 2 ? "" : "s", (unsigned)address];
    if (address < _memTop || address >= _memTop + MEM_BYTES)
        [self showInMemoryFrom:address last:address + (uint32_t)tokens.count - 2];
    else [self refreshMem];
}

- (void)searchBackward:(BOOL)backward
{
    uint8_t pat[MSDOSSEARCH_MAX], mask = 0xFF;
    int count = 0, flags = 0;
    uint32_t found;
    if (!msdossearch_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String, pat, &count, &mask,
                           &flags) || count < 1) {
        _srchNote.stringValue = @"Type hex bytes (B8 34 12) or some text.";
        return;
    }
    if (!msdosdebug_stopped(_dbg)) {
        _srchNote.stringValue = @"Stop the PC (F5) to search its memory.";
        return;
    }
    const uint32_t from = _memHl ? (backward ? _memHlLo : _memHlLo + 1) : _memTop;
    const int hit = msdosdebug_search_next(_dbg, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at %06X", (unsigned)found];
        [self showInMemoryFrom:found last:found + (uint32_t)count - 1];
        [self scrollMemToLine:1 + (int)((found - _memTop) / 16)];
    } else {
        _srchNote.stringValue = hit == 0 ? @"Not found." : @"Nothing to search (is the PC running?).";
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

- (void)followToggled:(id)sender
{
    (void)sender;
    if (msdossession_is_running(_session)) [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    uint16_t seg;
    uint32_t off, linear;
    if (![self resolveAddress:_jump.stringValue seg:&seg off:&off linear:&linear]) {
        [self message:@"Not an address (seg:off, cs:ip, a linear address, a symbol)"];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _topSeg = seg;
    _topOff = off;
    _selected = linear;
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

/* A row's condition field, Enter: replaces the breakpoint's condition. */
- (void)bpCondEdited:(NSTextField *)sender
{
    char err[256];
    err[0] = '\0';
    if (!msdosdebug_bp_set_condition(_dbg, (int)sender.tag, trimmed(sender.stringValue).UTF8String, err,
                                     (int)sizeof err)) {
        _bpMsg.stringValue = err[0] ? textFrom(err) : @"The condition was refused";
        _bpSig = nil;           /* the field goes back to what the engine holds */
    } else {
        _bpMsg.stringValue = @"";
    }
    [_window makeFirstResponder:nil];
    [self resyncLater];
}

- (void)bpClear:(id)sender
{
    (void)sender;
    msdosdebug_bp_clear(_dbg);
    [self resync];
}

- (void)bpClearHits:(id)sender
{
    (void)sender;
    msdosdebug_bp_clear_hits(_dbg);
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

/* Execute and memory breakpoints take addresses; port and INT ones numbers.
 * The end is optional; AH only means something for an INT; the condition is
 * an expression. */
- (void)bpAdd:(id)sender
{
    (void)sender;
    NSInteger type = _bpType.indexOfSelectedItem;
    NSString *start = trimmed(_bpStart.stringValue);
    NSString *end = trimmed(_bpEnd.stringValue);
    NSString *ahText = trimmed(_bpAh.stringValue);
    NSString *why = nil;
    uint32_t first, last;
    int ahValue = -1;
    if (type < 0 || type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    if (start.length == 0) {
        _bpMsg.stringValue = type <= MSDOS_BP_ACCESS ? @"Give an address" : type == MSDOS_BP_INT ? @"Give an interrupt"
                                                                                                 : @"Give a port";
        return;
    }
    if (type <= MSDOS_BP_ACCESS) {
        uint16_t seg;
        uint32_t off;
        if (![self resolveAddress:start seg:&seg off:&off linear:&first]) {
            _bpMsg.stringValue = @"Not an address (seg:off, a linear address, a symbol)";
            return;
        }
        last = first;
        if (end.length && ![self resolveAddress:end seg:&seg off:&off linear:&last]) {
            _bpMsg.stringValue = @"The end is not an address";
            return;
        }
    } else {
        int64_t from, upto;
        const int64_t limit = type == MSDOS_BP_INT ? 0xFF : 0xFFFF;
        if (![self evaluate:start into:&from why:&why] || from < 0 || from > limit) {
            _bpMsg.stringValue = type == MSDOS_BP_INT ? @"An interrupt is 00-FF" : @"A port is 0000-FFFF";
            return;
        }
        upto = from;
        if (end.length && type != MSDOS_BP_INT && (![self evaluate:end into:&upto why:&why] || upto < from || upto > 0xFFFF)) {
            _bpMsg.stringValue = @"The last port is a port at or after the first";
            return;
        }
        first = (uint32_t)from;
        last = (uint32_t)upto;
        if (type == MSDOS_BP_INT && ahText.length) {
            int64_t ahWide;
            if (![self evaluate:ahText into:&ahWide why:&why] || ahWide < 0 || ahWide > 0xFF) {
                _bpMsg.stringValue = @"AH is 00-FF, or empty for any";
                return;
            }
            ahValue = (int)ahWide;
        }
    }
    if (last < first) {
        _bpMsg.stringValue = @"The end is before the start";
        return;
    }
    char err[256];
    err[0] = '\0';
    NSString *condition = trimmed(_bpCond.stringValue);
    if (msdosdebug_bp_add_if(_dbg, (int)type, first, last, ahValue, condition.length ? condition.UTF8String : NULL, err,
                             (int)sizeof err) < 0) {
        _bpMsg.stringValue = err[0] ? textFrom(err) : @"The breakpoint was refused";
        return;
    }
    _bpMsg.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpAh.stringValue = @"";
    _bpCond.stringValue = @"";
    [self resync];
}

- (void)addWatch:(id)sender
{
    (void)sender;
    if (msdoswatch_add(trimmed(_wtEntry.stringValue).UTF8String) < 0) {
        [self message:@"Type an address, symbol, expression or low-high range to watch"];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
}

- (void)wtMemory:(id)sender
{
    (void)sender;
    uint32_t first, last;
    if (_wtList.picked >= 0 && msdoswatch_range(_dbg, _wtList.picked, &first, &last)) {
        [self selectTab:TAB_MEMORY];
        [self showInMemoryFrom:first last:last];
        [self scrollMemToLine:1 + (int)((first - _memTop) / 16)];
    }
}

- (void)wtBreak:(id)sender
{
    (void)sender;
    uint32_t first, last;
    if (_wtList.picked < 0 || !msdoswatch_range(_dbg, _wtList.picked, &first, &last)) {
        [self message:@"Pick a watch first (click its line)"];
        return;
    }
    [self breakOnChangeFrom:first to:last];
}

/* A write breakpoint on first..last that fires when the value written differs
 * from the old one (the core's `value != old`). */
- (void)breakOnChangeFrom:(uint32_t)first to:(uint32_t)last
{
    char err[160];
    err[0] = '\0';
    if (msdosdebug_bp_add_if(_dbg, MSDOS_BP_WRITE, first, last, -1, "value != old", err, (int)sizeof err) >= 0)
        [self message:@"Breaking when it changes"];
    else
        [self message:err[0] ? textFrom(err) : @"The breakpoint was refused"];
    [self resync];
}

- (void)wtRemove:(id)sender
{
    (void)sender;
    if (_wtList.picked >= 0) {
        msdoswatch_remove(_wtList.picked);
        _wtList.picked = -1;
        [self refreshWatches];
    }
}

- (void)histRec:(NSButton *)sender
{
    const int enabled = sender.state == NSControlStateValueOn;
    if (sender.tag == 0) msdosdebug_trace_enable(_dbg, enabled);
    else msdosdebug_history_enable(_dbg, enabled);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self resync];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshHist:0 force:YES];
    [self refreshHist:1 force:YES];
}

- (void)histClear:(id)sender
{
    (void)sender;
    msdosdebug_trace_clear(_dbg);
    msdosdebug_history_clear(_dbg);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self resync];
}

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    _symList.picked = -1;
    [self refreshSymbols];
}

/* Breakpoint on the picked symbol's address. */
- (void)symBreak:(id)sender
{
    (void)sender;
    char err[160];
    err[0] = '\0';
    if (_symList.picked < 0 || _symList.picked >= (int)_symAddrs.count) {
        [self message:@"Pick a symbol first (click its line)"];
        return;
    }
    const uint32_t linear = _symAddrs[(NSUInteger)_symList.picked].unsignedIntValue;
    if (msdosdebug_bp_add_if(_dbg, MSDOS_BP_EXEC, linear, linear, -1, NULL, err, (int)sizeof err) >= 0)
        [self message:[NSString stringWithFormat:@"Breakpoint at %06X", (unsigned)linear]];
    else
        [self message:err[0] ? textFrom(err) : @"The breakpoint was refused"];
    [self resync];
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    long seg = 0;
    if (!parseNum(_symSeg.stringValue, &seg) || seg < 0 || seg > 0xFFFF) seg = 0;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Symbols";
    panel.message = @"An MS LINK or Watcom WLINK .map file";
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    char msg[512];
    msg[0] = '\0';
    if (msdosdebug_load_symbols(_dbg, panel.URL.fileSystemRepresentation, (uint16_t)seg, msg, (int)sizeof msg) < 0 &&
        !msg[0])
        snprintf(msg, sizeof msg, "No symbols found");
    [self say:textFrom(msg)];
    [self resync];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    msdosdebug_clear_symbols(_dbg);
    [self say:@"Symbols cleared"];
    [self resync];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [panel setNameFieldStringValue:kFileNames[kind]];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    char msg[512];
    msg[0] = '\0';
    msdosdebug_save(_dbg, kFileKinds[kind], panel.URL.fileSystemRepresentation, msg, (int)sizeof msg);
    [self say:textFrom(msg)];
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

/* ---- tabs in windows of their own --------------------------------------------------- */

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (_tabs.selectedTabViewItem == _items[slot]) {
            [self popOutTab:slot];
            return;
        }
}

- (void)popOutTab:(int)tab
{
    if (_popped[tab] || _tabs.numberOfTabViewItems <= 1) return;     /* one tab always stays */
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [NSString stringWithFormat:@"Debugger – %s", kTabs[tab].label];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    MSDOSSetHelpContext(win, [@"debugger/" stringByAppendingString:@(kTabs[tab].name)]);
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSStackView *root = vstack(@[dockBtn, view]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [view.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [view.heightAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;
    win.contentView = root;
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self refreshVisibleForce:YES];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

- (void)dockTab:(int)tab
{
    NSWindow *win = _popped[tab];
    if (!win) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int pos = 0;
    _popped[tab] = nil;
    [view removeFromSuperview];
    item.view = view;
    for (int slot = 0; slot < tab; slot++)   /* back where it was in the order */
        if (!_popped[slot]) pos++;
    [_tabs insertTabViewItem:item atIndex:pos];
    [_tabs selectTabViewItem:item];
    win.delegate = nil;
    [win close];
    [self refreshVisibleForce:YES];
}
@end
