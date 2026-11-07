/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (msdosvidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) wrapping that
 * buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws only
 * the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#include <string.h>

#include "msdosvidtab.h"

static const int kZoomValues[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };
static NSString *const kZoomLabels[] = { @"Fit", @"1×", @"2×", @"3×", @"4×", @"6×", @"8×", @"12×", @"16×" };
#define NZOOM 9
#define PAL_CELL 18

@class MSDOSVideoTab;

/* ---- the picture ------------------------------------------------------------------ */

@interface MSDOSVideoPicture : NSView
@property (nonatomic, assign) msdosvid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);     /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(int id);     /* a double-click's breakpoint */
- (void)sync;                                            /* the controller re-rendered */
@end

@implementation MSDOSVideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 200); }

- (void)sync
{
    int w = 0, h = 0;
    const uint32_t *px = msdosvid_tab_picture(_tab, &w, &h);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (px) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)w * h * 4);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, prov, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(prov);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(w, h);
    } else {
        const char *why = msdosvid_tab_why(_tab);
        _message = [NSString stringWithUTF8String:why ? why : "Nothing to show"];
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *a = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                             NSFontAttributeName: [NSFont systemFontOfSize:13] };
        const NSSize sz = [_message sizeWithAttributes:a];
        [_message drawAtPoint:NSMakePoint((self.bounds.size.width - sz.width) / 2,
                                          (self.bounds.size.height - sz.height) / 2) withAttributes:a];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(ctx);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextTranslateCTM(ctx, 0, _size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(ctx);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = msdosvid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5, hover[2] - 1, hover[3] - 1)];
        p.lineWidth = 1;
        [p stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1, pin[2] - 2, pin[3] - 2)];
        p.lineWidth = 2;
        [p stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)e { return [self convertPoint:e.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint p = [self local:event];
    if (msdosvid_tab_hover(_tab, (int)p.x, (int)p.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (msdosvid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self local:event];
    if (event.clickCount >= 2) {
        msdosvid_tab_hover(_tab, (int)p.x, (int)p.y);
        const int id = msdosvid_tab_break_on_pixel(_tab);
        if (_onBreak) _onBreak(id);
    } else {
        msdosvid_tab_pin(_tab, (int)p.x, (int)p.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- the palette ---------------------------------------------------------------------- */

@interface MSDOSVideoPalette : NSView
@property (nonatomic, assign) msdosvid_tab *tab;
@property (nonatomic, copy) void (^onDetail)(NSString *text);
@end

@implementation MSDOSVideoPalette {
    int _hover;
    NSTrackingArea *_area;
}

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithFrame:(NSRect)f
{
    self = [super initWithFrame:f];
    _hover = -1;
    return self;
}

- (NSSize)intrinsicContentSize
{
    const msdosvid_pal_entry *e;
    const int n = _tab ? msdosvid_tab_palette(_tab, &e) : 0, cols = n > 16 ? 16 : 8;
    const int rows = n ? (n + cols - 1) / cols : 1;
    return NSMakeSize(cols * (PAL_CELL + 2), rows * (PAL_CELL + 2));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const msdosvid_pal_entry *e;
    const int n = msdosvid_tab_palette(_tab, &e), cols = n > 16 ? 16 : 8, sel = msdosvid_tab_pixel_palette_index(_tab);
    for (int i = 0; i < n; i++) {
        const NSRect r = NSMakeRect((i % cols) * (PAL_CELL + 2), (i / cols) * (PAL_CELL + 2), PAL_CELL, PAL_CELL);
        [[NSColor colorWithCalibratedRed:((e[i].rgb >> 16) & 255) / 255.0 green:((e[i].rgb >> 8) & 255) / 255.0
                                    blue:(e[i].rgb & 255) / 255.0 alpha:1] setFill];
        NSRectFill(r);
        if (!e[i].active) {
            [[NSColor colorWithCalibratedWhite:0 alpha:0.55] setFill];
            NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
        }
        if (e[i].age > 0 && e[i].age < MSDOSVID_REG_FADE) {
            [[NSColor colorWithCalibratedRed:1 green:0.3 blue:0.9 alpha:1] setStroke];
            NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 1, 1)];
            p.lineWidth = 2;
            [p stroke];
        }
        if (i == sel || i == _hover) {
            [NSColor.labelColor setStroke];
            NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -1, -1)];
            p.lineWidth = 2;
            [p stroke];
        }
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (void)mouseMoved:(NSEvent *)event
{
    const msdosvid_pal_entry *e;
    const int n = msdosvid_tab_palette(_tab, &e), cols = n > 16 ? 16 : 8;
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int i = ((int)p.y / (PAL_CELL + 2)) * cols + (int)p.x / (PAL_CELL + 2);
    if (p.x < 0 || p.y < 0 || i < 0 || i >= n) i = -1;
    if (i != _hover) {
        _hover = i;
        [self setNeedsDisplay:YES];
    }
    if (i < 0) {
        if (_onDetail) _onDetail(@"");
        return;
    }
    NSString *used = e[i].used >= 0 ? [NSString stringWithFormat:@"%d pixels", e[i].used] : @"?";
    if (_onDetail)
        _onDetail([NSString stringWithFormat:@"#%06X  %s\nused by %@%@", e[i].rgb & 0xFFFFFF, e[i].detail, used,
                                             e[i].active ? @"" : @"  (this mode cannot reach it)"]);
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    _hover = -1;
    [self setNeedsDisplay:YES];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

enum { SecPixel, SecRegs, SecPalette, SecMemory, SecTiming };

@interface MSDOSVideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation MSDOSVideoTab {
    msdosvid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPop, *_zoomPop;
    NSButton *_ov[MSDOSVID_O_COUNT];
    NSTextField *_status, *_note, *_pixelText, *_memText, *_timingText, *_palDetail;
    MSDOSVideoPicture *_pic;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_regs;
    MSDOSVideoPalette *_pal;
    NSProgressIndicator *_beam;
    NSString *_regsSig;
    BOOL _updating;
    int _nregs;
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSTextField *monoLabel(void)
{
    NSTextField *l = [NSTextField wrappingLabelWithString:@""];
    l.font = monoFont11();
    l.selectable = YES;
    return l;
}

- (NSView *)view { return _root; }

- (instancetype)initWithSession:(msdossession *)session
{
    self = [super init];
    if (!self) return nil;
    _tab = msdosvid_tab_new(session);
    [self build];
    return self;
}

- (void)dealloc
{
    msdosvid_tab_free(_tab);
}

- (void)build
{
    int n = 0;
    const msdosvid_overlay_def *od = msdosvid_overlays(&n);

    _viewPop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < MSDOSVID_V_COUNT; i++) [_viewPop addItemWithTitle:@(msdosvid_view_name(i))];
    _viewPop.target = self;
    _viewPop.action = @selector(onView:);
    _viewPop.toolTip = @"Decoded: built from VRAM and the registers now.\nDisplay: the last finished frame (what the monitor shows).\n"
                        "Beam: the live frame, dimmed below the beam.\nValues: false colour by the value in VRAM.\n"
                        "Bit planes: the EGA/VGA planes, 0 1 over 2 3.\nCharacter set, Memory map: the font and VRAM.";
    _zoomPop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < NZOOM; i++) [_zoomPop addItemWithTitle:kZoomLabels[i]];
    _zoomPop.target = self;
    _zoomPop.action = @selector(onZoom:);
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPop,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPop]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        _ov[i] = [NSButton checkboxWithTitle:@(od[i].label) target:self action:@selector(onOverlay:)];
        _ov[i].toolTip = @(od[i].tip);
        [(i < 5 ? row1 : row2) addObject:_ov[i]];
    }
    NSStackView *ovRow1 = [NSStackView stackViewWithViews:row1], *ovRow2 = [NSStackView stackViewWithViews:row2];
    ovRow1.spacing = ovRow2.spacing = 10;

    _status = monoLabel();
    _pic = [[MSDOSVideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _pic.tab = _tab;
    __weak MSDOSVideoTab *weak = self;
    _pic.onChanged = ^{ [weak updatePixel]; };
    _pic.onBreak = ^(int id) {
        [weak setNoteText:id > 0 ? @"Breakpoint added: break on writes to that address (see Breakpoints)"
                                 : @"That pixel has no CPU address to break on"];
    };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _pic;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, ovRow1, ovRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* ---- the sections */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {
        _pixelText = monoLabel();
        NSButton *brk = [NSButton buttonWithTitle:@"Break on its writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *btns = [NSStackView stackViewWithViews:@[brk, unpin]];
        btns.spacing = 6;
        NSStackView *v = [NSStackView stackViewWithViews:@[_pixelText, btns]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [self addSection:@"Pixel" content:v];
    }
    {
        _regs = [[NSTableView alloc] init];
        const struct { NSString *id, *title; CGFloat w; } cols[4] = { { @"reg", @"Reg", 90 }, { @"name", @"Name", 150 },
                                                                      { @"val", @"Value", 50 }, { @"meaning", @"Meaning", 320 } };
        for (int i = 0; i < 4; i++) {
            NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:cols[i].id];
            c.title = cols[i].title;
            c.width = cols[i].w;
            [_regs addTableColumn:c];
        }
        _regs.dataSource = self;
        _regs.delegate = self;
        _regs.usesAlternatingRowBackgroundColors = YES;
        NSScrollView *sc = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        sc.hasVerticalScroller = YES;
        sc.documentView = _regs;
        [self addSection:@"Registers" content:sc];
    }
    {
        _pal = [[MSDOSVideoPalette alloc] initWithFrame:NSMakeRect(0, 0, 8 * (PAL_CELL + 2), 2 * (PAL_CELL + 2))];
        _pal.tab = _tab;
        _palDetail = monoLabel();
        _pal.onDetail = ^(NSString *t) { [weak setPaletteDetail:t]; };
        NSStackView *v = [NSStackView stackViewWithViews:@[_pal, _palDetail]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [self addSection:@"Palette" content:v];
    }
    {
        _memText = monoLabel();
        [self addSection:@"Memory" content:_memText];
    }
    {
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel();
        NSStackView *v = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
        [self addSection:@"Timing" content:v];
    }
    [_sections.widthAnchor constraintEqualToConstant:400].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the MSDOS_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPop selectItemAtIndex:msdosvid_tab_view(_tab)];
    for (int i = 0; i < NZOOM; i++)
        if (kZoomValues[i] == msdosvid_tab_zoom(_tab)) [_zoomPop selectItemAtIndex:i];
    for (int i = 0; i < n; i++) _ov[i].state = (msdosvid_tab_overlays(_tab) & od[i].bit) ? NSControlStateValueOn : NSControlStateValueOff;
    [_sections selectTabViewItemAtIndex:msdosvid_tab_env_section(_tab)];
    _updating = NO;
}

- (void)setNoteText:(NSString *)text
{
    _note.stringValue = text;
}

- (void)setPaletteDetail:(NSString *)text
{
    _palDetail.stringValue = text;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    msdosvid_tab_set_view(_tab, (int)_viewPop.indexOfSelectedItem);
    [self refreshView:MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    const NSInteger i = _zoomPop.indexOfSelectedItem;
    if (_updating || i < 0 || i >= NZOOM) return;
    msdosvid_tab_set_zoom(_tab, kZoomValues[i]);
    [self refreshView:MSDOSVID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int n = 0;
    const msdosvid_overlay_def *od = msdosvid_overlays(&n);
    unsigned m = 0;
    for (int i = 0; i < n; i++)
        if (_ov[i].state == NSControlStateValueOn) m |= od[i].bit;
    msdosvid_tab_set_overlays(_tab, m);
    [self refreshView:MSDOSVID_TAB_PICTURE];
}

- (void)onBreak:(id)sender
{
    (void)sender;
    const int id = msdosvid_tab_break_on_pixel(_tab);
    _note.stringValue = id > 0 ? @"Breakpoint added: break on writes to that address (see Breakpoints)"
                               : @"Pin a pixel that has a CPU address first";
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    msdosvid_tab_unpin(_tab);
    [_pic setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:MSDOSVID_TAB_TABLES];
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const msdosvid_pixel *p = msdosvid_tab_pixel(_tab);
    _pixelText.stringValue = p ? @(p->text)
                               : @"Move over the picture to inspect a pixel.\nClick to pin it; double-click to break on writes to it.";
    [_pal setNeedsDisplay:YES];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t
{
    (void)t;
    return _nregs;
}

- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    const msdosvid_reg *r;
    const int n = msdosvid_tab_regs(_tab, &r);
    if (row < 0 || row >= n) return nil;
    NSString *text;
    if ([col.identifier isEqualToString:@"reg"])
        text = r[row].port && r[row].index < 0 ? [NSString stringWithFormat:@"%s %03X", r[row].group, r[row].port]
                                               : [NSString stringWithFormat:@"%s %02X", r[row].group, r[row].index];
    else if ([col.identifier isEqualToString:@"name"])
        text = @(r[row].name);
    else if ([col.identifier isEqualToString:@"val"])
        text = [NSString stringWithFormat:@"%0*X", r[row].digits, r[row].value];
    else
        text = @(r[row].meaning);
    NSTextField *f = [t makeViewWithIdentifier:col.identifier owner:self];
    if (!f) {
        f = [NSTextField labelWithString:@""];
        f.identifier = col.identifier;
        f.font = monoFont11();
    }
    f.stringValue = text;
    const BOOL hot = r[row].age > 0 && r[row].age < MSDOSVID_REG_FADE;
    f.drawsBackground = hot;
    f.backgroundColor = hot ? [NSColor colorWithCalibratedRed:0.48 green:0.16 blue:0.42 alpha:1] : nil;
    f.textColor = hot ? NSColor.whiteColor : NSColor.labelColor;
    return f;
}

- (void)fillRegs
{
    const msdosvid_reg *r;
    const int n = msdosvid_tab_regs(_tab, &r);
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; i < n; i++)
        [sig appendFormat:@"%x%c", r[i].value, (r[i].age > 0 && r[i].age < MSDOSVID_REG_FADE) ? '*' : '.'];
    [sig appendFormat:@"%d", n];
    if ([sig isEqualToString:_regsSig]) return;
    _regsSig = sig;
    _nregs = n;
    [_regs reloadData];
}

- (void)fillMemory
{
    const msdosvid_region *r;
    const int n = msdosvid_tab_regions(_tab, &r);
    const msdos_vid_snap *sn = msdosvid_tab_snap(_tab);
    NSMutableString *s = [NSMutableString string];
    if (!n) [s appendString:@"No video memory"];
    for (int i = 0; i < n; i++) {
        [s appendFormat:@"%s\n     VRAM %05X + %05X", r[i].name, r[i].vram_off, r[i].size];
        if (r[i].cpu_addr != MSDOS_VID_NONE) [s appendFormat:@"   CPU %05X", r[i].cpu_addr];
        [s appendString:@"\n"];
    }
    if (n)
        [s appendFormat:@"\n%u KB of VRAM%@; the CPU sees it at %05X-%05X.\nSelect the Memory map view to see it all.",
                        sn->vram_size / 1024, sn->planar ? @", four planes" : @"", sn->cpu_base,
                        sn->cpu_base + (sn->cpu_size ? sn->cpu_size - 1 : 0)];
    _memText.stringValue = s;
}

- (void)fillTiming
{
    const char *const *lines;
    const int n = msdosvid_tab_timing(_tab, &lines);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        NSString *line = @(lines[i]);
        const NSRange t = [line rangeOfString:@"\t"];
        if (t.location != NSNotFound)
            [s appendFormat:@"%-13s %@\n", [[line substringToIndex:t.location] UTF8String], [line substringFromIndex:t.location + 1]];
        else
            [s appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = n ? s : @"The PC is off";
    _beam.doubleValue = msdosvid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & MSDOSVID_TAB_PICTURE) {
        const NSSize vp = _scroll.contentSize;
        if (vp.width > 4 && vp.height > 4) msdosvid_tab_set_viewport(_tab, (int)vp.width - 2, (int)vp.height - 2);
        [_pic sync];
        [_pic setFrameSize:_pic.intrinsicContentSize];
    }
    if (what & MSDOSVID_TAB_TABLES) {
        _status.stringValue = @(msdosvid_tab_status(_tab));
        switch ([_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem]) {
        case SecRegs: [self fillRegs]; break;
        case SecPalette:
            [_pal invalidateIntrinsicContentSize];
            [_pal setNeedsDisplay:YES];
            break;
        case SecMemory: [self fillMemory]; break;
        case SecTiming: [self fillTiming]; break;
        default: break;
        }
        [self updatePixel];
    }
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = msdosvid_tab_refresh(_tab, force);
    if (force) what |= MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES;
    [self refreshView:what];
}

@end
