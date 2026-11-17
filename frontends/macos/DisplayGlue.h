/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <math.h>
#include <stdatomic.h>

#include "msdossession.h"

#define FN_VIEW      MSDOSDisplayView
#define FN_SESSION   msdossession
#define FN_FB_W      MSDOSSESSION_FB_MAX_WIDTH
#define FN_FB_H      MSDOSSESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 960
#define FN_INITIAL_H 720

/* The frame follows the video mode (CGA's 640x200, VGA's 720x400 text, SVGA up
 * to 1024x768) and carries the pixel aspect the 4:3 setting asks for. */
#define FN_VARIABLE_WIDTH 1

/* Whole multiples of the picture when asked for and it fits. */
#define FN_SNAP_SIZE 1

/* The mouse (captured: hidden and pinned, sent as mickeys), the headless
 * fallback clock, and the screenshot and counter hooks (DisplayExtras.inc). */
#define FN_EXTRA_IVARS \
    NSTimer *_fallback;         /* the clock when there is no display link */ \
    BOOL _captured; \
    unsigned _buttons; \
    double _dx, _dy, _wheel;    /* fractions of a mickey / a notch, carried */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* The view's session is let go when it stops: the pointer handlers check it. */
#define FN_STOP_HOOK \
    [_fallback invalidate]; \
    _fallback = nil; \
    _session = NULL;

/* 86Box paces itself on its own thread; there is no vsync lock. */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 60; }
static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns) { (void)s; (void)ns; }

/* The last frame's pixel aspect x1000. Written by the pulling thread, read by
 * whoever draws. */
static _Atomic int s_fn_par = 1000;
static _Atomic int s_fn_integer = 0;

static inline int fn_pull_frame_w(FN_SESSION *s, uint32_t *dst, int *width, int *lines, uint64_t *serial)
{
    int w = 0, h = 0, par = 1000;
    if (!s || !msdossession_copy_frame(s, dst, &w, &h, &par, serial) || w <= 0 || h <= 0)
        return 0;
    atomic_store(&s_fn_par, par > 0 ? par : 1000);
    *width = w;
    *lines = h;
    return 1;
}

/* the frame's own shape, its pixel aspect applied */
static inline double fn_picture_aspect_w(FN_SESSION *s, int width, int lines, BOOL tv)
{
    (void)s; (void)tv;
    return (width * atomic_load(&s_fn_par) / 1000.0) / (double)lines;
}

static inline BOOL fn_snap_size(CGSize avail, int width, int lines, CGSize *out)
{
    if (!atomic_load(&s_fn_integer)) return NO;
    const double pw = width * atomic_load(&s_fn_par) / 1000.0, ph = lines;
    if (pw > avail.width || ph > avail.height) return NO;   /* does not fit at all */
    const double k = floor(fmin(avail.width / pw, avail.height / ph));
    *out = CGSizeMake(pw * k, ph * k);
    return YES;
}
