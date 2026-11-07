/*
 * The Video tab's controller: see msdosvidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msdosvidtab.h"

#define MAX_REGS 260
#define MAX_PAL 256
#define MAX_REG_ROWS 12
#define TIMING_LINES 16

struct msdosvid_tab {
    msdossession    *s;
    msdosdebug      *d;
    msdosvid_tracker track;
    int              have_snap;

    int      view;
    unsigned overlays;
    int      zoom;           /* 0 = fit */
    int      vp_w, vp_h;
    int      env_section;

    /* the display frame, kept for the Display view */
    uint32_t *disp;
    size_t    disp_cap;
    int       disp_w, disp_h;
    uint64_t  disp_serial;

    /* the picture */
    uint32_t *pix;
    size_t    pix_cap;
    int       pix_w, pix_h;
    int       pix_valid;     /* pix holds the current view */
    int       stale;
    const char *why;
    uint64_t  token;         /* names the snapshot for the render cache */
    int       eff_zoom;

    /* tables */
    msdosvid_info      info;
    char               status[400];
    msdosvid_reg       regs[MAX_REGS];
    int                n_regs;
    msdosvid_pal_entry pal[MAX_PAL];
    int                n_pal;
    msdosvid_region    regions[MAX_REG_ROWS];
    int                n_regions;
    char               timing_buf[TIMING_LINES][96];
    const char        *timing_ptr[TIMING_LINES];
    int                n_timing;

    /* hover / pin, in picture pixels */
    int            hx, hy, px, py;
    int            has_hover, has_pin;
    msdosvid_pixel hover_px, pin_px;
    int            hover_ok, pin_ok;

    int last_beam_row;
};

static msdosvid_ctx
ctx_of(const msdosvid_tab *t)
{
    msdosvid_ctx c;
    memset(&c, 0, sizeof c);
    c.snap = &t->track.cur;
    c.track = &t->track;
    c.display = t->disp;
    c.display_w = t->disp_w;
    c.display_h = t->disp_h;
    c.token = t->token;
    return c;
}

msdosvid_tab *
msdosvid_tab_new(msdossession *s)
{
    msdosvid_tab *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->s = s;
    t->d = msdossession_debugger(s);
    msdosvid_tracker_init(&t->track);
    t->view = MSDOSVID_V_DECODED;
    t->overlays = msdosvid_overlay_defaults();
    t->zoom = 0;
    t->vp_w = 640;
    t->vp_h = 400;
    t->stale = 1;
    t->token = 1;
    t->last_beam_row = -2;

    const char *e = getenv("MSDOS_DEBUGGER_VIDEO_VIEW");
    if (e && msdosvid_view_from_key(e) >= 0)
        t->view = msdosvid_view_from_key(e);
    e = getenv("MSDOS_DEBUGGER_VIDEO_OVERLAYS");
    if (e)
        t->overlays = msdosvid_overlays_from_keys(e);
    e = getenv("MSDOS_DEBUGGER_VIDEO_ZOOM");
    if (e && *e && strcmp(e, "fit"))
        t->zoom = atoi(e) < 0 ? 0 : atoi(e) > 16 ? 16 : atoi(e);
    e = getenv("MSDOS_DEBUGGER_VIDEO_SECTION");
    t->env_section = e && msdosvid_section_from_key(e) >= 0 ? msdosvid_section_from_key(e) : MSDOSVID_S_PIXEL;
    return t;
}

void
msdosvid_tab_free(msdosvid_tab *t)
{
    if (!t)
        return;
    msdosvid_tracker_free(&t->track);
    free(t->disp);
    free(t->pix);
    free(t);
}

int
msdosvid_tab_env_section(const msdosvid_tab *t)
{
    return t->env_section;
}

static void
touch(msdosvid_tab *t)
{
    t->stale = 1;
    t->token++;
}

void
msdosvid_tab_set_view(msdosvid_tab *t, int v)
{
    if (v >= 0 && v < MSDOSVID_V_COUNT && v != t->view) {
        t->view = v;
        t->has_hover = 0;
        t->has_pin = 0;
        touch(t);
    }
}

int
msdosvid_tab_view(const msdosvid_tab *t)
{
    return t->view;
}

void
msdosvid_tab_set_overlays(msdosvid_tab *t, unsigned ov)
{
    if (ov != t->overlays) {
        t->overlays = ov;
        touch(t);
    }
}

unsigned
msdosvid_tab_overlays(const msdosvid_tab *t)
{
    return t->overlays;
}

void
msdosvid_tab_set_zoom(msdosvid_tab *t, int z)
{
    if (z < 0)
        z = 0;
    if (z > 16)
        z = 16;
    if (z != t->zoom) {
        t->zoom = z;
        t->has_hover = 0;
        touch(t);
    }
}

int
msdosvid_tab_zoom(const msdosvid_tab *t)
{
    return t->zoom;
}

void
msdosvid_tab_set_viewport(msdosvid_tab *t, int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if ((w != t->vp_w || h != t->vp_h) && t->zoom == 0) {
        t->vp_w = w;
        t->vp_h = h;
        touch(t);
    } else {
        t->vp_w = w;
        t->vp_h = h;
    }
}

/* ---- tables ------------------------------------------------------------------------ */

static void
fill_timing(msdosvid_tab *t)
{
    const msdos_vid_snap *s = &t->track.cur;
    const msdosvid_info  *in = &t->info;
    int                   n = 0;
#define LINE(...)                                                                            \
    do {                                                                                     \
        if (n < TIMING_LINES) {                                                              \
            snprintf(t->timing_buf[n], sizeof t->timing_buf[n], __VA_ARGS__);                \
            t->timing_ptr[n] = t->timing_buf[n];                                             \
            n++;                                                                             \
        }                                                                                    \
    } while (0)
    if (s->card == MSDOS_VID_OFF) {
        t->n_timing = 0;
        return;
    }
    if (in->dot_mhz > 0)
        LINE("Dot clock\t%.3f MHz", in->dot_mhz);
    if (in->line_khz > 0)
        LINE("Line rate\t%.2f kHz", in->line_khz);
    if (in->frame_hz > 0)
        LINE("Frame rate\t%.1f Hz", in->frame_hz);
    LINE("Picture\t%d x %d, %d colours", in->width, in->height, in->colors);
    int total = s->vtotal ? s->vtotal : (s->crtc[4] + 1) * ((s->crtc[9] & 0x1F) + 1);
    LINE("Beam\tline %d of %d", s->displine, total);
    LINE("Row\t%d, scan line %d", s->vc, s->scanline);
    LINE("Display\t%s%s", s->dispon ? "on" : "off (vertical blank)", s->linepos ? ", end of line" : "");
    LINE("Vertical\t%d total, %d shown", total, s->dispend);
    LINE("V sync at\tline %d", s->vsyncstart);
    LINE("Start\t%04X  (byte %05X)", s->crtc[12] << 8 | s->crtc[13], s->start_byte);
    LINE("Row pitch\t%u units x %u bytes%s", s->pitch_units, s->stride, s->addr_exact ? "" : "  (guessed)");
    if (in->cursor_x >= 0)
        LINE("Cursor\tcol %d, row %d, lines %d-%d%s", in->cursor_x, in->cursor_y, in->cursor_top,
             in->cursor_bottom, in->cursor_visible ? "" : "  (hidden)");
    if (in->split_row >= 0)
        LINE("Split\tfrom row %d", in->split_row);
    if (s->render[0])
        LINE("86Box draws\t%s", s->render);
    if (in->bios_mode >= 0)
        LINE("BIOS mode\t%02Xh (a guess)", in->bios_mode);
    t->n_timing = n;
#undef LINE
}

static void
fill_tables(msdosvid_tab *t)
{
    const msdos_vid_snap *s = &t->track.cur;
    msdosvid_info_get(s, &t->info);
    snprintf(t->status, sizeof t->status, "%s\n%s", t->info.line1, t->info.line2);
    t->n_regs = msdosvid_regs(s, &t->track, t->regs, MAX_REGS);
    t->n_pal = msdosvid_palette(s, &t->track, t->pal, MAX_PAL);
    t->n_regions = msdosvid_regions(s, t->regions, MAX_REG_ROWS);
    fill_timing(t);
}

/* how many pixels each palette entry takes in the Decoded picture */
static void
count_palette_use(msdosvid_tab *t)
{
    int counts[256];
    for (int i = 0; i < t->n_pal; i++)
        t->pal[i].used = -1;
    msdosvid_ctx c = ctx_of(t);
    if (t->n_pal == 0 || msdosvid_usage(&c, counts, 256) != 0)
        return;
    for (int i = 0; i < t->n_pal; i++)
        t->pal[i].used = counts[t->pal[i].index & 255];
}

/* ---- refresh ----------------------------------------------------------------------- */

static int
heat_active(const msdosvid_tab *t)
{
    const msdosvid_tracker *k = &t->track;
    for (uint32_t i = 0; i < k->heat_n; i++)
        if (k->heat[i])
            return 1;
    return 0;
}

static int
beam_row_of(const msdosvid_tab *t)
{
    return t->track.cur.displine;
}

unsigned
msdosvid_tab_refresh(msdosvid_tab *t, int force)
{
    msdos_vid_snap fresh;
    unsigned       out = 0;
    memset(&fresh, 0, sizeof fresh);
    int rc = msdosdebug_video_snap(t->d, &fresh);
    if (rc != 0) {
        int was = t->have_snap && t->track.cur.card != MSDOS_VID_OFF;
        msdos_vid_snap_free(&fresh);
        if (was || force || !t->have_snap) {
            msdos_vid_snap off;
            memset(&off, 0, sizeof off);
            msdosvid_tracker_push(&t->track, &off);
            t->have_snap = 1;
            fill_tables(t);
            t->why = "The PC is off";
            t->pix_valid = 0;
            touch(t);
            return MSDOSVID_TAB_OFF | MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES;
        }
        return MSDOSVID_TAB_OFF;
    }
    msdosvid_tracker_push(&t->track, &fresh);
    t->have_snap = 1;

    /* the finished frame, for the Display view */
    uint64_t serial = t->disp_serial;
    int      dw = t->disp_w, dh = t->disp_h, par = 0;
    if (!t->disp) {
        t->disp_cap = 800 * 600;
        t->disp = malloc(t->disp_cap * 4);
        serial = 0;
    }
    int new_frame = 0;
    if (t->disp) {
        int got = msdossession_copy_frame_n(t->s, t->disp, t->disp_cap, &dw, &dh, &par, &serial);
        if (!got && dw > 0 && dh > 0 && (size_t) dw * (size_t) dh > t->disp_cap) {
            size_t   need = (size_t) dw * (size_t) dh;
            uint32_t *nb = realloc(t->disp, need * 4);
            if (nb) {
                t->disp = nb;
                t->disp_cap = need;
                got = msdossession_copy_frame_n(t->s, t->disp, t->disp_cap, &dw, &dh, &par, &serial);
            }
        }
        if (got) {
            t->disp_w = dw;
            t->disp_h = dh;
            t->disp_serial = serial;
            new_frame = 1;
        }
    }

    const msdosvid_tracker *k = &t->track;
    int regs_changed = 0;
    for (int g = 0; g < 3 && !regs_changed; g++)
        for (int i = 0; i < 256; i++)
            if (k->reg_age[g][i] == 1) {
                regs_changed = 1;
                break;
            }
    int mem_changed = k->changed_bytes > 0;
    int hot = heat_active(t);
    int beam_moved = beam_row_of(t) != t->last_beam_row;
    t->last_beam_row = beam_row_of(t);
    int beam_matters = (t->overlays & MSDOSVID_O_BEAM) || t->view == MSDOSVID_V_BEAM;

    if (force || regs_changed || mem_changed || hot || new_frame || (beam_moved && beam_matters) ||
        !t->have_snap) {
        touch(t);
        out |= MSDOSVID_TAB_PICTURE;
    }
    if (force || regs_changed || mem_changed || beam_moved || k->refreshes <= 2 || hot) {
        fill_tables(t);
        count_palette_use(t);
        out |= MSDOSVID_TAB_TABLES;
    }
    /* the pixel under the pointer follows the new picture (the byte it shows may have changed) */
    if (out & MSDOSVID_TAB_PICTURE) {
        t->pix_valid = 0;
        msdosvid_ctx c = ctx_of(t);
        if (t->has_hover)
            t->hover_ok = msdosvid_hit(&c, t->view, t->eff_zoom ? t->eff_zoom : 1, t->hx * (t->eff_zoom ? t->eff_zoom : 1), t->hy * (t->eff_zoom ? t->eff_zoom : 1), &t->hover_px) == 0;
        if (t->has_pin)
            t->pin_ok = msdosvid_hit(&c, t->view, t->eff_zoom ? t->eff_zoom : 1, t->px * (t->eff_zoom ? t->eff_zoom : 1), t->py * (t->eff_zoom ? t->eff_zoom : 1), &t->pin_px) == 0;
    }
    return out;
}

/* ---- the picture ------------------------------------------------------------------- */

const char *
msdosvid_tab_why(const msdosvid_tab *t)
{
    if (!t->have_snap || t->track.cur.card == MSDOS_VID_OFF)
        return "The PC is off";
    msdosvid_ctx c = ctx_of(t);
    return msdosvid_view_why(&c, t->view);
}

int
msdosvid_tab_effective_zoom(const msdosvid_tab *t)
{
    return t->eff_zoom ? t->eff_zoom : 1;
}

const uint32_t *
msdosvid_tab_picture(msdosvid_tab *t, int *w, int *h)
{
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!t->have_snap || t->track.cur.card == MSDOS_VID_OFF)
        return NULL;
    msdosvid_ctx c = ctx_of(t);
    if (msdosvid_view_why(&c, t->view))
        return NULL;
    if (t->pix_valid && !t->stale) {
        if (w)
            *w = t->pix_w;
        if (h)
            *h = t->pix_h;
        return t->pix;
    }
    int zoom = t->zoom;
    if (zoom == 0)
        zoom = msdosvid_fit_zoom(&c, t->view, t->vp_w, t->vp_h, 8);
    int pw, ph;
    if (msdosvid_render(&c, t->view, t->overlays, zoom, NULL, 0, &pw, &ph) != 0)
        return NULL;
    size_t need = (size_t) pw * (size_t) ph;
    if (need > t->pix_cap) {
        uint32_t *nb = realloc(t->pix, need * 4);
        if (!nb)
            return NULL;
        t->pix = nb;
        t->pix_cap = need;
    }
    if (msdosvid_render(&c, t->view, t->overlays, zoom, t->pix, t->pix_cap, &pw, &ph) != 0)
        return NULL;
    t->pix_w = pw;
    t->pix_h = ph;
    t->pix_valid = 1;
    t->stale = 0;
    /* the zoom the renderer really used (it clamps to its pixel cap) */
    {
        int bw, bh;
        msdosvid_render(&c, t->view, 0, 1, NULL, 0, &bw, &bh);
        t->eff_zoom = bw > 0 ? pw / bw : 1;
        if (t->eff_zoom < 1)
            t->eff_zoom = 1;
    }
    if (w)
        *w = pw;
    if (h)
        *h = ph;
    return t->pix;
}

/* ---- tables -------------------------------------------------------------------------- */

const msdos_vid_snap *
msdosvid_tab_snap(const msdosvid_tab *t)
{
    return &t->track.cur;
}

const msdosvid_info *
msdosvid_tab_info(const msdosvid_tab *t)
{
    return &t->info;
}

const char *
msdosvid_tab_status(const msdosvid_tab *t)
{
    return t->status;
}

int
msdosvid_tab_regs(const msdosvid_tab *t, const msdosvid_reg **out)
{
    *out = t->regs;
    return t->n_regs;
}

int
msdosvid_tab_palette(const msdosvid_tab *t, const msdosvid_pal_entry **out)
{
    *out = t->pal;
    return t->n_pal;
}

int
msdosvid_tab_regions(const msdosvid_tab *t, const msdosvid_region **out)
{
    *out = t->regions;
    return t->n_regions;
}

int
msdosvid_tab_timing(const msdosvid_tab *t, const char *const **lines)
{
    *lines = t->timing_ptr;
    return t->n_timing;
}

int
msdosvid_tab_beam_permille(const msdosvid_tab *t)
{
    const msdos_vid_snap *s = &t->track.cur;
    int total = s->vtotal ? s->vtotal : (s->crtc[4] + 1) * ((s->crtc[9] & 0x1F) + 1);
    if (s->card == MSDOS_VID_OFF || total <= 0)
        return 0;
    int v = (int) ((int64_t) s->displine * 1000 / total);
    return v < 0 ? 0 : v > 1000 ? 1000 : v;
}

/* ---- hover, pin ------------------------------------------------------------------- */

int
msdosvid_tab_hover(msdosvid_tab *t, int x, int y)
{
    if (x < 0 || y < 0) {
        int had = t->has_hover;
        t->has_hover = 0;
        t->hover_ok = 0;
        return had;
    }
    msdosvid_ctx c = ctx_of(t);
    int          z = t->eff_zoom ? t->eff_zoom : 1;
    msdosvid_pixel np;
    int ok = msdosvid_hit(&c, t->view, z, x, y, &np) == 0;
    int changed = !t->has_hover || !ok || np.x != t->hover_px.x || np.y != t->hover_px.y ||
                  strcmp(np.text, t->hover_px.text) != 0;
    t->has_hover = ok;
    t->hover_ok = ok;
    if (ok) {
        t->hover_px = np;
        t->hx = np.x;
        t->hy = np.y;
    }
    return changed;
}

void
msdosvid_tab_pin(msdosvid_tab *t, int x, int y)
{
    msdosvid_ctx c = ctx_of(t);
    int          z = t->eff_zoom ? t->eff_zoom : 1;
    msdosvid_pixel np;
    if (msdosvid_hit(&c, t->view, z, x, y, &np) != 0)
        return;
    if (t->has_pin && np.x == t->pin_px.x && np.y == t->pin_px.y) {
        t->has_pin = 0;
        return;
    }
    t->has_pin = 1;
    t->pin_ok = 1;
    t->pin_px = np;
    t->px = np.x;
    t->py = np.y;
}

void
msdosvid_tab_unpin(msdosvid_tab *t)
{
    t->has_pin = 0;
}

const msdosvid_pixel *
msdosvid_tab_pixel(const msdosvid_tab *t)
{
    if (t->has_pin && t->pin_ok)
        return &t->pin_px;
    if (t->has_hover && t->hover_ok)
        return &t->hover_px;
    return NULL;
}

int
msdosvid_tab_marks(const msdosvid_tab *t, int hover[4], int pin[4])
{
    int          m = 0;
    msdosvid_ctx c = ctx_of(t);
    int          z = t->eff_zoom ? t->eff_zoom : 1;
    if (t->has_hover && t->hover_ok &&
        msdosvid_cell_rect(&c, t->view, z, t->hx * z, t->hy * z, &hover[0], &hover[1], &hover[2], &hover[3]) == 0)
        m |= 1;
    if (t->has_pin && t->pin_ok &&
        msdosvid_cell_rect(&c, t->view, z, t->px * z, t->py * z, &pin[0], &pin[1], &pin[2], &pin[3]) == 0)
        m |= 2;
    return m;
}

int
msdosvid_tab_break_on_pixel(msdosvid_tab *t)
{
    const msdosvid_pixel *p = msdosvid_tab_pixel(t);
    if (!p || p->cpu_addr == MSDOS_VID_NONE)
        return -1;
    return msdosdebug_bp_add(t->d, MSDOS_BP_WRITE, p->cpu_addr, p->cpu_addr + (p->chr >= 0 ? 1u : 0u), -1);
}

int
msdosvid_tab_pixel_palette_index(const msdosvid_tab *t)
{
    const msdosvid_pixel *p = msdosvid_tab_pixel(t);
    if (!p || p->value < 0 || t->view == MSDOSVID_V_MEMORY || t->view == MSDOSVID_V_FONT ||
        t->view == MSDOSVID_V_PLANES)
        return -1;
    if (p->chr >= 0)
        return p->value & 15;
    return p->value;
}
