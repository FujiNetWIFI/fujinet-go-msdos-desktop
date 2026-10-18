/*
 * MSDOSControllersWindow -- the AppKit Controllers panel: the machine's own
 * keyboard on screen (drawn from msdossession_keyboard_layout, so every
 * frontend shows the same keys for the machine -- the PCjr's, the XT's
 * 83 keys, the AT's 101 -- the keys a desktop keeps for itself, SysRq,
 * Pause and Scroll Lock, included), the lock lights, the game port's two
 * joysticks with their four buttons, and the gamepads and the joystick
 * each drives.
 *
 * Keys are caps (MSDOSCap) that act on mouseDown AND mouseUp: a cap is HELD
 * while the mouse is down on it, exactly like the real key, so the PC sees
 * a make and a break with the time between them; dragging off lets it go.
 * What the PC holds -- host keyboard, gamepads and this panel -- is filled
 * with the accent (the badge's ink on it), so the panel doubles as an input
 * tester. Keys typed with the panel key go where the main window's go (its
 * MSDOSKeyRouter); no view here takes the keyboard, so the panel itself is
 * the first responder.
 *
 * A joystick is dragged inside its square and springs back to the middle
 * when let go, as a real one does.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents.
 * Only a machine change resizes it (the keyboard is rebuilt; the top edge
 * stays put). Hidden at start, ordered out rather than closed, so its
 * position survives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#include <math.h>
#include <string.h>

#define UNIT          34.0  /* points per key unit */
#define KBD_GAP        3.0  /* between key caps */
#define MAX_KEYS     160
#define STICK        120.0  /* a joystick's square */
#define STICK_BOX_W  150.0
#define STICKS_GAP    32.0
#define BTN_W         28.0
#define BTN_H         24.0
#define BTN_GAP        4.0
#define TITLE_H       18.0
#define STICKS_H     (TITLE_H + 6 + STICK + 6 + BTN_H)
#define MARGIN        12.0
#define PAD_ROW_H     28.0
#define POPUP_W      150.0
#define MAX_PAD_ROWS   4
#define MIN_W        380.0

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* Points from key units, rounded so neighbouring caps keep an even gap. */
static CGFloat px(float units)
{
    return floor(units * UNIT + 0.5);
}

/* Lines of text centred in r, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect r, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat nlines = (CGFloat)lines.count;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat w = 0;
        lineH = 0;
        for (NSString *l in lines) {
            const NSSize s = [l sizeWithAttributes:attrs];
            if (s.width > w) w = s.width;
            if (s.height > lineH) lineH = s.height;
        }
        if ((w <= r.size.width && lineH * nlines <= r.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat y = r.origin.y + floor((r.size.height - lineH * nlines) / 2);
    for (NSString *l in lines) {
        [l drawInRect:NSMakeRect(r.origin.x, y, r.size.width, lineH) withAttributes:attrs];
        y += lineH;
    }
}

/* ---- views ------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' tables. */
@interface MSDOSCtlFlippedView : NSView
@end

@implementation MSDOSCtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

@class MSDOSCap;
@class MSDOSStickView;

@interface MSDOSControllersWindow ()
- (void)capPressed:(MSDOSCap *)cap down:(BOOL)down;
- (void)stick:(MSDOSStickView *)view dragTo:(NSPoint)p;
- (void)stickLetGo:(MSDOSStickView *)view;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

/* A key cap or a joystick button: drawn here, so it is exactly the size
 * given and lit as a fill. */
@interface MSDOSCap : NSView
@property (nonatomic, weak) MSDOSControllersWindow *owner;
@property (nonatomic, copy) NSString *face;
@property (nonatomic, copy) NSString *shifted;  /* the key's shifted legend, or nil */
@property (nonatomic) int kbdIndex;             /* on-screen keyboard index, or -1 */
@property (nonatomic) int stick;                /* a joystick button: its stick ... */
@property (nonatomic) int button;               /* ... and bit, or -1 */
@property (nonatomic) BOOL keyCap;              /* a key of the keyboard: smaller, bold */
@property (nonatomic) BOOL lit;
@property (nonatomic) BOOL greyed;
@property (nonatomic) BOOL down;                /* the mouse holds it */
@end

@implementation MSDOSCap

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _kbdIndex = -1;
    _stick = -1;
    _button = -1;
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)setLit:(BOOL)lit
{
    if (lit == _lit) return;
    _lit = lit;
    [self setNeedsDisplay:YES];
}

- (void)setGreyed:(BOOL)greyed
{
    if (greyed == _greyed) return;
    _greyed = greyed;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(b, 0.5, 0.5) xRadius:4 yRadius:4];
    NSColor *ink;
    if (self.lit) {
        /* the badge's silver, its ink on it */
        [MSDOSAccentColor() setFill];
        [path fill];
        ink = MSDOSInkColor();
    } else if (self.greyed) {
        ink = [NSColor tertiaryLabelColor];
    } else {
        [[NSColor controlColor] setFill];
        [path fill];
        ink = [NSColor labelColor];
    }
    [[NSColor separatorColor] setStroke];
    [path stroke];

    const NSRect inner = NSInsetRect(b, 2, 2);
    if (self.shifted.length) {
        const CGFloat half = floor(inner.size.height / 2);
        drawFitted(self.shifted, NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, half), 9, YES, ink);
        drawFitted(self.face, NSMakeRect(inner.origin.x, inner.origin.y + half, inner.size.width,
                                         inner.size.height - half), 9, YES, ink);
    } else if (self.face.length) {
        drawFitted(self.face, inner, self.keyCap ? 10 : 12, self.keyCap, ink);
    }
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (self.down || self.greyed) return;
    self.down = YES;
    [self.owner capPressed:self down:YES];
}

/* Dragging off a cap lets it go. */
- (void)mouseDragged:(NSEvent *)e
{
    if (!self.down) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    if (NSPointInRect(p, [self bounds])) return;
    self.down = NO;
    [self.owner capPressed:self down:NO];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self.owner capPressed:self down:NO];
}
@end

/* A joystick: its square, the cross, and the knob where the stick is. */
@interface MSDOSStickView : NSView
@property (nonatomic, weak) MSDOSControllersWindow *owner;
@property (nonatomic) int stick;
@property (nonatomic) int x, y;                 /* -32768..32767 */
@property (nonatomic) BOOL dragging;
@property (nonatomic) BOOL usable;              /* a joystick is on the game port */
@end

@implementation MSDOSStickView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    const CGFloat w = b.size.width, h = b.size.height;
    const CGFloat cx = w / 2.0 + self.x / 32768.0 * (w / 2.0 - 12);
    const CGFloat cy = h / 2.0 + self.y / 32768.0 * (h / 2.0 - 12);
    const CGFloat alpha = self.usable ? 1.0 : 0.4;
    NSColor *fg = [NSColor labelColor];

    [[fg colorWithAlphaComponent:0.15 * alpha] setFill];
    NSRectFill(NSInsetRect(b, 0.5, 0.5));
    NSBezierPath *cross = [NSBezierPath bezierPath];
    [cross moveToPoint:NSMakePoint(w / 2.0, 0)];
    [cross lineToPoint:NSMakePoint(w / 2.0, h)];
    [cross moveToPoint:NSMakePoint(0, h / 2.0)];
    [cross lineToPoint:NSMakePoint(w, h / 2.0)];
    [cross setLineWidth:1];
    [[fg colorWithAlphaComponent:0.4 * alpha] setStroke];
    [cross stroke];

    NSBezierPath *knob = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - 10, cy - 10, 20, 20)];
    [[MSDOSAccentColor() colorWithAlphaComponent:alpha] setFill];
    [knob fill];
    [[fg colorWithAlphaComponent:0.8 * alpha] setStroke];
    [knob stroke];
}

- (void)mouseDown:(NSEvent *)e
{
    if (!self.usable) return;
    self.dragging = YES;
    [self.owner stick:self dragTo:[self convertPoint:[e locationInWindow] fromView:nil]];
}

- (void)mouseDragged:(NSEvent *)e
{
    if (!self.dragging) return;
    [self.owner stick:self dragTo:[self convertPoint:[e locationInWindow] fromView:nil]];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.dragging) return;
    self.dragging = NO;
    [self.owner stickLetGo:self];
}
@end

/* Keys typed with the panel key go to its owner. */
@interface MSDOSControllersPanel : NSPanel
@property (nonatomic, weak) MSDOSControllersWindow *owner;
@end

@implementation MSDOSControllersPanel
- (void)keyDown:(NSEvent *)e { [self.owner panelKeyDown:e]; }
- (void)keyUp:(NSEvent *)e { [self.owner panelKeyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [self.owner panelFlagsChanged:e]; }
/* Escape is the PC's Esc, not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------ */

@interface MSDOSControllersWindow () <NSWindowDelegate>
@end

@implementation MSDOSControllersWindow {
    msdossession *_session;
    __weak id<MSDOSKeyRouter> _router;
    MSDOSControllersPanel *_panel;
    MSDOSCtlFlippedView *_kbdView;
    MSDOSCtlFlippedView *_sticksView;
    NSMutableArray<MSDOSCap *> *_keys;
    MSDOSStickView *_stickView[MSDOS_JOYSTICKS];
    MSDOSCap *_stickButton[MSDOS_JOYSTICKS][4];
    unsigned _held[MSDOS_JOYSTICKS];        /* the buttons the mouse holds here */
    NSTextField *_lights;
    NSTextField *_stickNote;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    unsigned _padGeneration;
    unsigned _lastLights;
    NSTimer *_timer;
    NSSize _kbdSize;
    int _builtMachine;
    int _mouseKey;                          /* the key the mouse holds, or -1 */
    BOOL _placed;
}

- (instancetype)initWithSession:(msdossession *)session router:(id<MSDOSKeyRouter>)router
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _router = router;
    _mouseKey = -1;
    _builtMachine = -1;
    _lastLights = ~0u;
    _keys = [NSMutableArray array];

    _panel = [[MSDOSControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 640, 520)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the PC's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    [_panel setHidesOnDeactivate:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    MSDOSCtlFlippedView *content = [[MSDOSCtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 640, 520)];
    [content setAutoresizesSubviews:NO];
    [_panel setContentView:content];

    _kbdView = [[MSDOSCtlFlippedView alloc] initWithFrame:NSZeroRect];
    [content addSubview:_kbdView];

    _lights = [NSTextField labelWithString:@""];
    [_lights setTextColor:[NSColor secondaryLabelColor]];
    [_lights setAlignment:NSTextAlignmentCenter];
    [content addSubview:_lights];

    /* the two joysticks, side by side */
    _sticksView = [[MSDOSCtlFlippedView alloc] initWithFrame:NSZeroRect];
    [content addSubview:_sticksView];
    for (int s = 0; s < MSDOS_JOYSTICKS; s++) {
        const CGFloat x0 = s * (STICK_BOX_W + STICKS_GAP);
        NSTextField *title = [NSTextField labelWithString:s ? @"Joystick B" : @"Joystick A"];
        [title setFont:[NSFont boldSystemFontOfSize:12]];
        [title setAlignment:NSTextAlignmentCenter];
        [title setFrame:NSMakeRect(x0, 0, STICK_BOX_W, TITLE_H)];
        [_sticksView addSubview:title];
        MSDOSStickView *v = [[MSDOSStickView alloc]
            initWithFrame:NSMakeRect(x0 + floor((STICK_BOX_W - STICK) / 2), TITLE_H + 6, STICK, STICK)];
        v.owner = self;
        v.stick = s;
        v.usable = YES;
        [_sticksView addSubview:v];
        _stickView[s] = v;
        const CGFloat bw = 4 * BTN_W + 3 * BTN_GAP;
        const CGFloat bx = x0 + floor((STICK_BOX_W - bw) / 2);
        for (int k = 0; k < 4; k++) {
            MSDOSCap *c = [[MSDOSCap alloc] initWithFrame:NSMakeRect(bx + k * (BTN_W + BTN_GAP),
                                                                     TITLE_H + 6 + STICK + 6, BTN_W, BTN_H)];
            c.owner = self;
            c.face = [NSString stringWithFormat:@"%d", k + 1];
            c.stick = s;
            c.button = k;
            [_sticksView addSubview:c];
            _stickButton[s][k] = c;
        }
    }
    _stickNote = [NSTextField labelWithString:@""];
    [_stickNote setTextColor:[NSColor secondaryLabelColor]];
    [_stickNote setAlignment:NSTextAlignmentCenter];
    [content addSubview:_stickNote];

    /* Gamepads: one row per pad, its name and the joystick it is set to
     * drive; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads. One plugged in drives Joystick A, the next B."];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [content addSubview:_padName[i]];
        /* 0 = automatic (connection order), 1-2 = that joystick */
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Joystick A", @"Joystick B"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [content addSubview:_padAssign[i]];
    }
    /* the keyboard and the rest are built when first shown */
    return self;
}

- (void)dealloc
{
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- building ---------------------------------------------------------------------- */

- (void)buildKeyboard
{
    const msdos_kbd_key *keys = NULL;
    const int n = msdossession_keyboard_layout(_session, &keys);
    float ew = 0, eh = 0;

    [_kbdView setSubviews:@[]];
    [_keys removeAllObjects];
    for (int i = 0; keys && i < n && i < MAX_KEYS; i++) {
        const msdos_kbd_key *k = &keys[i];
        const CGFloat x0 = px(k->x), y0 = px(k->y);
        const CGFloat x1 = px(k->x + k->w), y1 = px(k->y + k->h);
        MSDOSCap *c = [[MSDOSCap alloc] initWithFrame:NSMakeRect(x0 + KBD_GAP / 2, y0 + KBD_GAP / 2,
                                                                 x1 - x0 - KBD_GAP, y1 - y0 - KBD_GAP)];
        c.owner = self;
        c.face = nsstr(k->label);
        c.shifted = k->shifted ? nsstr(k->shifted) : nil;
        c.kbdIndex = i;
        c.keyCap = YES;
        [_kbdView addSubview:c];
        [_keys addObject:c];
        if (k->x + k->w > ew) ew = k->x + k->w;
        if (k->y + k->h > eh) eh = k->y + k->h;
    }
    _kbdSize = NSMakeSize(px(ew), px(eh));
    _builtMachine = msdossession_machine(_session);
}

- (CGFloat)innerWidth
{
    return MAX(MAX(_kbdSize.width, 2 * STICK_BOX_W + STICKS_GAP), MIN_W);
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const CGFloat w = [self innerWidth];
    const CGFloat sticksW = 2 * STICK_BOX_W + STICKS_GAP;
    CGFloat y = MARGIN;

    [_kbdView setFrame:NSMakeRect(MARGIN + floor((w - _kbdSize.width) / 2), y, _kbdSize.width, _kbdSize.height)];
    y += _kbdSize.height + 8;
    [_lights setFrame:NSMakeRect(MARGIN, y, w, 18)];
    y += 18 + 10;
    [_sticksView setFrame:NSMakeRect(MARGIN + floor((w - sticksW) / 2), y, sticksW, STICKS_H)];
    y += STICKS_H + 4;
    [_stickNote setFrame:NSMakeRect(MARGIN, y, w, 18)];
    y += 18 + 10;
    [_padsTitle setFrame:NSMakeRect(MARGIN, y, 200, 16)];
    y += TITLE_H + 4;
    [_noPads setFrame:NSMakeRect(MARGIN, y + 4, w, 18)];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat ry = y + i * PAD_ROW_H;
        [_padName[i] setFrame:NSMakeRect(MARGIN, ry + 4, w - POPUP_W - 8, 18)];
        [_padAssign[i] setFrame:NSMakeRect(MARGIN + w - POPUP_W, ry, POPUP_W, 25)];
    }
    y += MAX_PAD_ROWS * PAD_ROW_H + MARGIN;
    [self fitPanel:NSMakeSize(MARGIN + w + MARGIN, y)];
}

- (void)rebuild
{
    const BOOL fitted = msdossession_joystick_fitted(_session) != 0;
    [self letGo];
    [self buildKeyboard];
    for (int s = 0; s < MSDOS_JOYSTICKS; s++) {
        _stickView[s].usable = fitted;
        [_stickView[s] setNeedsDisplay:YES];
        for (int k = 0; k < 4; k++) _stickButton[s][k].greyed = !fitted;
    }
    _stickNote.stringValue = fitted ? @"" : @"No joystick on the game port (Preferences ▸ Ports)";
    _stickNote.hidden = fitted;
    _lastLights = ~0u;
    [self relayout];
    [self refreshPads];
    [self tick];
}

/* ---- pressing ------------------------------------------------------------------- */

- (void)sendStick:(int)s
{
    msdossession_joystick(_session, s, _stickView[s].x, _stickView[s].y, _held[s]);
}

/* Lets go of everything the mouse holds in this panel. */
- (void)letGo
{
    if (_mouseKey >= 0) {
        msdossession_keyboard_press(_session, _mouseKey, 0);
        _mouseKey = -1;
    }
    for (MSDOSCap *c in _keys) c.down = NO;
    for (int s = 0; s < MSDOS_JOYSTICKS; s++) {
        BOOL changed = _held[s] != 0;
        for (int k = 0; k < 4; k++) _stickButton[s][k].down = NO;
        _held[s] = 0;
        if (_stickView[s].dragging) {
            _stickView[s].dragging = NO;
            _stickView[s].x = _stickView[s].y = 0;
            [_stickView[s] setNeedsDisplay:YES];
            changed = YES;
        }
        if (changed) [self sendStick:s];
    }
}

- (void)capPressed:(MSDOSCap *)c down:(BOOL)down
{
    if (c.kbdIndex >= 0) {
        if (down) {
            if (_mouseKey >= 0 && _mouseKey != c.kbdIndex) msdossession_keyboard_press(_session, _mouseKey, 0);
            _mouseKey = c.kbdIndex;
            msdossession_keyboard_press(_session, c.kbdIndex, 1);
            c.lit = YES;
        } else if (_mouseKey == c.kbdIndex) {
            _mouseKey = -1;
            msdossession_keyboard_press(_session, c.kbdIndex, 0);
        }
    } else if (c.stick >= 0 && c.stick < MSDOS_JOYSTICKS && c.button >= 0 && c.button < 4) {
        const unsigned bit = 1u << c.button;
        if (down) _held[c.stick] |= bit;
        else _held[c.stick] &= ~bit;
        [self sendStick:c.stick];
        c.lit = down;
    }
}

- (void)stick:(MSDOSStickView *)v dragTo:(NSPoint)p
{
    const NSRect b = [v bounds];
    const double hw = b.size.width / 2.0, hh = b.size.height / 2.0;
    double x = (p.x - hw) / hw * 32767.0, y = (p.y - hh) / hh * 32767.0;
    if (x < -32768) x = -32768;
    if (x > 32767) x = 32767;
    if (y < -32768) y = -32768;
    if (y > 32767) y = 32767;
    v.x = (int)x;
    v.y = (int)y;
    [self sendStick:v.stick];
    [v setNeedsDisplay:YES];
}

/* A joystick springs back to the middle. */
- (void)stickLetGo:(MSDOSStickView *)v
{
    v.x = v.y = 0;
    [self sendStick:v.stick];
    [v setNeedsDisplay:YES];
}

/* ---- gamepads ------------------------------------------------------------------- */

- (void)refreshPads
{
    const int n = msdossession_gamepad_count(_session);
    const int rows = n < MAX_PAD_ROWS ? (n > 0 ? n : 0) : MAX_PAD_ROWS;
    _padGeneration = msdossession_gamepad_generation(_session);
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < rows;
        _padName[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        msdossession_gamepad_name(_session, i, name, (int)sizeof name);
        _padName[i].stringValue = nsstr(name);
        _padName[i].toolTip = nsstr(name);
        const int a = msdossession_gamepad_assignment(_session, i);
        [_padAssign[i] selectItemAtIndex:(a < 0 || a >= MSDOS_JOYSTICKS) ? 0 : a + 1];
    }
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    msdossession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
}

/* 20 Hz while shown: what the PC holds lit, the lock lights, the sticks as
 * the PC sees them (gamepads too), hot-plugged pads, a machine change. */
- (void)tick
{
    if (!_session) return;
    for (MSDOSCap *c in _keys)
        c.lit = c.kbdIndex == _mouseKey || msdossession_keyboard_held(_session, c.kbdIndex);

    const unsigned lights = msdossession_lock_lights(_session);
    if (lights != _lastLights) {
        _lastLights = lights;
        _lights.stringValue = [NSString stringWithFormat:@"%@ Num Lock     %@ Caps Lock     %@ Scroll Lock",
                               (lights & 1) ? @"●" : @"○", (lights & 2) ? @"●" : @"○",
                               (lights & 4) ? @"●" : @"○"];
    }

    for (int s = 0; s < MSDOS_JOYSTICKS; s++) {
        int x = 0, y = 0;
        unsigned b = 0;
        msdossession_joystick_state(_session, s, &x, &y, &b);
        MSDOSStickView *v = _stickView[s];
        if (!v.dragging && (x != v.x || y != v.y)) {
            v.x = x;
            v.y = y;
            [v setNeedsDisplay:YES];
        }
        for (int k = 0; k < 4; k++) _stickButton[s][k].lit = ((b | _held[s]) >> k) & 1u;
    }

    if (msdossession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
    if (msdossession_machine(_session) != _builtMachine) [self rebuild];
}

/* ---- the keyboard: the main window's routing ------------------------------------- */

- (void)panelKeyDown:(NSEvent *)e { (void)[_router routeKey:e down:YES]; }
- (void)panelKeyUp:(NSEvent *)e { (void)[_router routeKey:e down:NO]; }
- (void)panelFlagsChanged:(NSEvent *)e { [_router routeFlags:e]; }

/* ---- the window ----------------------------------------------------------------- */

- (void)show
{
    if (msdossession_machine(_session) != _builtMachine) [self rebuild];
    [self refreshPads];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    if (!_timer) {
        __weak MSDOSControllersWindow *weakSelf = self;
        _timer = [NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf tick];
        }];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    [self tick];
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
}

/* Hidden: nothing held, no polling. */
- (void)quiesce
{
    [self letGo];
    [_timer invalidate];
    _timer = nil;
}

- (void)hide
{
    [self quiesce];
    [_panel orderOut:nil];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    if ([self isVisible]) [self rebuild];
    else _builtMachine = -1;    /* rebuilt when next shown */
}

/* Only what this panel's mouse holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
