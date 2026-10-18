/*
 * MSDOSDisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * 86Box paces itself on its own thread and publishes a frame when one is
 * done; the display link says when the panel refreshes. Its callback runs
 * on its OWN thread, so it only asks the main thread to pull: frames are
 * pulled by serial (copy_frame does nothing when the PC has drawn nothing
 * new) on the main thread, where every other session call is made, as the
 * GNOME frontend's frame-clock tick does.
 *
 * The frame's size follows the video mode (CGA's 640x200, VGA's 720x400 for
 * text, SVGA up to 1024x768); its pixel aspect (x1000) is what the 4:3
 * setting asks for, so a 200-line mode is shown on a monitor's shape.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FB_PIXELS ((size_t)MSDOSSESSION_FB_MAX_WIDTH * MSDOSSESSION_FB_MAX_HEIGHT)

@implementation MSDOSDisplayView {
    msdossession *_session;
    CVDisplayLinkRef _link;
    NSTimer *_fallback;         /* the clock when there is no display link */
    int _queued;                /* a pull is waiting on the main queue (atomic) */
    uint32_t *_fb;              /* the last frame pulled (main thread) */
    uint32_t *_shown;           /* what drawRect: draws */
    int _fw, _fh, _fpar;        /* _fb's frame */
    int _w, _h, _par;           /* _shown's */
    uint64_t _serial;
    uint64_t _frames, _ticks;
    BOOL _fresh;                /* _fb holds a frame drawRect: has not taken */
    BOOL _haveFrame;
    CGColorSpaceRef _cs;
    BOOL _smooth;
    BOOL _integer;
    BOOL _captured;
    unsigned _buttons;
    double _dx, _dy, _wheel;    /* fractions of a mickey / a notch, carried */
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)now; (void)out; (void)flagsIn; (void)flagsOut;
    MSDOSDisplayView *view = (__bridge MSDOSDisplayView *)ctx;
    [view linkTick];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(msdossession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 960, 720)];
    if (!self) return nil;
    _session = session;
    _smooth = YES;
    _fb = calloc(FB_PIXELS, sizeof *_fb);
    _shown = calloc(FB_PIXELS, sizeof *_shown);
    _w = _fw = 640;
    _h = _fh = 480;
    _par = _fpar = 1000;
    _cs = CGColorSpaceCreateDeviceRGB();

    /* the pointer's movement over the picture, for a captured mouse */
    NSTrackingArea *tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:tracking];

    _link = NULL;
    if (CVDisplayLinkCreateWithActiveCGDisplays(&_link) == kCVReturnSuccess && _link) {
        CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
        CVDisplayLinkStart(_link);
    } else {
        /* no display to follow (a headless runner): 60 Hz by the clock */
        _link = NULL;
        __weak MSDOSDisplayView *weakSelf = self;
        _fallback = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf pullFrame];
        }];
        [[NSRunLoop mainRunLoop] addTimer:_fallback forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
    [_fallback invalidate];
    _fallback = nil;
    _session = NULL;
}

- (void)dealloc
{
    [self stop];
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
    free(_shown);
}

/* The display link's thread: one pull at a time waits on the main queue. */
- (void)linkTick
{
    if (__atomic_exchange_n(&_queued, 1, __ATOMIC_ACQ_REL)) return;
    dispatch_async(dispatch_get_main_queue(), ^{ [self pullFrame]; });
}

- (void)pullFrame
{
    int w = 0, h = 0, par = 1000;
    __atomic_store_n(&_queued, 0, __ATOMIC_RELEASE);
    _ticks++;
    if (!_session || !_fb) return;
    if (!msdossession_copy_frame(_session, _fb, &w, &h, &par, &_serial) || w <= 0 || h <= 0)
        return;
    _fw = w;
    _fh = h;
    _fpar = par > 0 ? par : 1000;
    _fresh = YES;
    _frames++;
    [self setNeedsDisplay:YES];
}

- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }
- (void)setIntegerScale:(BOOL)on { _integer = on; [self setNeedsDisplay:YES]; }
- (void)refreshFrame { _serial = 0; }
- (uint64_t)frames { return _frames; }
- (uint64_t)ticks { return _ticks; }
- (BOOL)hasFrame { return _haveFrame || _fresh; }

- (BOOL)isOpaque { return YES; }

- (CGImageRef)newImage
{
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit image with the alpha byte skipped reads them as they are. */
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, _shown, (size_t)_w * _h * 4, NULL);
    if (!dp) return NULL;
    CGImageRef img = CGImageCreate((size_t)_w, (size_t)_h, 8, 32, (size_t)_w * 4, _cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    return img;
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, NSRectToCGRect(b));

    if (_fresh) {
        memcpy(_shown, _fb, (size_t)_fw * _fh * sizeof *_fb);
        _w = _fw;
        _h = _fh;
        _par = _fpar;
        _fresh = NO;
        _haveFrame = YES;
    }
    if (!_haveFrame || b.size.width <= 0 || b.size.height <= 0) return;
    CGImageRef img = [self newImage];
    if (!img) return;

    /* the frame's own shape (its pixel aspect applied), fitted; whole
     * multiples of it when asked for and it fits at all */
    const double pw = _w * _par / 1000.0, ph = _h;
    const double w = b.size.width, h = b.size.height;
    double sw, sh;
    if (_integer && pw <= w && ph <= h) {
        const double k = floor(fmin(w / pw, h / ph));
        sw = pw * k;
        sh = ph * k;
    } else if (w / h > pw / ph) {
        sh = h;
        sw = sh * pw / ph;
    } else {
        sw = w;
        sh = sw * ph / pw;
    }
    const NSRect picture = NSMakeRect(floor((w - sw) / 2), floor((h - sh) / 2), sw, sh);

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, NSRectToCGRect(picture), img);
    CGImageRelease(img);
}

- (BOOL)savePng:(NSString *)path
{
    if (_fresh) {
        memcpy(_shown, _fb, (size_t)_fw * _fh * sizeof *_fb);
        _w = _fw;
        _h = _fh;
        _par = _fpar;
        _fresh = NO;
        _haveFrame = YES;
    }
    if (!_haveFrame) return NO;
    CGImageRef img = [self newImage];
    if (!img) return NO;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    CGImageRelease(img);
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

/* ---- the mouse ----------------------------------------------------------------- */

- (BOOL)captured { return _captured; }

- (void)setCaptured:(BOOL)on
{
    on = on && _session && msdossession_mouse_fitted(_session);
    if (on == _captured) return;
    _captured = on;
    _dx = _dy = _wheel = 0;
    if (on) {
        /* the pointer stays where it is, hidden; its movement is the PC's */
        [NSCursor hide];
        CGAssociateMouseAndMouseCursorPosition(false);
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
        if (_buttons && _session) {
            _buttons = 0;
            msdossession_mouse_buttons(_session, 0);
        }
        _buttons = 0;
    }
    if (self.onCaptureChanged) self.onCaptureChanged(on);
}

/* Movement while captured: AppKit's deltas count down the screen, as the
 * PC's mouse does. Fractions (a trackpad's) are carried to the next. */
- (void)motion:(NSEvent *)e
{
    if (!_captured || !_session) return;
    _dx += [e deltaX];
    _dy += [e deltaY];
    const int dx = (int)_dx, dy = (int)_dy;
    _dx -= dx;
    _dy -= dy;
    if (dx || dy) msdossession_mouse_motion(_session, dx, dy);
}

- (void)press:(unsigned)bit down:(BOOL)down
{
    if (!_captured || !_session) return;
    if (down) _buttons |= bit;
    else if (_buttons & bit) _buttons &= ~bit;
    else return;
    msdossession_mouse_buttons(_session, _buttons);
}

- (void)mouseMoved:(NSEvent *)e { [self motion:e]; }
- (void)mouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self motion:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self motion:e]; }

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (!_captured) {
        [self setCaptured:YES];   /* the click that captures is not the PC's */
        return;
    }
    [self press:1 down:YES];
}

- (void)mouseUp:(NSEvent *)e { (void)e; [self press:1 down:NO]; }
- (void)rightMouseDown:(NSEvent *)e { (void)e; [self press:2 down:YES]; }
- (void)rightMouseUp:(NSEvent *)e { (void)e; [self press:2 down:NO]; }

/* The middle button lets the mouse go (86Box's own way). */
- (void)otherMouseDown:(NSEvent *)e
{
    if (_captured && [e buttonNumber] == 2) [self setCaptured:NO];
}

- (void)scrollWheel:(NSEvent *)e
{
    if (!_captured || !_session) return;
    /* a trackpad reports points, a wheel notches; up is positive */
    _wheel += [e hasPreciseScrollingDeltas] ? [e scrollingDeltaY] / 10.0 : [e scrollingDeltaY];
    const int dz = (int)_wheel;
    _wheel -= dz;
    if (dz) msdossession_mouse_wheel(_session, dz);
}
@end
