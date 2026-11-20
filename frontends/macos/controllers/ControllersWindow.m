/*
 * MSDOSControllersWindow -- the AppKit Controllers panel: the CURRENT machine's
 * own keyboard on screen (the IBM Model F for the 5150/5160, the PCjr's chiclet
 * keyboard, the Model M for the 286 and later), drawn from the session's skin
 * (msdossession_keyboard_skin_layout) so every frontend shows the same keys;
 * under it the game port's two Kraft joysticks (msdossession_controls_layout),
 * dimmed by the port mode; then Map / Clear / Defaults / Show bindings and the
 * gamepads with the joystick each drives. The art follows
 * ~/Workspace/fujinet-go-all/controllers-port/DESIGN-msdos.md (colours are the
 * MSDOS_*_RGB defines, read from photos of the hardware).
 *
 * Keys act on mouseDown AND mouseUp: a key is HELD while the mouse is down on
 * it, exactly like the real key; the keyboard's keys are the PC's own, so a
 * click presses the key whatever the bindings say. A Kraft stick is dragged
 * from anywhere (the knob follows, the stick springs back when let go); button
 * 1 is the knob, 2 the side pad, 3 and 4 the pills (4-button port only). The
 * mouse is its own input source (msdossession_pointer_set): the panel lets go
 * of everything it holds when it loses key status or is hidden
 * (msdossession_release_all). What the PC sees held from any source (the host
 * keyboard, a gamepad, the mouse) is painted with the held light, so the panel
 * doubles as an input tester. The Model M's three lock lights follow the lock
 * state.
 *
 * Keys typed with the panel key go where the main window's go (its
 * MSDOSKeyRouter); no view here takes the keyboard, so the panel itself is
 * the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized for the largest keyboard
 * skin plus the two sticks; a machine change redraws the keyboard in place.
 * Only more pads than the two rows kept for them change its height (the top
 * edge stays put). Hidden at start, ordered out rather than closed, so its
 * position survives; it floats only while the app is active
 * (hidesOnDeactivate). The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all 16 targets); every control other than the pick
 * dims. Then press a key or a gamepad button; the next one is bound and
 * whatever held it is told so in the hint line. Clear unbinds the pick's key
 * and pad inputs. Esc backs out of the pick.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here). Every file-static name carries a MsdCtl / MSDC_
 * prefix: a generic one can clash with an SDK symbol.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define MSDC_UNIT          25.0     /* points per layout unit */
#define MSDC_MARGIN        14.0
#define MSDC_STAGE_PAD     10.0
#define MSDC_TOP_GAP        8.0
#define MSDC_ROW_H         26.0
#define MSDC_EFF_W        130.0
#define MSDC_POPUP_W      130.0
#define MSDC_BTN_H         28.0
#define MSDC_MIN_PAD_ROWS   2       /* rows kept for pads, more when more are connected */
#define MSDC_MAX_PAD_ROWS   4
#define MSDC_SLOW_TICKS     8       /* the 30 Hz tick's 250 ms part */
#define MSDC_PILL_H        14.0
#define MSDC_PILL_W        46.0
#define MSDC_MAX_PILLS     32
#define MSDC_MAX_CTL       48
#define MSDC_MAX_KEYS      256
#define MSDC_DIM_ALPHA      0.4     /* disabled parts, and controls that are not the Map pick */
#define MSDC_MIN_WIN_W    600.0
#define MSDC_MAP_IDLE      (-2)
#define MSDC_MAP_ARMED     (-1)
#define MSDC_HID_ESCAPE    0x29

/* The stage behind the drawings, and the captions on it. */
#define MSDC_COL_STAGE_TOP   0x6B6C72
#define MSDC_COL_STAGE_BOT   0x55565B
#define MSDC_COL_STAGE_EDGE  0x3A3B3F
#define MSDC_COL_CAPTION     0xEEEEF2
#define MSDC_COL_PILL        0x1A1A1C
#define MSDC_COL_BTN_INK     0x303030

/* ---- helpers -------------------------------------------------------------------- */

static NSString *MsdCtlStr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *MsdCtlKeyName(uint32_t usage)
{
    const char *name = usage ? msdossession_hid_name(usage) : NULL;
    return MsdCtlStr(name);
}

static NSColor *MsdCtlRgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *MsdCtlRgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

/* A mix of two 0xRRGGBB colours, frac of the way from the first to the second. */
static uint32_t MsdCtlMixColour(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* Toggled-on look for a push button: accent bezel, white title. */
static void MsdCtlSetButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = MSDOSAccentColor();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A linear gradient clipped to a path, from one point to another. */
static void MsdCtlFillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, uint32_t rgbB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, 1.0,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views are flipped, so "top" is
 * the smaller y). */
static void MsdCtlFillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    MsdCtlFillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)),
                     topRgb, botRgb);
}

/* Lines of text (split at newlines) centred vertically in rect, the font
 * shrunk (to 5 pt) until they fit. */
static void MsdCtlDrawText(NSString *text, NSRect rect, CGFloat size, BOOL bold,
                           NSTextAlignment align, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat nlines = (CGFloat)lines.count;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = align;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat width = 0;
        lineHeight = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > width) width = extent.width;
            if (extent.height > lineHeight) lineHeight = extent.height;
        }
        if ((width <= rect.size.width && lineHeight * nlines <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight * nlines) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
        ypos += lineHeight;
    }
}

static NSBezierPath *MsdCtlRound(NSRect rect, CGFloat radius)
{
    const CGFloat rad = MIN(radius, MIN(rect.size.width, rect.size.height) / 2);
    return [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rad yRadius:rad];
}

/* A rectangle with the top corners rounded, the bottom corners, or both (the
 * views are flipped, so the top is the smaller y): the halves of the PCjr's
 * L-shaped Enter, which meet without a seam. */
static NSBezierPath *MsdCtlHalfRound(NSRect rect, CGFloat radius, BOOL top, BOOL bottom)
{
    if (top && bottom) return MsdCtlRound(rect, radius);
    const CGFloat rad = MIN(radius, MIN(rect.size.width, rect.size.height) / 2);
    const CGFloat left = NSMinX(rect), right = NSMaxX(rect), upper = NSMinY(rect), lower = NSMaxY(rect);
    NSBezierPath *path = [NSBezierPath bezierPath];
    [path moveToPoint:NSMakePoint(left + (top ? rad : 0), upper)];
    [path lineToPoint:NSMakePoint(right - (top ? rad : 0), upper)];
    if (top) [path appendBezierPathWithArcFromPoint:NSMakePoint(right, upper)
                                            toPoint:NSMakePoint(right, upper + rad) radius:rad];
    [path lineToPoint:NSMakePoint(right, lower - (bottom ? rad : 0))];
    if (bottom) [path appendBezierPathWithArcFromPoint:NSMakePoint(right, lower)
                                               toPoint:NSMakePoint(right - rad, lower) radius:rad];
    [path lineToPoint:NSMakePoint(left + (bottom ? rad : 0), lower)];
    if (bottom) [path appendBezierPathWithArcFromPoint:NSMakePoint(left, lower)
                                               toPoint:NSMakePoint(left, lower - rad) radius:rad];
    [path lineToPoint:NSMakePoint(left, upper + (top ? rad : 0))];
    if (top) [path appendBezierPathWithArcFromPoint:NSMakePoint(left, upper)
                                            toPoint:NSMakePoint(left + rad, upper) radius:rad];
    [path closePath];
    return path;
}

/* A held key or button: a 20 % wash of the held colour and a 2 px outline of
 * it, inside the path. */
static void MsdCtlMarkHeld(NSBezierPath *path)
{
    [MsdCtlRgba(MSDOS_HELD_RGB, 0.20) setFill];
    [path fill];
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    [MsdCtlRgb(MSDOS_HELD_RGB) setStroke];
    path.lineWidth = 4.0;     /* half is clipped away: 2 px inside */
    [path stroke];
    [NSGraphicsContext restoreGraphicsState];
}

/* The Map pick: a ring. */
static void MsdCtlRingPicked(NSBezierPath *path)
{
    [MsdCtlRgb(MSDOS_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* A label on a dark pill, filled with the accent when picked. */
static void MsdCtlDrawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = MsdCtlRound(rect, rect.size.height / 2);
    [(picked ? MSDOSAccentColor() : MsdCtlRgba(MSDC_COL_PILL, 0.88)) setFill];
    [pill fill];
    MsdCtlDrawText(text, NSInsetRect(rect, 3, 0), 9, YES, NSTextAlignmentCenter,
                   picked ? MSDOSInkColor() : NSColor.whiteColor);
}

/* The stage: a grey rounded backdrop. */
static void MsdCtlDrawStage(NSRect bounds)
{
    NSBezierPath *stage = MsdCtlRound(bounds, 10);
    MsdCtlFillGradient(stage, MSDC_COL_STAGE_TOP, MSDC_COL_STAGE_BOT);
    [MsdCtlRgb(MSDC_COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];
}

/* The modifier key a flagsChanged event is about, down or up; 0 for a key
 * Map does not take. */
static uint32_t MsdCtlModifierFromFlags(NSEvent *event, BOOL *down)
{
    NSEventModifierFlags flag = 0;
    switch ([event keyCode]) {
    case kVK_Shift:
    case kVK_RightShift:   flag = NSEventModifierFlagShift; break;
    case kVK_Control:
    case kVK_RightControl: flag = NSEventModifierFlagControl; break;
    case kVK_Option:
    case kVK_RightOption:  flag = NSEventModifierFlagOption; break;
    default:               return 0;
    }
    *down = ([event modifierFlags] & flag) != 0;
    return MSDOSHidFromKeyCode([event keyCode]);
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface MsdCtlFlippedView : NSView
@end

@implementation MsdCtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

@class MSDOSControllersWindow;

/* Keys typed with the panel key go to its owner. */
@interface MsdCtlPanel : NSPanel
@property (nonatomic, weak) MSDOSControllersWindow *owner;
@end

/* What the views call on the window. */
@interface MSDOSControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)event;
- (void)panelKeyUp:(NSEvent *)event;
- (void)panelFlagsChanged:(NSEvent *)event;
@end

/* ---- the keyboard skin ------------------------------------------------------------ */

/* The machine's keyboard, drawn from the core's skin: click a key to hold it,
 * every held key (typed, clicked, or from a gamepad binding's twin) is lit. */
@interface MsdCtlSkinView : MsdCtlFlippedView
@property (nonatomic) msdossession *session;
@property (nonatomic) BOOL mapping;          /* Map armed: keys inert, nothing lit */
/* Reads the skin again when the machine changed; YES when it did. */
- (BOOL)reload;
- (void)poll;
- (void)letGo;
@end

@implementation MsdCtlSkinView {
    int _machine;
    const msdos_skin_item *_items;
    int _count;
    float _unitsW, _unitsH;
    BOOL _held[MSDC_MAX_KEYS];
    unsigned _lights;
    int _pressed;               /* the key index the mouse holds, or -1 */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _machine = -1;
    _pressed = -1;
    return self;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (BOOL)reload
{
    const int machine = msdossession_machine(_session);
    if (machine == _machine) return NO;
    [self letGo];
    _machine = machine;
    _items = msdossession_keyboard_skin_layout(machine, &_count, &_unitsW, &_unitsH);
    if (!_items || _count < 0) _count = 0;
    memset(_held, 0, sizeof _held);
    [self setNeedsDisplay:YES];
    return YES;
}

- (void)poll
{
    [self reload];
    BOOL changed = NO;
    for (int idx = 0; idx < _count; idx++) {
        const msdos_skin_item *item = &_items[idx];
        if (item->kind != MSDOS_SKIN_KEYCAP || item->key < 0 || item->key >= MSDC_MAX_KEYS) continue;
        const BOOL held = !self.mapping && msdossession_keyboard_held(_session, item->key) != 0;
        if (held != _held[item->key]) { _held[item->key] = held; changed = YES; }
    }
    const unsigned lights = msdossession_lock_lights(_session);
    if (lights != _lights) { _lights = lights; changed = YES; }
    if (changed) [self setNeedsDisplay:YES];
}

/* Where the skin's origin is: centred in the (fixed) stage. */
- (NSPoint)origin
{
    const NSRect bnd = [self bounds];
    return NSMakePoint(floor((bnd.size.width - _unitsW * MSDC_UNIT) / 2),
                       floor((bnd.size.height - _unitsH * MSDC_UNIT) / 2));
}

- (void)drawBody:(const msdos_skin_item *)item rect:(NSRect)rect
{
    const char *label = item->label ? item->label : "";
    const CGFloat unit = MSDC_UNIT;
    if (!strcmp(label, "FnBracket") || !strcmp(label, "AltBracket")) {
        NSBezierPath *bracket = MsdCtlRound(NSInsetRect(rect, 1, 1), 0.2 * unit);
        [MsdCtlRgb(item->alt_rgb) setStroke];
        bracket.lineWidth = 2;
        [bracket stroke];
        return;
    }
    const BOOL isCase = !strcmp(label, "Case");
    CGFloat radius = 0.15 * unit;
    if (isCase) radius = 0.5 * unit;
    else if (!strcmp(label, "Plate")) radius = 0.4 * unit;
    NSBezierPath *body = MsdCtlRound(rect, radius);
    [MsdCtlRgb(item->fill_rgb) setFill];
    [body fill];
    if (isCase) {
        [MsdCtlRgba(0x000000, 0.4) setStroke];
        body.lineWidth = 1.5;
        [body stroke];
    } else if (!strcmp(label, "Well")) {
        [MsdCtlRgba(0x000000, 0.25) setStroke];
        body.lineWidth = 1;
        [body stroke];
    }
    if (item->shifted)
        MsdCtlDrawText(MsdCtlStr(item->shifted), NSInsetRect(rect, 3, 0), MAX(7.0, rect.size.height * 0.6), YES,
                       NSTextAlignmentCenter, MsdCtlRgb(item->ink_rgb));
}

/* A lock light with its name under it, lit while the lock is on. */
- (void)drawLed:(const msdos_skin_item *)item rect:(NSRect)rect
{
    const BOOL lit = item->key >= 0 && ((_lights >> item->key) & 1u);
    NSBezierPath *led = MsdCtlRound(rect, rect.size.height / 2);
    [MsdCtlRgb(lit ? item->alt_rgb : item->fill_rgb) setFill];
    [led fill];
    [MsdCtlRgba(0x000000, 0.45) setStroke];
    led.lineWidth = 0.75;
    [led stroke];
    MsdCtlDrawText(MsdCtlStr(item->label),
                   NSMakeRect(NSMidX(rect) - 18, NSMaxY(rect) + 1, 36, 9), 7, NO,
                   NSTextAlignmentCenter, MsdCtlRgb(item->ink_rgb));
}

/* One keycap: a bevelled cap with a lighter top face; the main legend large
 * (low when the key has a shifted legend), the shifted symbol above it, and on
 * the PCjr the green Fn legend at the bottom right. */
- (void)drawCap:(const msdos_skin_item *)item rect:(NSRect)rect
{
    const CGFloat unit = MSDC_UNIT;
    const CGFloat gap = 0.04 * unit;
    NSRect box = NSInsetRect(rect, gap, gap);
    BOOL top = YES, bottom = YES;
    if (item->shape == MSDOS_SHAPE_JOIN_BELOW) {
        box.size.height += gap;
        bottom = NO;
    } else if (item->shape == MSDOS_SHAPE_JOIN_ABOVE) {
        box.origin.y -= gap;
        box.size.height += gap;
        top = NO;
    }
    const BOOL lit = item->key >= 0 && item->key < MSDC_MAX_KEYS && !self.mapping &&
                     (_held[item->key] || item->key == _pressed);
    const CGFloat radius = item->cap_class == MSDOS_CAP_CHICLET ? 0.18 * unit : 0.12 * unit;
    NSBezierPath *cap = MsdCtlHalfRound(box, radius, top, bottom);
    [MsdCtlRgb(MsdCtlMixColour(item->fill_rgb, 0x000000, 0.25)) setFill];
    [cap fill];
    NSRect face = NSInsetRect(box, 0.04 * unit, 0.04 * unit);
    if (bottom) { face.size.height -= 0.06 * unit; }
    NSBezierPath *facePath = MsdCtlHalfRound(face, radius * 0.8, top, bottom);
    MsdCtlFillGradient(facePath, MsdCtlMixColour(item->fill_rgb, 0xFFFFFF, 0.18), item->fill_rgb);
    [MsdCtlRgba(0x000000, 0.35) setStroke];
    cap.lineWidth = 1;
    [cap stroke];

    NSColor *ink = MsdCtlRgb(item->ink_rgb);
    /* the joined Enter halves carry the legend once, on the upper one */
    if (item->label && item->shape != MSDOS_SHAPE_JOIN_ABOVE) {
        NSRect area = NSInsetRect(face, 1, 0);
        const CGFloat mainSize = MAX(7.0, unit * 0.4);
        if (item->fn_legend) {
            MsdCtlDrawText(MsdCtlStr(item->fn_legend),
                           NSMakeRect(area.origin.x + area.size.width * 0.45, area.origin.y + area.size.height * 0.5,
                                      area.size.width * 0.55, area.size.height * 0.5),
                           MAX(5.5, unit * 0.26), YES, NSTextAlignmentRight, MsdCtlRgb(item->alt_rgb));
            area.size.width *= 0.78;
        }
        if (item->shifted && item->shifted[0]) {
            const CGFloat half = floor(area.size.height / 2);
            MsdCtlDrawText(MsdCtlStr(item->shifted), NSMakeRect(area.origin.x, area.origin.y, area.size.width, half),
                           MAX(6.5, unit * 0.34), NO, NSTextAlignmentCenter, ink);
            MsdCtlDrawText(MsdCtlStr(item->label),
                           NSMakeRect(area.origin.x, area.origin.y + half, area.size.width, area.size.height - half),
                           MAX(6.5, unit * 0.34), NO, NSTextAlignmentCenter, ink);
        } else {
            MsdCtlDrawText(MsdCtlStr(item->label), area, mainSize, NO, NSTextAlignmentCenter, ink);
        }
    }
    if (lit) MsdCtlMarkHeld(MsdCtlHalfRound(box, radius, top, bottom));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [self reload];
    MsdCtlDrawStage([self bounds]);
    if (_count == 0) return;
    const NSPoint org = [self origin];
    for (int idx = 0; idx < _count; idx++) {
        const msdos_skin_item *item = &_items[idx];
        const NSRect rect = NSMakeRect(org.x + item->x * MSDC_UNIT, org.y + item->y * MSDC_UNIT,
                                       item->w * MSDC_UNIT, item->h * MSDC_UNIT);
        switch (item->kind) {
        case MSDOS_SKIN_BODY:   [self drawBody:item rect:rect]; break;
        case MSDOS_SKIN_KEYCAP: [self drawCap:item rect:rect]; break;
        case MSDOS_SKIN_LED:    [self drawLed:item rect:rect]; break;
        default: break;
        }
    }
}

- (void)mouseDown:(NSEvent *)event
{
    [self reload];
    if (_count == 0 || _pressed >= 0 || self.mapping) return;
    const NSPoint org = [self origin];
    const NSPoint pnt = [self convertPoint:[event locationInWindow] fromView:nil];
    const int idx = msdossession_keyboard_skin_hit(_machine, (float)((pnt.x - org.x) / MSDC_UNIT),
                                                   (float)((pnt.y - org.y) / MSDC_UNIT));
    if (idx < 0 || idx >= _count || _items[idx].key < 0) return;
    _pressed = _items[idx].key;
    msdossession_keyboard_press(_session, _pressed, 1);
    [self setNeedsDisplay:YES];
}

/* A drag off the key lets it go. */
- (void)mouseDragged:(NSEvent *)event
{
    if (_pressed < 0) return;
    const NSPoint org = [self origin];
    const NSPoint pnt = [self convertPoint:[event locationInWindow] fromView:nil];
    const int idx = msdossession_keyboard_skin_hit(_machine, (float)((pnt.x - org.x) / MSDC_UNIT),
                                                   (float)((pnt.y - org.y) / MSDC_UNIT));
    if (idx < 0 || idx >= _count || _items[idx].key != _pressed) [self letGo];
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    [self letGo];
}

- (void)letGo
{
    if (_pressed < 0) return;
    msdossession_keyboard_press(_session, _pressed, 0);
    _pressed = -1;
    [self setNeedsDisplay:YES];
}
@end

/* ---- the Kraft sticks ----------------------------------------------------------------- */

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} MsdCtlLabel;

/* The two joysticks side by side, drawn from the core's layout and hit-tested
 * by it (the drag maths are the core's too). */
@interface MsdCtlStageView : MsdCtlFlippedView
@property (nonatomic) msdossession *session;
@property (nonatomic, weak) MSDOSControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
/* (Re)reads the layout for the game port's mode. */
- (void)configure;
/* Whether the layout is out of date: the game port has another mode. */
- (BOOL)layoutStale;
/* Reads what the PC sees; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation MsdCtlStageView {
    int _mode;
    const msdos_control *_ctls;   /* the core's static table */
    int _count;
    float _dimW, _dimH;
    BOOL _lit[MSDC_MAX_CTL];      /* what the PC sees held, per control */
    int _axisX[MSDOS_JOYSTICKS];  /* the merged sticks (the knobs) */
    int _axisY[MSDOS_JOYSTICKS];
    float _travel[MSDOS_JOYSTICKS];   /* each stick's knob travel, layout units */

    /* the mouse */
    int _ptrX[MSDOS_JOYSTICKS], _ptrY[MSDOS_JOYSTICKS];
    unsigned _ptrButtons[MSDOS_JOYSTICKS];
    int _downIdx;                 /* the button or pill pressed, or -1 */
    int _steerIdx;                /* the stick control being dragged, or -1 */
    int _hover;                   /* the pressable part under the pointer (Map), or -1 */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _mode = -1;
    _downIdx = -1;
    _steerIdx = -1;
    _mapTarget = -1;
    _hover = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)configure
{
    _mode = msdossession_game_port_mode(_session);
    _ctls = msdossession_controls_layout(_mode, &_count, &_dimW, &_dimH);
    if (!_ctls || _count < 0) _count = 0;
    if (_count > MSDC_MAX_CTL) _count = MSDC_MAX_CTL;
    _hover = -1;
    memset(_lit, 0, sizeof _lit);
    for (int idx = 0; idx < _count; idx++)
        if (_ctls[idx].kind == MSDOS_CTL_STICK && _ctls[idx].stick >= 0 && _ctls[idx].stick < MSDOS_JOYSTICKS)
            _travel[_ctls[idx].stick] = _ctls[idx].reach;
    [self setNeedsDisplay:YES];
}

- (BOOL)layoutStale
{
    return msdossession_game_port_mode(_session) != _mode;
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

/* Where the layout's origin is, in view points: centred in the stage. */
- (NSPoint)origin
{
    return NSMakePoint(floor(([self bounds].size.width - _dimW * MSDC_UNIT) / 2), MSDC_STAGE_PAD);
}

- (NSRect)rectOf:(const msdos_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * MSDC_UNIT, org.y + ctl->y * MSDC_UNIT, ctl->w * MSDC_UNIT, ctl->h * MSDC_UNIT);
}

- (NSPoint)pointAtX:(float)xpos y:(float)ypos
{
    const NSPoint org = [self origin];
    return NSMakePoint(org.x + xpos * MSDC_UNIT, org.y + ypos * MSDC_UNIT);
}

- (const msdos_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable part under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return msdossession_controls_hit(_mode, (float)((pnt.x - org.x) / MSDC_UNIT),
                                     (float)((pnt.y - org.y) / MSDC_UNIT));
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

/* Whether a Map pick is one of a stick's four directions. */
static BOOL MsdCtlIsDirectionOf(int target, int stick)
{
    return target >= MSDOS_TARGET(stick, MSDOS_ACT_UP) && target <= MSDOS_TARGET(stick, MSDOS_ACT_RIGHT);
}

/* The binding target a Map click on a stick at (dx, dy) layout units from the
 * knob's rest centre means: the dominant direction; -1 in the dead centre. */
static int MsdCtlDirectionTarget(int stick, float dx, float dy)
{
    if (hypotf(dx, dy) < 0.5f) return -1;
    int act;
    if (fabsf(dx) >= fabsf(dy)) act = dx < 0 ? MSDOS_ACT_LEFT : MSDOS_ACT_RIGHT;
    else act = dy < 0 ? MSDOS_ACT_UP : MSDOS_ACT_DOWN;
    return MSDOS_TARGET(stick, act);
}

/* The alpha a part is drawn with: faded when the port mode disables it, and in
 * Map mode every pressable part but the pick (or, while nothing is picked yet,
 * the one under the pointer). */
- (CGFloat)alphaOfIdx:(int)idx
{
    const msdos_control *ctl = &_ctls[idx];
    CGFloat alpha = ctl->enabled ? 1.0 : MSDC_DIM_ALPHA;
    if (!self.mapping || !ctl->enabled) return alpha;
    BOOL show;
    switch (ctl->kind) {
    case MSDOS_CTL_STICK:
        show = (self.mapTarget >= 0 && MsdCtlIsDirectionOf(self.mapTarget, ctl->stick)) ||
               (self.mapTarget < 0 && _hover == idx);
        break;
    case MSDOS_CTL_BUTTON:
    case MSDOS_CTL_PILL:
        show = [self picked:ctl->target] || (self.mapTarget < 0 && _hover == idx);
        break;
    default:
        return alpha;
    }
    return show ? alpha : alpha * MSDC_DIM_ALPHA;
}

- (void)beginAlpha:(CGFloat)alpha
{
    if (alpha >= 1.0) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, alpha);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endAlpha:(CGFloat)alpha
{
    if (alpha >= 1.0) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* ---- the labels shown over the controls ---------------------------------------- */

- (int)labels:(MsdCtlLabel *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
#define MSDC_ADD_LABEL(cx, cy, tgt, prefix) do { \
        if (num < max) { \
            MsdCtlLabel *lab = &out[num++]; \
            lab->rect = NSMakeRect((cx) - MSDC_PILL_W / 2, (cy) - MSDC_PILL_H / 2, MSDC_PILL_W, MSDC_PILL_H); \
            lab->target = (tgt); \
            NSString *name = [self.owner legendForTarget:(tgt)]; \
            NSString *text = [NSString stringWithFormat:@"%@%@", prefix, name.length ? name : @"-"]; \
            strncpy(lab->text, text.UTF8String, sizeof lab->text - 1); \
            lab->text[sizeof lab->text - 1] = '\0'; \
        } } while (0)
    for (int idx = 0; idx < _count; idx++) {
        const msdos_control *ctl = &_ctls[idx];
        if (!ctl->enabled) continue;
        const NSRect rect = [self rectOf:ctl];
        switch (ctl->kind) {
        case MSDOS_CTL_STICK: {
            const NSPoint ctr = [self pointAtX:ctl->cenx y:ctl->ceny];
            const CGFloat far = 2.5 * MSDC_UNIT;
            MSDC_ADD_LABEL(ctr.x - far, ctr.y, MSDOS_TARGET(ctl->stick, MSDOS_ACT_LEFT), @"◀ ");
            MSDC_ADD_LABEL(ctr.x + far, ctr.y, MSDOS_TARGET(ctl->stick, MSDOS_ACT_RIGHT), @"▶ ");
            MSDC_ADD_LABEL(ctr.x, ctr.y - far, MSDOS_TARGET(ctl->stick, MSDOS_ACT_UP), @"▲ ");
            MSDC_ADD_LABEL(ctr.x, ctr.y + far, MSDOS_TARGET(ctl->stick, MSDOS_ACT_DOWN), @"▼ ");
            break;
        }
        case MSDOS_CTL_BUTTON:
            if (ctl->reach > 0) {
                const NSPoint ctr = [self pointAtX:ctl->cenx y:ctl->ceny];
                MSDC_ADD_LABEL(ctr.x, ctr.y, ctl->target, @"");
            } else {
                MSDC_ADD_LABEL(NSMidX(rect), NSMaxY(rect) + MSDC_PILL_H / 2 + 1, ctl->target, @"");
            }
            break;
        case MSDOS_CTL_PILL:
            MSDC_ADD_LABEL(NSMidX(rect), NSMidY(rect), ctl->target, @"");
            break;
        default:
            break;
        }
    }
#undef MSDC_ADD_LABEL
    return num;
}

/* The target Map would pick at a point (view points), or -1. */
- (int)mapTargetAt:(NSPoint)pnt
{
    MsdCtlLabel labels[MSDC_MAX_PILLS];
    const int num = [self labels:labels max:MSDC_MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, labels[idx].rect)) return labels[idx].target;
    const msdos_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case MSDOS_CTL_BUTTON:
    case MSDOS_CTL_PILL:
        return ctl->target;
    case MSDOS_CTL_STICK: {
        const NSPoint org = [self origin];
        return MsdCtlDirectionTarget(ctl->stick, (float)((pnt.x - org.x) / MSDC_UNIT) - ctl->cenx,
                                     (float)((pnt.y - org.y) / MSDC_UNIT) - ctl->ceny);
    }
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    for (int stick = 0; stick < MSDOS_JOYSTICKS; stick++) {
        int xpos = 0, ypos = 0;
        unsigned buttons = 0;
        if (!self.mapping) msdossession_joystick_state(_session, stick, &xpos, &ypos, &buttons);
        if (xpos != _axisX[stick] || ypos != _axisY[stick]) {
            _axisX[stick] = xpos;
            _axisY[stick] = ypos;
            dirty = YES;
        }
    }
    for (int idx = 0; idx < _count; idx++) {
        const msdos_control *ctl = &_ctls[idx];
        BOOL lit = NO;
        if (!self.mapping && ctl->enabled && ctl->target >= 0 &&
            (ctl->kind == MSDOS_CTL_BUTTON || ctl->kind == MSDOS_CTL_PILL))
            lit = msdossession_target_held(_session, ctl->target) != 0;
        if (lit != _lit[idx]) { _lit[idx] = lit; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

- (void)drawBody:(const msdos_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const char *label = ctl->label ? ctl->label : "";
    if (!strcmp(label, "Base")) {
        NSBezierPath *base = MsdCtlRound(rect, 0.6 * MSDC_UNIT);
        MsdCtlFillGradient(base, MsdCtlMixColour(MSDOS_KRAFT_BODY_RGB, 0xFFFFFF, 0.3), MSDOS_KRAFT_BODY_RGB);
        [MsdCtlRgb(MSDOS_KRAFT_RIM_RGB) setStroke];
        base.lineWidth = 1.5;
        [base stroke];
    } else if (!strcmp(label, "Plate")) {
        [MsdCtlRgb(MSDOS_KRAFT_PLATE_RGB) setFill];
        [MsdCtlRound(rect, 0.5 * MSDC_UNIT) fill];
    } else if (!strcmp(label, "Well")) {
        NSBezierPath *well = [NSBezierPath bezierPathWithOvalInRect:rect];
        [MsdCtlRgb(MSDOS_KRAFT_WELL_RGB) setFill];
        [well fill];
        [MsdCtlRgba(0x000000, 0.25) setStroke];
        well.lineWidth = 1;
        [well stroke];
    } else if (!strcmp(label, "Label")) {
        [MsdCtlRgb(MSDOS_KRAFT_LOGO_RGB) setFill];
        [MsdCtlRound(rect, 0.12 * MSDC_UNIT) fill];
    }
}

- (void)drawCaption:(const msdos_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSString *text = MsdCtlStr(ctl->label);
    if (ctl->label && !strcmp(ctl->label, "KRAFT"))
        MsdCtlDrawText(text, rect, MAX(7.0, rect.size.height * 0.85), YES, NSTextAlignmentCenter,
                       NSColor.whiteColor);
    else
        MsdCtlDrawText(text, rect, 14, YES, NSTextAlignmentCenter, MsdCtlRgb(MSDC_COL_CAPTION));
}

/* A trim slider at its centre: a groove and a thumb. */
- (void)drawTrim:(const msdos_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *groove = MsdCtlRound(rect, MIN(rect.size.width, rect.size.height) / 2);
    [MsdCtlRgb(MSDOS_KRAFT_TRIM_RGB) setFill];
    [groove fill];
    [MsdCtlRgba(0x000000, 0.3) setStroke];
    groove.lineWidth = 0.75;
    [groove stroke];
    const BOOL vertical = rect.size.height > rect.size.width;
    const CGFloat thumbLong = 0.8 * MSDC_UNIT, thumbShort = (vertical ? rect.size.width : rect.size.height) + 3;
    const NSRect thumb = vertical
        ? NSMakeRect(NSMidX(rect) - thumbShort / 2, NSMidY(rect) - thumbLong / 2, thumbShort, thumbLong)
        : NSMakeRect(NSMidX(rect) - thumbLong / 2, NSMidY(rect) - thumbShort / 2, thumbLong, thumbShort);
    [MsdCtlRgb(MsdCtlMixColour(MSDOS_KRAFT_BODY_RGB, 0xFFFFFF, 0.4)) setFill];
    NSBezierPath *thumbPath = MsdCtlRound(thumb, 2);
    [thumbPath fill];
    [MsdCtlRgba(0x000000, 0.4) setStroke];
    thumbPath.lineWidth = 0.75;
    [thumbPath stroke];
}

/* The stick's grip, at rest in the well and moved by the merged axes. */
- (void)drawGrip:(const msdos_control *)ctl
{
    const int stick = ctl->stick;
    const double reach = ctl->reach;
    const float cx = ctl->cenx + (float)(_axisX[stick] / 32768.0 * reach);
    const float cy = ctl->ceny + (float)(_axisY[stick] / 32768.0 * reach);
    const NSPoint rest = [self pointAtX:ctl->cenx y:ctl->ceny];
    const NSPoint ctr = [self pointAtX:cx y:cy];
    const CGFloat radius = 1.3 * MSDC_UNIT;
    if (_axisX[stick] || _axisY[stick]) {
        /* the shaft from the pivot to the grip */
        NSBezierPath *shaft = [NSBezierPath bezierPath];
        [shaft moveToPoint:rest];
        [shaft lineToPoint:ctr];
        shaft.lineWidth = 0.7 * MSDC_UNIT;
        shaft.lineCapStyle = NSLineCapStyleRound;
        [MsdCtlRgb(MSDOS_KRAFT_SHAFT_RGB) setStroke];
        [shaft stroke];
    }
    NSBezierPath *grip = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(ctr.x - radius, ctr.y - radius,
                                                                           2 * radius, 2 * radius)];
    MsdCtlFillGradient(grip, MsdCtlMixColour(MSDOS_KRAFT_GRIP_RGB, 0xFFFFFF, 0.35), MSDOS_KRAFT_GRIP_RGB);
    [MsdCtlRgb(MSDOS_KRAFT_SHAFT_RGB) setStroke];
    grip.lineWidth = 1.5;
    [grip stroke];
}

/* Button 1: the round cap on the knob, which moves with it. */
- (void)drawKnobButton:(const msdos_control *)ctl lit:(BOOL)lit
{
    const int stick = ctl->stick;
    const double travel = _travel[stick];   /* the knob and its button move together */
    const NSPoint ctr = [self pointAtX:ctl->cenx + (float)(_axisX[stick] / 32768.0 * travel)
                                      y:ctl->ceny + (float)(_axisY[stick] / 32768.0 * travel)];
    const CGFloat radius = ctl->reach * MSDC_UNIT;
    const NSRect disc = NSMakeRect(ctr.x - radius, ctr.y - radius, 2 * radius, 2 * radius);
    NSBezierPath *path = [NSBezierPath bezierPathWithOvalInRect:disc];
    MsdCtlFillGradient(path, MsdCtlMixColour(MSDOS_KRAFT_CAP_RGB, 0xFFFFFF, 0.25), MSDOS_KRAFT_CAP_RGB);
    [MsdCtlRgba(0x000000, 0.4) setStroke];
    path.lineWidth = 1;
    [path stroke];
    MsdCtlDrawText(MsdCtlStr(ctl->legend), NSInsetRect(disc, 4, 4), 13, YES, NSTextAlignmentCenter,
                   NSColor.whiteColor);
    if (lit) MsdCtlMarkHeld([NSBezierPath bezierPathWithOvalInRect:disc]);
    if ([self picked:ctl->target]) MsdCtlRingPicked([NSBezierPath bezierPathWithOvalInRect:disc]);
}

/* Button 2 (the side pad) and buttons 3 and 4 (the pills). */
- (void)drawFlatButton:(const msdos_control *)ctl lit:(BOOL)lit
{
    const NSRect rect = [self rectOf:ctl];
    const CGFloat radius = ctl->kind == MSDOS_CTL_PILL ? rect.size.height / 2 : 0.25 * MSDC_UNIT;
    NSBezierPath *path = MsdCtlRound(rect, radius);
    MsdCtlFillGradient(path, MsdCtlMixColour(MSDOS_KRAFT_SIDE_RGB, 0xFFFFFF, 0.25), MSDOS_KRAFT_SIDE_RGB);
    [MsdCtlRgba(0x000000, 0.4) setStroke];
    path.lineWidth = 1;
    [path stroke];
    MsdCtlDrawText(MsdCtlStr(ctl->legend), NSInsetRect(rect, 2, 1), 13, YES, NSTextAlignmentCenter,
                   MsdCtlRgb(MSDC_COL_BTN_INK));
    if (lit) MsdCtlMarkHeld(MsdCtlRound(rect, radius));
    if ([self picked:ctl->target]) MsdCtlRingPicked(MsdCtlRound(rect, radius));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    MsdCtlDrawStage([self bounds]);
    for (int idx = 0; idx < _count; idx++) {
        const msdos_control *ctl = &_ctls[idx];
        const CGFloat alpha = [self alphaOfIdx:idx];
        [self beginAlpha:alpha];
        switch (ctl->kind) {
        case MSDOS_CTL_CAPTION: [self drawCaption:ctl]; break;
        case MSDOS_CTL_BODY:    [self drawBody:ctl]; break;
        case MSDOS_CTL_TRIM:    [self drawTrim:ctl]; break;
        case MSDOS_CTL_STICK:   [self drawGrip:ctl]; break;
        case MSDOS_CTL_BUTTON:
            if (ctl->reach > 0) [self drawKnobButton:ctl lit:_lit[idx]];
            else [self drawFlatButton:ctl lit:_lit[idx]];
            break;
        case MSDOS_CTL_PILL:    [self drawFlatButton:ctl lit:_lit[idx]]; break;
        default: break;
        }
        [self endAlpha:alpha];
    }

    /* the Map / Show bindings labels, over everything */
    MsdCtlLabel labels[MSDC_MAX_PILLS];
    const int num = [self labels:labels max:MSDC_MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        MsdCtlDrawPill(labels[idx].rect, MsdCtlStr(labels[idx].text), [self picked:labels[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)sendStick:(int)stick
{
    msdossession_pointer_set(_session, stick, _ptrX[stick], _ptrY[stick], _ptrButtons[stick]);
}

- (void)letGo
{
    const msdos_control *pressed = [self ctlAt:_downIdx];
    const msdos_control *steered = [self ctlAt:_steerIdx];
    if (pressed) {
        const int bit = pressed->target % MSDOS_ACT_COUNT - MSDOS_ACT_B1;
        _ptrButtons[pressed->stick] &= ~(1u << bit);
        [self sendStick:pressed->stick];
    }
    if (steered) {
        _ptrX[steered->stick] = 0;
        _ptrY[steered->stick] = 0;
        [self sendStick:steered->stick];
    }
    _downIdx = -1;
    _steerIdx = -1;
    _hover = -1;
    [self setNeedsDisplay:YES];
}

/* The stick follows the pointer from anywhere: the core turns the offset from
 * the knob's rest centre into axes. */
- (void)steerAt:(NSPoint)pnt
{
    const msdos_control *ctl = [self ctlAt:_steerIdx];
    if (!ctl) return;
    const NSPoint org = [self origin];
    int xpos = 0, ypos = 0;
    msdossession_stick_from_vector((float)((pnt.x - org.x) / MSDC_UNIT) - ctl->cenx,
                                   (float)((pnt.y - org.y) / MSDC_UNIT) - ctl->ceny, ctl->reach, &xpos, &ypos);
    _ptrX[ctl->stick] = xpos;
    _ptrY[ctl->stick] = ypos;
    [self sendStick:ctl->stick];
}

- (void)mouseDown:(NSEvent *)event
{
    if (_downIdx >= 0 || _steerIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[event locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, a direction or a button is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    const int idx = [self hit:pnt];
    const msdos_control *ctl = [self ctlAt:idx];
    if (!ctl || !ctl->enabled) return;
    switch (ctl->kind) {
    case MSDOS_CTL_STICK:
        _steerIdx = idx;
        [self steerAt:pnt];
        break;
    case MSDOS_CTL_BUTTON:
    case MSDOS_CTL_PILL:
        if (ctl->target < 0) break;
        _downIdx = idx;
        _ptrButtons[ctl->stick] |= 1u << (ctl->target % MSDOS_ACT_COUNT - MSDOS_ACT_B1);
        [self sendStick:ctl->stick];
        break;
    default:
        break;
    }
    [self poll];
}

- (void)mouseDragged:(NSEvent *)event
{
    if (_downIdx < 0 && _steerIdx < 0) return;
    const NSPoint pnt = [self convertPoint:[event locationInWindow] fromView:nil];
    if (_steerIdx >= 0) {
        [self steerAt:pnt];
        [self poll];
        return;
    }
    /* a button or pill: dragging off lets go */
    if ([self hit:pnt] != _downIdx) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    if (_downIdx < 0 && _steerIdx < 0) return;
    [self letGo];
    [self poll];
}

/* In Map mode the part under the pointer stays bright until one is picked. */
- (void)mouseMoved:(NSEvent *)event
{
    if (!self.mapping || self.mapTarget >= 0) return;
    const int idx = [self hit:[self convertPoint:[event locationInWindow] fromView:nil]];
    if (idx != _hover) {
        _hover = idx;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}
@end

@implementation MsdCtlPanel
- (void)keyDown:(NSEvent *)event { [self.owner panelKeyDown:event]; }
- (void)keyUp:(NSEvent *)event { [self.owner panelKeyUp:event]; }
- (void)flagsChanged:(NSEvent *)event { [self.owner panelFlagsChanged:event]; }
/* Escape is the PC's Esc (or backs out of a Map pick), not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation MSDOSControllersWindow {
    msdossession *_session;
    __weak id<MSDOSKeyRouter> _router;
    MsdCtlPanel *_panel;
    MsdCtlFlippedView *_content;
    MsdCtlSkinView *_skin;
    MsdCtlStageView *_stage;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MSDC_MAX_PAD_ROWS];
    NSTextField *_padEff[MSDC_MAX_PAD_ROWS];     /* each pad's "drives Joystick N" */
    NSPopUpButton *_padAssign[MSDC_MAX_PAD_ROWS];
    int _padRows;                                /* the rows showing a pad */
    int _reservedRows;                           /* the rows the panel has room for */
    int _menuSticks;                             /* the sticks the pad menus list */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSPopUpButton *_mapList;                     /* the 16 targets */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    CGFloat _winW;
    CGFloat _skinH, _stageH;
    uint32_t _swallowKey;                        /* the key Map took: its repeats and release too */
    /* MSDC_MAP_IDLE, MSDC_MAP_ARMED (waiting for a control), or >= 0: waiting
     * for a key or gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(msdossession *)session router:(id<MSDOSKeyRouter>)router
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _router = router;
    _mapState = MSDC_MAP_IDLE;
    _padGeneration = (unsigned)-1;
    _reservedRows = MSDC_MIN_PAD_ROWS;
    _menuSticks = -1;

    /* The window is sized for the largest keyboard skin (whatever the machine)
     * and the two sticks. */
    float skinW = 0, skinH = 0;
    for (int machine = 0; machine < MSDOS_M_COUNT; machine++) {
        int count = 0;
        float wid = 0, hgt = 0;
        (void)msdossession_keyboard_skin_layout(machine, &count, &wid, &hgt);
        if (wid > skinW) skinW = wid;
        if (hgt > skinH) skinH = hgt;
    }
    int ctlCount = 0;
    float ctlW = 0, ctlH = 0;
    (void)msdossession_controls_layout(MSDOS_PORT_TWO, &ctlCount, &ctlW, &ctlH);
    _winW = MAX(MSDC_MIN_WIN_W, MAX(ceil(skinW * MSDC_UNIT), ceil(ctlW * MSDC_UNIT)) + 2 * MSDC_STAGE_PAD +
                                    2 * MSDC_MARGIN);
    _skinH = ceil(skinH * MSDC_UNIT) + 2 * MSDC_STAGE_PAD;
    _stageH = ceil(ctlH * MSDC_UNIT) + 2 * MSDC_STAGE_PAD;

    _panel = [[MsdCtlPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, _winW, 600)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the PC's window, a full-screen one too, but only while the
     * app is active: it does not stay over other apps' windows. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    [_panel setHidesOnDeactivate:YES];
    _panel.owner = self;
    [_panel setDelegate:self];
    MSDOSSetHelpContext(_panel, @"controllers");

    _content = [[MsdCtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, _winW, 600)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _skin = [[MsdCtlSkinView alloc] initWithFrame:NSZeroRect];
    _skin.session = _session;
    [_content addSubview:_skin];

    _stage = [[MsdCtlStageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = msdossession_controls_show_bindings(_session) != 0;
    [_content addSubview:_stage];

    /* Gamepads: one row per pad, its name, the joystick it drives and the one
     * it is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads. One plugged in drives Joystick A, the next B."];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MSDC_MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* Automatic (connection order), then one item per stick the port has */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    /* The Map list: every target by name. Items go in by the menu, as equal
     * titles would collapse. */
    _mapList = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    {
        NSMenuItem *none = [[_mapList menu] addItemWithTitle:@"Choose a control..." action:nil keyEquivalent:@""];
        none.tag = -1;
        for (int target = 0; target < MSDOS_TARGET_COUNT; target++) {
            NSMenuItem *item = [[_mapList menu] addItemWithTitle:MsdCtlStr(msdos_target_name(target)) action:nil
                                                   keyEquivalent:@""];
            item.tag = target;
        }
    }
    _mapList.target = self;
    _mapList.action = @selector(mapListChanged:);
    [_mapList setRefusesFirstResponder:YES];
    [_content addSubview:_mapList];
    _helpButton = MSDOSHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];

    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(occlusionChanged:)
                   name:NSWindowDidChangeOcclusionStateNotification object:_panel];
    /* The app going to the background: Map must not keep swallowing pad input
     * while the panel is hidden. */
    [center addObserver:self selector:@selector(appResigned:)
                   name:NSApplicationDidResignActiveNotification object:nil];

    [_stage configure];
    [_skin reload];
    [self rebuildPadMenus];
    [self setMapState:MSDC_MAP_IDLE];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

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
    const CGFloat inner = _winW - 2 * MSDC_MARGIN;
    const CGFloat skinY = MSDC_TOP_GAP;
    const CGFloat stageY = skinY + _skinH + 8;
    const CGFloat mapY = stageY + _stageH + 10;
    const CGFloat hintY = mapY + MSDC_BTN_H + 6;
    const CGFloat padsY = hintY + 34 + 6;
    const CGFloat rowsY = padsY + 18;
    const CGFloat total = rowsY + _reservedRows * MSDC_ROW_H + MSDC_MARGIN;

    [self fitPanel:NSMakeSize(_winW, total)];
    [_content setFrameSize:NSMakeSize(_winW, total)];
    [_skin setFrame:NSMakeRect(MSDC_MARGIN, skinY, inner, _skinH)];
    [_stage setFrame:NSMakeRect(MSDC_MARGIN, stageY, inner, _stageH)];
    [_mapButton setFrame:NSMakeRect(MSDC_MARGIN, mapY, 56, MSDC_BTN_H)];
    [_clearButton setFrame:NSMakeRect(MSDC_MARGIN + 62, mapY, 56, MSDC_BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MSDC_MARGIN + 124, mapY, 72, MSDC_BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MSDC_MARGIN + 206, mapY + 3, 122, 22)];
    [_mapList setFrame:NSMakeRect(MSDC_MARGIN + 336, mapY + 1, 200, 25)];
    [_helpButton setFrameOrigin:NSMakePoint(_winW - MSDC_MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_hint setFrame:NSMakeRect(MSDC_MARGIN, hintY, inner, 34)];
    [_padsTitle setFrame:NSMakeRect(MSDC_MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MSDC_MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MSDC_MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * MSDC_ROW_H;
        [_padName[row] setFrame:NSMakeRect(MSDC_MARGIN, rowY + 4, inner - MSDC_POPUP_W - MSDC_EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MSDC_MARGIN + inner - MSDC_POPUP_W - MSDC_EFF_W - 6, rowY + 4,
                                          MSDC_EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MSDC_MARGIN + inner - MSDC_POPUP_W, rowY - 1, MSDC_POPUP_W, 25)];
    }
    [_stage setNeedsDisplay:YES];
    [_skin setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel (the pointer source and the
 * clicked key). */
- (void)letGo
{
    [_stage letGo];
    [_skin letGo];
}

/* The machine (the keyboard) or the game port's mode (the sticks) changed:
 * the pictures are read again; the panel does not change size. */
- (void)syncLayout
{
    if ([_skin reload]) [_skin poll];
    if ([_stage layoutStale]) {
        [_stage letGo];
        [_stage configure];
        [self rebuildPadMenus];
        [self refreshPadEffects];
        [self setIdleHint];
        [_stage poll];
    }
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    const msdos_binding bind = msdossession_binding_get(_session, target);
    if (bind.key) return MsdCtlKeyName(bind.key);
    if (bind.button != MSDOS_PAD_BTN_NONE) return MsdCtlStr(msdos_pad_button_name(bind.button));
    return @"";
}

/* What is bound to a target, for the hint line. */
- (NSString *)bindingText:(int)target
{
    const msdos_binding bind = msdossession_binding_get(_session, target);
    NSString *key = bind.key ? MsdCtlKeyName(bind.key) : @"no key";
    if (bind.button != MSDOS_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, MsdCtlStr(msdos_pad_button_name(bind.button))];
    return key;
}

- (void)setIdleHint
{
    if (_mapState != MSDC_MAP_IDLE) return;
    _hint.stringValue = msdossession_game_port_mode(_session) == MSDOS_PORT_NONE
        ? @"No joystick on the game port (Preferences > Ports). The keyboard works as on the PC."
        : @"Click a key to hold it, drag a stick, hold a button. Keys and gamepads work here too.";
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != MSDC_MAP_IDLE;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    _skin.mapping = _mapState != MSDC_MAP_IDLE;
    [_stage setNeedsDisplay:YES];
    [_skin setNeedsDisplay:YES];
    [_stage poll];
    [_skin poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        msdossession_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc backs out.",
                             MsdCtlStr(msdos_target_name(state)), [self bindingText:state]];
    } else {
        msdossession_gamepad_capture_cancel(_session);
        if (state == MSDC_MAP_ARMED)
            _hint.stringValue = @"Click a direction or a button to remap it, or choose one from the list.";
        else [self setIdleHint];
    }
    if (state != MSDC_MAP_IDLE) {
        [self letGo];
        msdossession_release_all(_session);   /* Map's key-ups must not strand a held input */
    }
    [_mapButton setTitle:(state == MSDC_MAP_IDLE ? @"Map" : @"Done")];
    MsdCtlSetButtonAccent(_mapButton, state != MSDC_MAP_IDLE);
    _clearButton.enabled = state >= 0;
    _mapList.enabled = state != MSDC_MAP_IDLE;
    if (![_mapList selectItemWithTag:(state >= 0 ? state : -1)]) [_mapList selectItemAtIndex:0];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != MSDC_MAP_IDLE && target >= 0 && target < MSDOS_TARGET_COUNT) [self setMapState:target];
}

- (void)mapListChanged:(NSPopUpButton *)sender
{
    const NSInteger tag = sender.selectedItem.tag;
    if (tag >= 0) [self mapPick:(int)tag];
    else if (_mapState >= 0) [self setMapState:MSDC_MAP_ARMED];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = MsdCtlStr(msdos_target_name(target));
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, MsdCtlStr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)usage swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    msdossession_binding_set_key(_session, target, usage, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = usage;
    const BOOL taken = msdossession_binding_get(_session, target).key == usage;
    [self setMapState:MSDC_MAP_ARMED];   /* stay armed: remapping several in a row is normal */
    if (taken) [self showBound:MsdCtlKeyName(usage) target:target stolen:stolen];
    else _hint.stringValue = [NSString stringWithFormat:@"The PC has no key for %@", MsdCtlKeyName(usage)];
}

- (void)pollCapture
{
    int button = MSDOS_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !msdossession_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    msdossession_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:MSDC_MAP_ARMED];
    [self showBound:MsdCtlStr(msdos_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == MSDC_MAP_IDLE ? MSDC_MAP_ARMED : MSDC_MAP_IDLE)];
}

/* Clear: the picked control loses its key and its gamepad inputs. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    msdossession_binding_clear(_session, target);
    [self setMapState:MSDC_MAP_ARMED];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", MsdCtlStr(msdos_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    msdossession_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    msdossession_set_controls_show_bindings(_session, show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

/* The pad menus list the sticks the game port has: Joystick B is not offered
 * with the 4-button port, and with no game port the pads are inert. */
- (void)rebuildPadMenus
{
    const int sticks = msdossession_joystick_count(_session);
    if (sticks == _menuSticks) return;
    _menuSticks = sticks;
    for (int row = 0; row < MSDC_MAX_PAD_ROWS; row++) {
        [_padAssign[row] removeAllItems];
        [_padAssign[row] addItemWithTitle:@"Automatic"];
        if (sticks >= 1) [_padAssign[row] addItemWithTitle:@"Joystick A"];
        if (sticks >= 2) [_padAssign[row] addItemWithTitle:@"Joystick B"];
        _padAssign[row].enabled = sticks > 0;
    }
    _padGeneration = (unsigned)-1;     /* the selections are read again */
}

- (void)refreshPadEffects
{
    for (int row = 0; row < _padRows; row++) {
        const int stick = msdossession_gamepad_effective_stick(_session, row);
        NSString *text = stick == 0 ? @"drives Joystick A" : stick == 1 ? @"drives Joystick B" : @"drives nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

/* Rebuilt whenever the pads' generation changes (one added or removed). */
- (void)refreshPads
{
    [self rebuildPadMenus];
    const int count = msdossession_gamepad_count(_session);
    _padGeneration = msdossession_gamepad_generation(_session);
    _padRows = count < MSDC_MAX_PAD_ROWS ? (count > 0 ? count : 0) : MSDC_MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more pads
     * are connected); the panel changes size only then */
    const int rooms = _padRows > MSDC_MIN_PAD_ROWS ? _padRows : MSDC_MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MSDC_MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MSDC_MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MSDC_MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        msdossession_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = MsdCtlStr(name);
        _padName[row].toolTip = MsdCtlStr(name);
        /* the pad's own joystick, or Automatic when it has none (or B is gone) */
        const int assign = msdossession_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= _menuSticks) ? 0 : assign + 1];
    }
    [self refreshPadEffects];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    msdossession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadEffects];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms a machine or port change and the hot-plugged pads. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= MSDC_SLOW_TICKS) {
        _slow = 0;
        [self syncLayout];
        if (msdossession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadEffects];
    }
    [_skin poll];
    [_stage poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak MSDOSControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* The app went to the background (the panel hides with it): back out of Map. */
- (void)appResigned:(NSNotification *)note
{
    (void)note;
    if (_mapState != MSDC_MAP_IDLE) [self setMapState:MSDC_MAP_IDLE];
}

/* ---- the keyboard: the main window's routing ------------------------------------- */

- (void)panelKeyDown:(NSEvent *)event
{
    if ([event modifierFlags] & NSEventModifierFlagCommand) return;
    if (_mapState == MSDC_MAP_IDLE) {
        (void)[_router routeKey:event down:YES];
        return;
    }
    /* Map: the next key is the binding; Esc backs out of the pick; nothing
     * reaches the PC */
    const uint32_t usage = MSDOSHidFromKeyCode([event keyCode]);
    if (!usage) return;
    if (_swallowKey && usage == _swallowKey) return;
    if ([event isARepeat]) return;
    if (_mapState < 0) return;      /* armed: typing does nothing */
    if (usage == MSDC_HID_ESCAPE) {
        [self setMapState:MSDC_MAP_ARMED];
        _swallowKey = usage;
    } else {
        [self bindKey:usage swallow:YES];
    }
}

- (void)panelKeyUp:(NSEvent *)event
{
    if (_mapState == MSDC_MAP_IDLE) {
        (void)[_router routeKey:event down:NO];
        return;
    }
    const uint32_t usage = MSDOSHidFromKeyCode([event keyCode]);
    if (_swallowKey && usage == _swallowKey) _swallowKey = 0;
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise they
 * are the PC's. */
- (void)panelFlagsChanged:(NSEvent *)event
{
    if (_mapState == MSDC_MAP_IDLE) {
        [_router routeFlags:event];
        return;
    }
    if (_mapState < 0) return;
    BOOL down = NO;
    const uint32_t usage = MsdCtlModifierFromFlags(event, &down);
    if (usage && down) [self bindKey:usage swallow:NO];
}

/* ---- the window ----------------------------------------------------------------- */

- (void)show
{
    [_stage configure];
    [_skin reload];
    _padGeneration = (unsigned)-1;
    [self refreshPads];
    [self setMapState:MSDC_MAP_IDLE];
    [self relayout];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
    [_skin poll];
    [_stage poll];
}

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:MSDC_MAP_IDLE];
    [self letGo];
    msdossession_release_all(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
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

/* Preferences or an import changed the machine, the game port or the pads:
 * read again. */
- (void)refresh
{
    [self syncLayout];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's holds: the pointer, the clicked key and the bound keys; the main
 * window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    msdossession_release_all(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
