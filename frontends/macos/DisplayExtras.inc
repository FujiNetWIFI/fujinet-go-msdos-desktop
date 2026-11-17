/*
 * DisplayExtras.inc -- the mouse, the fallback clock and the screenshot and
 * counter hooks, included into the display view's @implementation (see
 * DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit
{
    /* the picture is smoothed unless the setting says otherwise */
    atomic_store(&_smooth, 1);

    /* the pointer's movement over the picture, for a captured mouse */
    NSTrackingArea *tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:tracking];

    /* no display to follow (a headless runner): 60 Hz by the clock, on the
     * main thread, so the counters below still move */
    CVDisplayLinkRef probe = NULL;
    if (CVDisplayLinkCreateWithActiveCGDisplays(&probe) == kCVReturnSuccess && probe) {
        CVDisplayLinkRelease(probe);
    } else {
        __weak FN_VIEW *weakSelf = self;
        _fallback = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf tick:mach_absolute_time()];
        }];
        [[NSRunLoop mainRunLoop] addTimer:_fallback forMode:NSRunLoopCommonModes];
    }
}

- (void)setIntegerScale:(BOOL)on
{
    atomic_store(&s_fn_integer, on ? 1 : 0);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

/* The next refresh takes a frame even if the PC drew nothing new: its pixel
 * aspect follows a 4:3 change at once. */
- (void)refreshFrame
{
    os_unfair_lock_lock(&_fbLock);
    _serial = 0;
    os_unfair_lock_unlock(&_fbLock);
    atomic_store(&_redraw, 1);
}

- (uint64_t)frames { return atomic_load(&_frameCount); }
- (uint64_t)ticks { return _tickCount; }
- (BOOL)hasFrame { return atomic_load(&_frameCount) > 0; }

/* The last frame as a PNG (NO when there is none yet). */
- (BOOL)savePng:(NSString *)path
{
    os_unfair_lock_lock(&_fbLock);
    const int w = _width, h = _height;
    NSMutableData *copy = atomic_load(&_frameCount) > 0 && w > 0 && h > 0
        ? [NSMutableData dataWithBytes:_fb length:(size_t)w * h * 4] : nil;
    os_unfair_lock_unlock(&_fbLock);
    if (!copy) return NO;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
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
