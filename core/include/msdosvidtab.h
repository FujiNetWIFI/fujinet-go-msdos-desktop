/*
 * msdosvidtab -- the Video tab's controller. All four frontends draw the same tab from it, so
 * what they own is widgets: a view and zoom chooser, overlay toggles, a picture, the
 * inspector and four tables. Everything that was a lesson from the other debuggers' Video
 * tabs lives here once:
 *
 *   - refresh only copies on the emulation thread (msdosdebug_video_snap); decoding, scaling
 *     and overlays run on the caller's (UI) thread, never under the run lock;
 *   - the picture is rebuilt only when something it shows changed, into one reused buffer;
 *   - hover and pin are state here, so a mouse move never re-renders the picture: draw the
 *     marks from msdosvid_tab_marks() over the cached image;
 *   - pixels are 0xFFRRGGBB (opaque), whatever the toolkit does with the alpha byte;
 *   - one table of overlays and one env-var parser, so the frontends cannot disagree.
 *
 * UI thread only. Not thread-safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSVIDTAB_H
#define MSDOSVIDTAB_H

#include "msdosdebug.h"
#include "msdosvid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct msdosvid_tab msdosvid_tab;

msdosvid_tab *msdosvid_tab_new(msdossession *s);
void          msdosvid_tab_free(msdosvid_tab *t);

/* ---- what to show (set before or between refreshes; they mark the picture stale) ---- */
void     msdosvid_tab_set_view(msdosvid_tab *t, int view);        /* msdosvid_view */
int      msdosvid_tab_view(const msdosvid_tab *t);
void     msdosvid_tab_set_overlays(msdosvid_tab *t, unsigned ov);
unsigned msdosvid_tab_overlays(const msdosvid_tab *t);
/* 1..16, or 0 for "fit the viewport" */
void     msdosvid_tab_set_zoom(msdosvid_tab *t, int zoom);
int      msdosvid_tab_zoom(const msdosvid_tab *t);
/* the room the picture has, for Fit; call when the widget resizes */
void     msdosvid_tab_set_viewport(msdosvid_tab *t, int w, int h);

/*
 * The scripted-capture variables, read once by msdosvid_tab_new:
 *   MSDOS_DEBUGGER_VIDEO_VIEW      decoded display beam values planes font memory
 *   MSDOS_DEBUGGER_VIDEO_OVERLAYS  grid,cursor,beam,heat,changed,active,split,banks,digits | all | none
 *   MSDOS_DEBUGGER_VIDEO_ZOOM      1..16 or fit
 *   MSDOS_DEBUGGER_VIDEO_SECTION   pixel registers palette memory timing  (names, never indexes)
 * Returns the starting section, MSDOSVID_S_PIXEL when unset.
 */
int msdosvid_tab_env_section(const msdosvid_tab *t);

/* ---- refresh ------------------------------------------------------------------------ */
#define MSDOSVID_TAB_PICTURE 1u  /* repaint the picture (msdosvid_tab_picture) */
#define MSDOSVID_TAB_TABLES  2u  /* refill the status line, registers, palette, memory, timing */
#define MSDOSVID_TAB_OFF     4u  /* the PC is off: show msdosvid_tab_why() instead of a picture */

/* Take a snapshot, update the tracker, and say what to redraw. force: redraw everything. */
unsigned msdosvid_tab_refresh(msdosvid_tab *t, int force);

/* ---- the picture ------------------------------------------------------------------- */
/*
 * The rendered picture, 0xFFRRGGBB, valid until the next refresh or set_*. NULL when this view
 * cannot be drawn: msdosvid_tab_why() says why ("Only for the 16-colour EGA/VGA graphics
 * modes"), and the tab should show that text.
 */
const uint32_t *msdosvid_tab_picture(msdosvid_tab *t, int *w, int *h);
const char     *msdosvid_tab_why(const msdosvid_tab *t);
int             msdosvid_tab_effective_zoom(const msdosvid_tab *t);

/* ---- the tables -------------------------------------------------------------------- */
const msdos_vid_snap *msdosvid_tab_snap(const msdosvid_tab *t);
const msdosvid_info  *msdosvid_tab_info(const msdosvid_tab *t);
const char           *msdosvid_tab_status(const msdosvid_tab *t);   /* two lines, '\n' between */
int  msdosvid_tab_regs(const msdosvid_tab *t, const msdosvid_reg **out);
int  msdosvid_tab_palette(const msdosvid_tab *t, const msdosvid_pal_entry **out);
int  msdosvid_tab_regions(const msdosvid_tab *t, const msdosvid_region **out);
/* the timing section, as ready-made "label\tvalue" lines (n of them) */
int  msdosvid_tab_timing(const msdosvid_tab *t, const char *const **lines);

/* Where the beam is in the whole frame, 0..1000 (0 when unknown): for a progress bar. */
int  msdosvid_tab_beam_permille(const msdosvid_tab *t);

/* ---- hover, pin ---------------------------------------------------------------------- */
/* x, y in picture pixels; -1,-1 when the pointer left. Returns 1 when the inspector changed. */
int  msdosvid_tab_hover(msdosvid_tab *t, int x, int y);
void msdosvid_tab_pin(msdosvid_tab *t, int x, int y);     /* a click; the same spot unpins */
void msdosvid_tab_unpin(msdosvid_tab *t);
/* the pinned pixel if any, else the hovered one, else NULL */
const msdosvid_pixel *msdosvid_tab_pixel(const msdosvid_tab *t);
/* rectangles in picture pixels for the marks the frontend draws over the picture */
int  msdosvid_tab_marks(const msdosvid_tab *t, int hover[4], int pin[4]);   /* bit0 hover valid, bit1 pin valid */
/* a write breakpoint on the CPU address behind the inspected pixel; its id, or -1 */
int  msdosvid_tab_break_on_pixel(msdosvid_tab *t);
/* the palette entry the inspected pixel uses, or -1 (to outline it in the Palette section) */
int  msdosvid_tab_pixel_palette_index(const msdosvid_tab *t);

#ifdef __cplusplus
}
#endif

#endif
