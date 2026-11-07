/*
 * The Video tab, part 2: decoding VRAM into a picture, the views and overlays, and the
 * hit test. Pure C on a msdos_vid_snap.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msdosvid.h"
#include "vidint.h"

#define OPAQUE 0xFF000000u
#define NOVAL (-1)

/* ------------------------------------------------------------------------------------- */
/* the decoded picture                                                                    */
/* ------------------------------------------------------------------------------------- */

typedef struct vid_img {
    int       w, h;
    uint32_t *rgb;   /* 0xFFRRGGBB */
    uint32_t *voff;  /* VRAM byte behind each pixel */
    int16_t  *val;   /* palette index, or the attribute in text */
    int16_t  *chr;   /* text: the character, else -1 */
    int       text, cols, rows, cw, ch;
    int       xmul;  /* text: output pixels per font dot (2 in 40-column EGA/VGA text) */
} vid_img;

static inline int
is_ega_like(const msdos_vid_snap *s)
{
    return s->card == MSDOS_VID_EGA || s->card == MSDOS_VID_VGA || s->card == MSDOS_VID_TVGA;
}

static void
img_free(vid_img *g)
{
    free(g->rgb);
    free(g->voff);
    free(g->val);
    free(g->chr);
    memset(g, 0, sizeof *g);
}

static int
img_alloc(vid_img *g, int w, int h)
{
    memset(g, 0, sizeof *g);
    if (w <= 0 || h <= 0 || (size_t) w * (size_t) h > MSDOSVID_MAX_PIXELS)
        return -1;
    size_t n = (size_t) w * (size_t) h;
    g->w = w;
    g->h = h;
    g->rgb = calloc(n, 4);
    g->voff = malloc(n * 4);
    g->val = malloc(n * 2);
    g->chr = malloc(n * 2);
    if (!g->rgb || !g->voff || !g->val || !g->chr) {
        img_free(g);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        g->voff[i] = MSDOS_VID_NONE;
        g->val[i] = NOVAL;
        g->chr[i] = NOVAL;
        g->rgb[i] = OPAQUE;
    }
    return 0;
}

static inline void
put(vid_img *g, int x, int y, uint32_t rgb, uint32_t off, int val, int chr)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h)
        return;
    size_t i = (size_t) y * (size_t) g->w + (size_t) x;
    g->rgb[i] = OPAQUE | (rgb & 0xFFFFFF);
    g->voff[i] = off;
    g->val[i] = (int16_t) val;
    g->chr[i] = (int16_t) chr;
}

static inline uint8_t
vr(const msdos_vid_snap *s, uint32_t off)
{
    return s->vram_size ? s->vram[off & (s->vram_size - 1)] : 0;
}

static uint32_t
mono_color(int level)
{
    return level == 2 ? 0xFFFFFF : level == 1 ? 0xAAAAAA : 0x000000;
}

/* the colour of a 4-bit text/graphics index on this card */
static uint32_t
idx16_color(const msdos_vid_snap *s, int idx)
{
    switch (s->card) {
        case MSDOS_VID_CGA:
            return vid_rgbi(idx);
        case MSDOS_VID_PCJR:
            return vid_rgbi(s->gate[16 + (idx & s->gate[1] & 15)] & 15);
        default:
            return s->pal16[idx & 15] & 0xFFFFFF;
    }
}

/* ---- text -------------------------------------------------------------------------- */

static uint8_t
font_row(const msdos_vid_snap *s, int chr, int row, int attr)
{
    if (s->card == MSDOS_VID_MDA)
        return row < 14 ? s->font[chr * 16 + row] : 0;
    if (s->card == MSDOS_VID_CGA || s->card == MSDOS_VID_PCJR)
        return row < 8 ? s->font[chr * 16 + row] : 0;
    uint32_t base = (attr & 8) ? s->charsetb : s->charseta;
    return vr(s, base + (uint32_t) chr * 128u + (uint32_t) row * 4u);
}

static uint32_t
text_off(const msdos_vid_snap *s, const msdosvid_info *in, int cx, int cy)
{
    if (s->card == MSDOS_VID_MDA || s->card == MSDOS_VID_CGA || s->card == MSDOS_VID_PCJR) {
        uint32_t ma = (uint32_t) (s->crtc[12] << 8 | s->crtc[13]) + (uint32_t) (cy * in->cols + cx);
        uint32_t mask = s->card == MSDOS_VID_MDA ? 0xFFFu : 0x3FFFu;
        return (ma * 2u) & mask;
    }
    uint32_t start = s->start_byte;
    int      yy = cy;
    if (in->split_row > 0 && cy >= in->split_row) {
        start = 0;
        yy = cy - in->split_row;
    }
    uint32_t unit = (uint32_t) yy * s->pitch_units + (uint32_t) cx;
    return (start + unit * (s->stride ? s->stride : 4)) & (s->vram_size - 1);
}

static void
decode_text_cell(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g, int cx, int cy)
{
    uint32_t off = text_off(s, in, cx, cy);
    int      chr = vr(s, off), attr = vr(s, off + (s->card == MSDOS_VID_MDA ? 1 : 1));
    int      xm = g->text ? (g->xmul ? g->xmul : 1) : 1;
    int      cwo = in->cell_w * xm;
    uint32_t fg, bg;

    if (s->card == MSDOS_VID_MDA) {
        int f = attr & 7, b = (attr >> 4) & 7;
        int inten = (attr & 8) ? 1 : 0;
        int inv = (f == 0 && b == 7);
        int fl = (f == 7 || f == 1) ? 1 + inten : (f == 0 ? 0 : 1 + inten);
        if (inv) {
            fg = mono_color(0);
            bg = mono_color(1 + inten);
        } else {
            fg = mono_color(fl);
            bg = mono_color(b ? 1 : 0);
        }
        if (s->mode_reg & 0x20 && (attr & 0x80) && !inv) {
            /* a blinking cell: shown steady here */
        }
        for (int r = 0; r < in->cell_h; r++) {
            int bits = font_row(s, chr, r, attr);
            int ul = (f == 1 && r == in->cell_h - 2);
            for (int d = 0; d < 9; d++) {
                int on;
                if (d < 8)
                    on = (bits >> (7 - d)) & 1;
                else
                    on = (chr >= 0xC0 && chr <= 0xDF) ? (bits & 1) : 0;
                put(g, cx * cwo + d, cy * in->cell_h + r, (on || ul) ? fg : bg, off, attr, chr);
            }
        }
        return;
    }

    int fgi = attr & 15, bgi = attr >> 4;
    if (in->blink)
        bgi &= 7;
    fg = idx16_color(s, fgi);
    bg = idx16_color(s, bgi);
    int line_gfx = is_ega_like(s) ? ((s->attr[0x10] & 4) != 0) : 0;
    for (int r = 0; r < in->cell_h; r++) {
        int bits = font_row(s, chr, r, attr);
        for (int d = 0; d < in->cell_w; d++) {
            int on;
            if (d < 8)
                on = (bits >> (7 - d)) & 1;
            else
                on = (line_gfx && chr >= 0xC0 && chr <= 0xDF) ? (bits & 1) : 0;
            for (int k = 0; k < xm; k++)
                put(g, cx * cwo + d * xm + k, cy * in->cell_h + r, on ? fg : bg, off, attr, chr);
        }
    }
}

/* ---- graphics ---------------------------------------------------------------------- */

static uint32_t
row_start(const msdos_vid_snap *s, const msdosvid_info *in, int y)
{
    uint32_t start = s->start_byte;
    int      yy = y;
    if (in->split_row > 0 && y >= in->split_row) {
        start = 0;
        yy = y - in->split_row;
    }
    return start + (uint32_t) yy * s->pitch_units * (s->stride ? s->stride : 4);
}

static void
decode_planar16(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g, int y0, int y1)
{
    uint32_t stride = s->stride ? s->stride : 4;
    for (int y = y0; y < y1; y++) {
        uint32_t rs = row_start(s, in, y);
        for (int x = 0; x < g->w; x++) {
            uint32_t a = (rs + (uint32_t) (x >> 3) * stride) & (s->vram_size - 1);
            int      bit = 7 - (x & 7);
            int      idx = ((vr(s, a) >> bit) & 1) | (((vr(s, a + 1) >> bit) & 1) << 1) |
                      (((vr(s, a + 2) >> bit) & 1) << 2) | (((vr(s, a + 3) >> bit) & 1) << 3);
            put(g, x, y, s->pal16[idx] & 0xFFFFFF, a, idx, NOVAL);
        }
    }
}

static void
decode_packed8(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        uint32_t rs = row_start(s, in, y);
        for (int x = 0; x < g->w; x++) {
            uint32_t a = (rs + (uint32_t) (x >> 2) * (s->stride ? s->stride : 4) + (uint32_t) (x & 3)) &
                         (s->vram_size - 1);
            int idx = vr(s, a);
            put(g, x, y, s->pal256[idx] & 0xFFFFFF, a, idx, NOVAL);
        }
    }
}

/* a CPU byte address in the CGA's B8000 window -> an offset into our copy */
static uint32_t
cga_cpu_to_vram(const msdos_vid_snap *s, uint32_t a)
{
    if (is_ega_like(s))
        return (((a >> 1) << 2) | (a & 1)) & (s->vram_size - 1);
    return a & (s->vram_size - 1);
}

static void
decode_cgalike(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g, int y0, int y1)
{
    int      pj = s->card == MSDOS_VID_PCJR;
    int      ega = is_ega_like(s);
    int      banks = in->bank_count > 0 ? in->bank_count : 1;
    int      sel = pj ? vid_pcjr_select(s) : 0;
    uint32_t rowbytes = ega ? s->pitch_units * 2u : (uint32_t) s->crtc[1] * 2u;
    uint32_t startA = ega ? (s->start_byte / (s->stride ? s->stride : 4)) * 2u :
                            (uint32_t) (s->crtc[12] << 8 | s->crtc[13]) * 2u;
    uint32_t cga4[4];
    vid_cga_gfx_palette(s, cga4);

    for (int y = y0; y < y1; y++) {
        int      bank = y % banks, rin = y / banks;
        uint32_t base = startA + (uint32_t) rin * rowbytes;
        for (int x = 0; x < g->w; x++) {
            uint32_t a, rgb;
            int      idx;
            switch (in->mode) {
                case MSDOS_VM_GFX_4: {
                    a = (uint32_t) bank * 0x2000u + ((base + (uint32_t) (x >> 2) * 2u) & 0x1FFFu);
                    int dat = (vr(s, a) << 8) | vr(s, a + 1);
                    idx = (dat >> (12 - 4 * (x & 3))) & 15;
                    rgb = vid_rgbi(s->gate[16 + (idx & s->gate[1] & 15)] & 15);
                    break;
                }
                case MSDOS_VM_GFX_1: {
                    a = (uint32_t) bank * 0x2000u + ((base + (uint32_t) (x >> 3)) & 0x1FFFu);
                    idx = (vr(s, a) >> (7 - (x & 7))) & 1;
                    rgb = pj ? vid_rgbi(s->gate[16 + idx] & 15) :
                               (idx ? vid_rgbi(s->color_reg & 15) : 0);
                    break;
                }
                default: { /* GFX_2 */
                    if (pj && sel == 0x03) {
                        a = (uint32_t) bank * 0x2000u + ((base + (uint32_t) (x >> 3) * 2u) & 0x1FFFu);
                        int bit = 7 - (x & 7);
                        idx = (((vr(s, a + 1) >> bit) & 1) << 1) | ((vr(s, a) >> bit) & 1);
                        rgb = vid_rgbi(s->gate[16 + (idx & s->gate[1] & 15)] & 15);
                    } else {
                        uint32_t cpu = (uint32_t) bank * 0x2000u +
                                       ((base + (uint32_t) (x >> 2)) & 0x1FFFu);
                        a = cga_cpu_to_vram(s, cpu);
                        idx = (vr(s, a) >> (6 - 2 * (x & 3))) & 3;
                        rgb = pj    ? vid_rgbi(s->gate[16 + idx] & 15) :
                              ega   ? (s->pal16[idx] & 0xFFFFFF) :
                                      cga4[idx];
                    }
                    break;
                }
            }
            put(g, x, y, rgb, a, idx, NOVAL);
        }
    }
}

static int
decode_picture(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g)
{
    switch (in->mode) {
        case MSDOS_VM_TEXT: {
            int xm = (is_ega_like(s) && (s->seq[1] & 8)) ? 2 : 1;
            if (img_alloc(g, in->cols * in->cell_w * xm, in->rows * in->cell_h) < 0)
                return -1;
            g->text = 1;
            g->cols = in->cols;
            g->rows = in->rows;
            g->cw = in->cell_w * xm;
            g->ch = in->cell_h;
            g->xmul = xm;
            for (int cy = 0; cy < in->rows; cy++)
                for (int cx = 0; cx < in->cols; cx++)
                    decode_text_cell(s, in, g, cx, cy);
            return 0;
        }
        case MSDOS_VM_PLANAR_16:
            if (img_alloc(g, in->width, in->height) < 0)
                return -1;
            decode_planar16(s, in, g, 0, g->h);
            return 0;
        case MSDOS_VM_PACKED_8:
            if (img_alloc(g, in->width, in->height) < 0)
                return -1;
            decode_packed8(s, in, g, 0, g->h);
            return 0;
        case MSDOS_VM_GFX_1:
        case MSDOS_VM_GFX_2:
        case MSDOS_VM_GFX_4:
            if (img_alloc(g, in->width, in->height) < 0)
                return -1;
            decode_cgalike(s, in, g, 0, g->h);
            return 0;
        default:
            return -1;
    }
}

/* ------------------------------------------------------------------------------------- */
/* the other pictures                                                                     */
/* ------------------------------------------------------------------------------------- */

static uint32_t
blend(uint32_t c, uint32_t t, int a)
{
    int r = (((c >> 16) & 255) * (255 - a) + ((t >> 16) & 255) * a) / 255;
    int g = (((c >> 8) & 255) * (255 - a) + ((t >> 8) & 255) * a) / 255;
    int b = (((c) & 255) * (255 - a) + ((t) & 255) * a) / 255;
    return OPAQUE | (uint32_t) (r << 16 | g << 8 | b);
}

/* a distinct colour for each value: 16 hand-picked, then golden-angle hues */
static uint32_t
false_color(int v)
{
    static const uint32_t t[16] = { 0x202020, 0x3A6DF0, 0xE03030, 0x30C050, 0xE0C030, 0xB040E0,
                                    0x30D0D0, 0xF08030, 0x707070, 0x90B0FF, 0xFF9090, 0x90F0A0,
                                    0xFFF090, 0xE0A0FF, 0xA0F0F0, 0xFFC090 };
    if (v < 0)
        return 0x101010;
    if (v < 16)
        return t[v];
    double h = (double) ((int) (v * 137.508) % 360) / 60.0;
    int    i = (int) h;
    double f = h - i, q = 1.0 - f;
    double sat = (v & 1) ? 0.55 : 0.85, val = (v & 2) ? 0.95 : 0.7;
    double p = val * (1 - sat), qq = val * (1 - sat * f), tt = val * (1 - sat * q);
    double r, g, b;
    switch (i % 6) {
        case 0:  r = val; g = tt; b = p; break;
        case 1:  r = qq; g = val; b = p; break;
        case 2:  r = p; g = val; b = tt; break;
        case 3:  r = p; g = qq; b = val; break;
        case 4:  r = tt; g = p; b = val; break;
        default: r = val; g = p; b = qq; break;
    }
    return ((uint32_t) (r * 255) << 16) | ((uint32_t) (g * 255) << 8) | (uint32_t) (b * 255);
}

/* the other views build on a decoded picture ------------------------------------------ */

static const uint8_t g_digit[16][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 },
    { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 }, { 2, 5, 7, 5, 5 }, { 6, 5, 6, 5, 6 },
    { 3, 4, 4, 4, 3 }, { 6, 5, 5, 5, 6 }, { 7, 4, 6, 4, 7 }, { 7, 4, 6, 4, 4 },
};

static void
draw_hex(uint32_t *dst, int W, int H, int x, int y, unsigned v, int nd, int sc, uint32_t col)
{
    for (int d = 0; d < nd; d++) {
        int g = (int) ((v >> (4 * (nd - 1 - d))) & 15);
        int ox = x + d * 4 * sc;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if ((g_digit[g][r] >> (2 - c)) & 1)
                    for (int yy = 0; yy < sc; yy++)
                        for (int xx = 0; xx < sc; xx++) {
                            int px = ox + c * sc + xx, py = y + r * sc + yy;
                            if (px >= 0 && py >= 0 && px < W && py < H)
                                dst[(size_t) py * (size_t) W + (size_t) px] = col;
                        }
    }
}

static uint32_t
contrast(uint32_t c)
{
    int l = (int) (((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100;
    return l > 110 ? (OPAQUE | 0x000000) : (OPAQUE | 0xFFFFFF);
}

/* the views other than Decoded: transform a decoded picture, or build one */
static int
build_values(const vid_img *src, vid_img *g)
{
    if (img_alloc(g, src->w, src->h) < 0)
        return -1;
    g->text = src->text;
    g->cols = src->cols;
    g->rows = src->rows;
    g->cw = src->cw;
    g->ch = src->ch;
    g->xmul = src->xmul;
    size_t n = (size_t) src->w * (size_t) src->h;
    for (size_t i = 0; i < n; i++) {
        g->rgb[i] = OPAQUE | false_color(src->val[i]);
        g->voff[i] = src->voff[i];
        g->val[i] = src->val[i];
        g->chr[i] = src->chr[i];
    }
    return 0;
}

static const uint32_t g_plane_color[4] = { 0x4A7CFF, 0x3FD05A, 0xFF4A4A, 0xF0E060 };

static int
build_planes(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g)
{
    const int gap = 4;
    int       w = in->width, h = in->height;
    if (img_alloc(g, w * 2 + gap, h * 2 + gap) < 0)       /* planes 0 1 over 2 3 */
        return -1;
    for (size_t i = 0; i < (size_t) g->w * (size_t) g->h; i++)
        g->rgb[i] = OPAQUE | 0x14141C;
    uint32_t stride = s->stride ? s->stride : 4;
    for (int p = 0; p < 4; p++)
        for (int y = 0; y < h; y++) {
            uint32_t rs = row_start(s, in, y);
            for (int x = 0; x < w; x++) {
                uint32_t a = (rs + (uint32_t) (x >> 3) * stride + (uint32_t) p) & (s->vram_size - 1);
                int      bit = (vr(s, a) >> (7 - (x & 7))) & 1;
                put(g, (p & 1) * (w + gap) + x, (p >> 1) * (h + gap) + y, bit ? g_plane_color[p] : 0x14141C, a, bit,
                    NOVAL);
            }
        }
    return 0;
}

static int
build_font(const msdos_vid_snap *s, const msdosvid_info *in, vid_img *g)
{
    int cw = in->cell_w ? in->cell_w : 8, chh = in->cell_h ? in->cell_h : 8;
    if (in->mode != MSDOS_VM_TEXT) {
        /* not in text mode: show the BIOS character set the card would use */
        if (s->card == MSDOS_VID_MDA) {
            cw = 9;
            chh = 14;
        } else if (is_ega_like(s)) {
            cw = 8;
            chh = 16;
        } else {
            cw = 8;
            chh = 8;
        }
    }
    if (img_alloc(g, 16 * (cw + 1) + 1, 16 * (chh + 1) + 1) < 0)
        return -1;
    for (size_t i = 0; i < (size_t) g->w * (size_t) g->h; i++)
        g->rgb[i] = OPAQUE | 0x202028;
    for (int c = 0; c < 256; c++) {
        int ox = 1 + (c & 15) * (cw + 1), oy = 1 + (c >> 4) * (chh + 1);
        for (int r = 0; r < chh; r++) {
            int      bits = font_row(s, c, r, 0);
            uint32_t off = is_ega_like(s) ? s->charseta + (uint32_t) c * 128u + (uint32_t) r * 4u :
                                            (uint32_t) (c * 16 + r);
            for (int d = 0; d < cw; d++) {
                int on = d < 8 ? (bits >> (7 - d)) & 1 :
                                 ((c >= 0xC0 && c <= 0xDF) ? (bits & 1) : 0);
                put(g, ox + d, oy + r, on ? 0xF0F0F0 : 0x000000, off, on, c);
            }
        }
    }
    return 0;
}

static int
build_memory(const msdos_vid_snap *s, vid_img *g)
{
    if (!s->vram_size)
        return -1;
    int      planar = s->planar != 0;
    uint32_t units = planar ? s->vram_size / 4 : s->vram_size;
    uint32_t grp = units <= 65536 ? 1 : units <= 262144 ? 4 : 16;
    int      W = 256;
    uint32_t pxp = (units + grp - 1) / grp;
    int      rows = (int) ((pxp + (uint32_t) W - 1) / (uint32_t) W);
    int      np = planar ? 4 : 1, gap = 4;
    int      gcols = planar ? 2 : 1, grows = planar ? 2 : 1;       /* the planes in a 2x2 grid */
    if (img_alloc(g, W * gcols + gap * (gcols - 1), rows * grows + gap * (grows - 1)) < 0)
        return -1;
    for (size_t i = 0; i < (size_t) g->w * (size_t) g->h; i++)
        g->rgb[i] = OPAQUE | 0x0A0A0E;
    msdosvid_region rg[12];
    int             nr = msdosvid_regions(s, rg, 12);
    for (int p = 0; p < np; p++)
        for (uint32_t k = 0; k < pxp; k++) {
            uint32_t u0 = k * grp, mx = 0, off0 = planar ? (u0 << 2) | (uint32_t) p : u0;
            for (uint32_t u = u0; u < u0 + grp && u < units; u++) {
                uint32_t o = planar ? (u << 2) | (uint32_t) p : u;
                uint8_t  v = s->vram[o];
                if (v > mx)
                    mx = v;
            }
            uint32_t base = 0x303038;
            for (int r = 0; r < nr; r++) {
                uint32_t bo = planar ? (u0 << 2) : u0;
                if (bo >= rg[r].vram_off && bo < rg[r].vram_off + rg[r].size)
                    base = rg[r].color;
            }
            uint32_t col = blend(OPAQUE | base, OPAQUE | 0xFFFFFF, (int) (mx * 120 / 255));
            if (!mx)
                col = blend(OPAQUE | base, OPAQUE, 140);
            put(g, (p % gcols) * (W + gap) + (int) (k % (uint32_t) W),
                (p / gcols) * (rows + gap) + (int) (k / (uint32_t) W), col, off0, (int) mx, NOVAL);
        }
    return 0;
}

static int
build_plain(const uint32_t *px, int w, int h, vid_img *g)
{
    memset(g, 0, sizeof *g);
    if (!px || w <= 0 || h <= 0 || (size_t) w * (size_t) h > MSDOSVID_MAX_PIXELS)
        return -1;
    g->w = w;
    g->h = h;
    g->rgb = malloc((size_t) w * (size_t) h * 4);
    if (!g->rgb)
        return -1;
    for (size_t i = 0; i < (size_t) w * (size_t) h; i++)
        g->rgb[i] = OPAQUE | (px[i] & 0xFFFFFF);
    return 0;
}

/* ---- the cache ---------------------------------------------------------------------- */

static struct {
    uint64_t      token;
    int           view, valid;
    vid_img       g;
    msdosvid_info in;
} g_cache;

static void
cache_drop(void)
{
    img_free(&g_cache.g);
    g_cache.valid = 0;
}

static int
beam_view_ok(const msdos_vid_snap *s)
{
    return s->fb && s->fb_w > 0 && s->fb_h > 0;
}

const char *
msdosvid_view_why(const msdosvid_ctx *c, int view)
{
    const msdos_vid_snap *s = c->snap;
    msdosvid_info         in;
    if (!s || s->card == MSDOS_VID_OFF)
        return "The PC is off";
    msdosvid_info_get(s, &in);
    switch (view) {
        case MSDOSVID_V_DISPLAY:
            return c->display && c->display_w > 0 ? NULL : "No finished frame yet";
        case MSDOSVID_V_BEAM:
            return beam_view_ok(s) ? NULL : "The live frame is not available";
        case MSDOSVID_V_DECODED:
        case MSDOSVID_V_VALUES:
            return in.mode == MSDOS_VM_UNKNOWN || in.mode == MSDOS_VM_OFF ?
                       "This mode is not decoded; see the Display view" : NULL;
        case MSDOSVID_V_PLANES:
            return in.mode == MSDOS_VM_PLANAR_16 ? NULL : "Only for the 16-colour EGA/VGA graphics modes";
        case MSDOSVID_V_FONT:
            return s->card == MSDOS_VID_OFF ? "The PC is off" : NULL;
        case MSDOSVID_V_MEMORY:
            return s->vram_size ? NULL : "No video memory";
        default:
            return "?";
    }
}

int
msdosvid_view_available(const msdos_vid_snap *s, int view)
{
    msdosvid_ctx c;
    memset(&c, 0, sizeof c);
    c.snap = s;
    uint32_t dummy = 0;
    c.display = &dummy;
    c.display_w = 1;
    c.display_h = 1;
    return msdosvid_view_why(&c, view) == NULL;
}

static const vid_img *
get_base(const msdosvid_ctx *c, int view, const msdosvid_info **inout)
{
    if (c->token && g_cache.valid && g_cache.token == c->token && g_cache.view == view) {
        *inout = &g_cache.in;
        return &g_cache.g;
    }
    cache_drop();
    if (!c->snap || msdosvid_view_why(c, view))
        return NULL;
    const msdos_vid_snap *s = c->snap;
    msdosvid_info_get(s, &g_cache.in);
    vid_img tmp;
    int     rc = -1;
    memset(&tmp, 0, sizeof tmp);
    switch (view) {
        case MSDOSVID_V_DECODED:
            rc = decode_picture(s, &g_cache.in, &g_cache.g);
            break;
        case MSDOSVID_V_VALUES:
            if (decode_picture(s, &g_cache.in, &tmp) == 0) {
                rc = build_values(&tmp, &g_cache.g);
                img_free(&tmp);
            }
            break;
        case MSDOSVID_V_PLANES:
            rc = build_planes(s, &g_cache.in, &g_cache.g);
            break;
        case MSDOSVID_V_FONT:
            rc = build_font(s, &g_cache.in, &g_cache.g);
            break;
        case MSDOSVID_V_MEMORY:
            rc = build_memory(s, &g_cache.g);
            break;
        case MSDOSVID_V_DISPLAY:
            rc = build_plain(c->display, c->display_w, c->display_h, &g_cache.g);
            break;
        case MSDOSVID_V_BEAM: {
            rc = build_plain(s->fb, s->fb_w, s->fb_h, &g_cache.g);
            if (rc == 0 && s->beam_row >= 0)
                for (int y = s->beam_row + 1; y < g_cache.g.h; y++)
                    for (int x = 0; x < g_cache.g.w; x++) {
                        size_t i = (size_t) y * (size_t) g_cache.g.w + (size_t) x;
                        g_cache.g.rgb[i] = blend(g_cache.g.rgb[i], OPAQUE, 150);
                    }
            break;
        }
        default:
            break;
    }
    if (rc < 0) {
        cache_drop();
        return NULL;
    }
    g_cache.token = c->token;
    g_cache.view = view;
    g_cache.valid = c->token != 0;
    *inout = &g_cache.in;
    return &g_cache.g;
}

/* ------------------------------------------------------------------------------------- */
/* geometry                                                                               */
/* ------------------------------------------------------------------------------------- */

static int
eff_zoom(int w, int h, int zoom)
{
    if (zoom < 1)
        zoom = 1;
    if (zoom > 16)
        zoom = 16;
    while (zoom > 1 && (size_t) w * (size_t) zoom * (size_t) h * (size_t) zoom > MSDOSVID_MAX_PIXELS)
        zoom--;
    return zoom;
}

int
msdosvid_usage(const msdosvid_ctx *c, int *counts, int n)
{
    const msdosvid_info *in;
    const vid_img       *g = get_base(c, MSDOSVID_V_DECODED, &in);
    memset(counts, 0, (size_t) n * sizeof *counts);
    if (!g || !g->val)
        return -1;
    if (g->text) {
        /* one count per cell, at its top-left pixel */
        for (int cy = 0; cy < g->rows; cy++)
            for (int cx = 0; cx < g->cols; cx++) {
                int a = g->val[(size_t) (cy * g->ch) * (size_t) g->w + (size_t) (cx * g->cw)];
                if (a < 0)
                    continue;
                int fg = a & 15, bg = (a >> 4) & (in->blink ? 7 : 15);
                if (fg < n)
                    counts[fg]++;
                if (bg < n && bg != fg)
                    counts[bg]++;
            }
        return 0;
    }
    for (size_t i = 0; i < (size_t) g->w * (size_t) g->h; i++) {
        int v = g->val[i];
        if (v >= 0 && v < n)
            counts[v]++;
    }
    return 0;
}

int
msdosvid_fit_zoom(const msdosvid_ctx *c, int view, int aw, int ah, int maxz)
{
    const msdosvid_info *in;
    const vid_img       *g = get_base(c, view, &in);
    if (!g)
        return 1;
    int z = 1;
    for (int k = 1; k <= maxz && k <= 16; k++)
        if (g->w * k <= aw && g->h * k <= ah)
            z = k;
    return eff_zoom(g->w, g->h, z);
}

/* the image row the beam is on, for the decoded-style views; -1 when it is blanking */
static int
beam_img_row(const msdos_vid_snap *s, const msdosvid_info *in)
{
    int blank;
    if (s->card == MSDOS_VID_MDA || s->card == MSDOS_VID_CGA || s->card == MSDOS_VID_PCJR)
        blank = (s->crtc[4] + 1 - s->crtc[7]) * ((s->crtc[9] & 0x1F) + 1) + s->crtc[5];
    else
        blank = s->vtotal > s->vsyncstart ? s->vtotal - s->vsyncstart : 0;
    int line = s->displine - blank;
    if (line < 0)
        return -1;
    int row = line;
    if (is_ega_like(s) && in->mode != MSDOS_VM_TEXT) {
        int per = ((s->crtc[9] & 0x1F) + 1) * ((s->crtc[9] & 0x80) ? 2 : 1);
        row = line / (per ? per : 1);
    }
    return row;
}

static int
has_meta(int view)
{
    return view != MSDOSVID_V_DISPLAY && view != MSDOSVID_V_BEAM;
}

/* ------------------------------------------------------------------------------------- */
/* render                                                                                 */
/* ------------------------------------------------------------------------------------- */

static void
hline_blend(uint32_t *dst, int W, int H, int y, int x0, int x1, uint32_t col, int a)
{
    if (y < 0 || y >= H)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 > W)
        x1 = W;
    for (int x = x0; x < x1; x++) {
        uint32_t *p = &dst[(size_t) y * (size_t) W + (size_t) x];
        *p = blend(*p, col, a);
    }
}

static void
vline_blend(uint32_t *dst, int W, int H, int x, int y0, int y1, uint32_t col, int a)
{
    if (x < 0 || x >= W)
        return;
    if (y0 < 0)
        y0 = 0;
    if (y1 > H)
        y1 = H;
    for (int y = y0; y < y1; y++) {
        uint32_t *p = &dst[(size_t) y * (size_t) W + (size_t) x];
        *p = blend(*p, col, a);
    }
}

static void
rect_outline(uint32_t *dst, int W, int H, int x, int y, int w, int h, uint32_t col, int a)
{
    hline_blend(dst, W, H, y, x, x + w, col, a);
    hline_blend(dst, W, H, y + h - 1, x, x + w, col, a);
    vline_blend(dst, W, H, x, y, y + h, col, a);
    vline_blend(dst, W, H, x + w - 1, y, y + h, col, a);
}

static int
heat_of(const msdosvid_ctx *c, uint32_t voff, int span)
{
    const msdosvid_tracker *t = c->track;
    if (!t || !t->heat || voff == MSDOS_VID_NONE || voff + (uint32_t) span > t->heat_n)
        return 0;
    int m = 0;
    for (int i = 0; i < span; i++)
        if (t->heat[voff + (uint32_t) i] > m)
            m = t->heat[voff + (uint32_t) i];
    return m;
}

static int
changed_of(const msdosvid_ctx *c, uint32_t voff, int span)
{
    const msdosvid_tracker *t = c->track;
    if (!t || !t->changed || voff == MSDOS_VID_NONE || voff + (uint32_t) span > t->heat_n)
        return 0;
    for (int i = 0; i < span; i++)
        if (t->changed[voff + (uint32_t) i])
            return 1;
    return 0;
}

int
msdosvid_render(const msdosvid_ctx *c, int view, unsigned ov, int zoom, uint32_t *dst, size_t cap,
                int *ow, int *oh)
{
    const msdosvid_info *in = NULL;
    const vid_img       *g = get_base(c, view, &in);
    if (ow)
        *ow = 0;
    if (oh)
        *oh = 0;
    if (!g)
        return -1;
    const msdos_vid_snap *s = c->snap;
    int                   z = eff_zoom(g->w, g->h, zoom);
    int                   W = g->w * z, H = g->h * z;
    if (ow)
        *ow = W;
    if (oh)
        *oh = H;
    if (!dst)
        return 0;
    if ((size_t) W * (size_t) H > cap)
        return -1;

    int meta = has_meta(view) && g->voff;
    int span = g->text ? 2 : 1;
    int heat_on = (ov & MSDOSVID_O_HEAT) && meta && view != MSDOSVID_V_FONT;
    int chg_on = (ov & MSDOSVID_O_CHANGED) && meta && view != MSDOSVID_V_FONT;
    int banks = (ov & MSDOSVID_O_BANKS) && view == MSDOSVID_V_DECODED ? in->bank_count : 1;

    for (int sy = 0; sy < g->h; sy++) {
        for (int sx = 0; sx < g->w; sx++) {
            size_t   i = (size_t) sy * (size_t) g->w + (size_t) sx;
            uint32_t col = g->rgb[i];
            if (meta) {
                uint32_t vo = g->voff[i];
                if (heat_on) {
                    int h = heat_of(c, vo, span);
                    if (h)
                        col = blend(col, 0xFF6A00, 12 + h * 160 / 255);
                }
                if (chg_on && changed_of(c, vo, span))
                    col = blend(col, 0xFF00FF, 150);
            }
            if (banks > 1 && (sy % banks) & 1)
                col = blend(col, 0x4060FF, 40);
            for (int dy = 0; dy < z; dy++) {
                uint32_t *row = &dst[(size_t) (sy * z + dy) * (size_t) W + (size_t) (sx * z)];
                for (int dx = 0; dx < z; dx++)
                    row[dx] = col;
            }
        }
    }

    /* grid */
    if (ov & MSDOSVID_O_GRID) {
        if (g->text && meta && view != MSDOSVID_V_FONT && view != MSDOSVID_V_MEMORY) {
            for (int cx = 1; cx < g->cols; cx++)
                vline_blend(dst, W, H, cx * g->cw * z, 0, H, 0xFFFFFF, 50);
            for (int cy = 1; cy < g->rows; cy++)
                hline_blend(dst, W, H, cy * g->ch * z, 0, W, 0xFFFFFF, 50);
        } else if (z >= 4) {
            for (int x = 1; x < g->w; x++)
                vline_blend(dst, W, H, x * z, 0, H, 0xFFFFFF, 40);
            for (int y = 1; y < g->h; y++)
                hline_blend(dst, W, H, y * z, 0, W, 0xFFFFFF, 40);
        }
    }

    /* cursor */
    if ((ov & MSDOSVID_O_CURSOR) && view == MSDOSVID_V_DECODED && in->cursor_x >= 0 && g->text) {
        int top = in->cursor_top < in->cell_h ? in->cursor_top : in->cell_h - 1;
        int bot = in->cursor_bottom < in->cell_h ? in->cursor_bottom : in->cell_h - 1;
        int x0 = in->cursor_x * g->cw * z, y0 = (in->cursor_y * g->ch + top) * z;
        int cw = g->cw * z, chh = (bot - top + 1) * z;
        if (in->cursor_visible)
            for (int y = 0; y < chh; y++)
                hline_blend(dst, W, H, y0 + y, x0, x0 + cw, 0x00E5FF, 150);
        else
            rect_outline(dst, W, H, x0, in->cursor_y * g->ch * z, cw, g->ch * z, 0x00E5FF, 140);
    }

    /* split screen */
    if ((ov & MSDOSVID_O_SPLIT) && view == MSDOSVID_V_DECODED && in->split_row > 0) {
        int y = in->split_row * (g->text ? g->ch : 1) * z;
        hline_blend(dst, W, H, y, 0, W, 0xFFA030, 220);
    }

    /* active area */
    if (ov & MSDOSVID_O_ACTIVE) {
        if (view == MSDOSVID_V_DISPLAY || view == MSDOSVID_V_BEAM) {
            msdosvid_info d;
            msdosvid_info_get(s, &d);
            int pw = d.width, ph = d.height;
            if (pw > 0 && ph > 0 && pw <= g->w && ph <= g->h)
                rect_outline(dst, W, H, ((g->w - pw) / 2) * z, ((g->h - ph) / 2) * z, pw * z, ph * z,
                             0x00E5FF, 200);
        } else {
            rect_outline(dst, W, H, 0, 0, W, H, 0x00E5FF, 200);
        }
    }

    /* the beam */
    if (ov & MSDOSVID_O_BEAM) {
        if (view == MSDOSVID_V_BEAM && s->beam_row >= 0 && s->beam_row < g->h) {
            for (int k = 0; k < (z < 2 ? 2 : z); k++)
                hline_blend(dst, W, H, s->beam_row * z + k, 0, W, 0xFF2020, 255);
        } else if (view == MSDOSVID_V_DECODED || view == MSDOSVID_V_VALUES) {
            int row = beam_img_row(s, in);
            if (row >= 0 && row < g->h)
                for (int k = 0; k < (z < 2 ? 2 : z); k++)
                    hline_blend(dst, W, H, row * z + k, 0, W, 0xFF2020, 255);
        } else if (view == MSDOSVID_V_PLANES) {
            int row = beam_img_row(s, in);
            if (row >= 0 && row < g->h)
                for (int k = 0; k < (z < 2 ? 2 : z); k++)
                    hline_blend(dst, W, H, row * z + k, 0, W, 0xFF2020, 255);
        }
    }

    /* the value on each pixel or cell */
    if ((ov & MSDOSVID_O_DIGITS) && meta && view != MSDOSVID_V_FONT) {
        if (g->text && view != MSDOSVID_V_MEMORY) {
            int cw = g->cw * z, chh = g->ch * z;
            if (cw >= 9) {
                int sc = 1;
                while ((sc + 1) * 7 <= cw - 2 && (sc + 1) * 5 * 2 + 3 <= chh && sc < 4)
                    sc++;
                for (int cy = 0; cy < g->rows; cy++)
                    for (int cx = 0; cx < g->cols; cx++) {
                        size_t i = (size_t) (cy * g->ch) * (size_t) g->w + (size_t) (cx * g->cw);
                        if (g->chr[i] < 0)
                            continue;
                        uint32_t fg = contrast(dst[(size_t) (cy * chh + chh / 2) * (size_t) W + (size_t) (cx * cw + 1)]);
                        int two = chh >= 2 * 5 * sc + 3;
                        draw_hex(dst, W, H, cx * cw + 1, cy * chh + 1, (unsigned) g->chr[i], 2, sc, fg);
                        if (two)
                            draw_hex(dst, W, H, cx * cw + 1, cy * chh + 2 + 5 * sc, (unsigned) g->val[i], 2, sc, fg);
                    }
            }
        } else {
            int nd = (view == MSDOSVID_V_MEMORY || (s->card >= MSDOS_VID_VGA && in->colors > 16) ||
                      in->mode == MSDOS_VM_PACKED_8) ? 2 : 1;
            int need = nd == 1 ? 7 : 11;
            if (z >= need) {
                int sc = 1;
                while ((sc + 1) * (nd * 4 - 1) + 2 <= z && (sc + 1) * 5 + 2 <= z && sc < 4)
                    sc++;
                for (int sy = 0; sy < g->h; sy++)
                    for (int sx = 0; sx < g->w; sx++) {
                        size_t i = (size_t) sy * (size_t) g->w + (size_t) sx;
                        if (g->val[i] < 0)
                            continue;
                        uint32_t bgc = dst[(size_t) (sy * z + 1) * (size_t) W + (size_t) (sx * z + 1)];
                        draw_hex(dst, W, H, sx * z + (z - (nd * 4 - 1) * sc) / 2,
                                 sy * z + (z - 5 * sc) / 2, (unsigned) g->val[i], nd, sc, contrast(bgc));
                    }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------- */
/* hit test                                                                               */
/* ------------------------------------------------------------------------------------- */

static uint32_t
cpu_addr_of(const msdos_vid_snap *s, uint32_t voff)
{
    if (voff == MSDOS_VID_NONE || !s->cpu_base)
        return MSDOS_VID_NONE;
    switch (s->card) {
        case MSDOS_VID_MDA:
            return s->cpu_base + (voff & 0xFFF);
        case MSDOS_VID_CGA:
            return s->cpu_base + (voff & 0x3FFF);
        case MSDOS_VID_PCJR:
            return ((s->memctrl >> 3) & 7) == (s->memctrl & 7) ? s->cpu_base + voff : MSDOS_VID_NONE;
        default:
            if (s->seq[4] & 8)
                return s->cpu_base + voff;
            if (!(s->seq[4] & 4) && (voff & 3) < 2)
                return s->cpu_base + ((voff >> 2) << 1) + (voff & 1);
            return s->cpu_base + (voff >> 2);
    }
}

static const char *
rgbi_name(int i)
{
    static const char *const n[16] = { "black", "blue", "green", "cyan", "red", "magenta", "brown",
                                       "light grey", "dark grey", "light blue", "light green",
                                       "light cyan", "light red", "light magenta", "yellow",
                                       "white" };
    return n[i & 15];
}

int
msdosvid_cell_rect(const msdosvid_ctx *c, int view, int zoom, int x, int y, int *rx, int *ry,
                   int *rw, int *rh)
{
    const msdosvid_info *in;
    const vid_img       *g = get_base(c, view, &in);
    if (!g)
        return -1;
    int z = eff_zoom(g->w, g->h, zoom);
    int sx = x / z, sy = y / z;
    if (x < 0 || y < 0 || sx >= g->w || sy >= g->h)
        return -1;
    if (g->text && has_meta(view) && view != MSDOSVID_V_FONT && view != MSDOSVID_V_MEMORY) {
        *rx = (sx / g->cw) * g->cw * z;
        *ry = (sy / g->ch) * g->ch * z;
        *rw = g->cw * z;
        *rh = g->ch * z;
    } else {
        *rx = sx * z;
        *ry = sy * z;
        *rw = z;
        *rh = z;
    }
    return 0;
}

int
msdosvid_hit(const msdosvid_ctx *c, int view, int zoom, int x, int y, msdosvid_pixel *o)
{
    const msdosvid_info *in;
    const vid_img       *g = get_base(c, view, &in);
    memset(o, 0, sizeof *o);
    o->col = o->row = o->chr = o->value = -1;
    o->voff = o->cpu_addr = o->linear = MSDOS_VID_NONE;
    if (!g)
        return -1;
    int z = eff_zoom(g->w, g->h, zoom);
    int sx = x / z, sy = y / z;
    if (x < 0 || y < 0 || sx >= g->w || sy >= g->h)
        return -1;
    const msdos_vid_snap *s = c->snap;
    size_t                i = (size_t) sy * (size_t) g->w + (size_t) sx;
    o->x = sx;
    o->y = sy;
    o->rgb = g->rgb[i] & 0xFFFFFF;
    char *p = o->text;
    int   n = 0, cap = (int) sizeof o->text;
#define PUT(...)                                                                    \
    do {                                                                             \
        if (n < cap - 1) {                                                          \
            int w_ = snprintf(p + n, (size_t) (cap - n), __VA_ARGS__);              \
            n += w_ > 0 ? (w_ < cap - n ? w_ : cap - n - 1) : 0;                    \
        }                                                                            \
    } while (0)

    if (!has_meta(view) || !g->voff) {
        PUT("x %d  y %d\n#%06X", sx, sy, o->rgb);
        return 0;
    }
    o->voff = g->voff[i];
    o->value = g->val[i];
    o->chr = g->chr[i];
    o->cpu_addr = cpu_addr_of(s, o->voff);
    o->linear = o->cpu_addr;
    o->heat = heat_of(c, o->voff, g->text ? 2 : 1);

    if (view == MSDOSVID_V_MEMORY) {
        PUT("VRAM byte %05X = %02X\n", o->voff, o->value);
        if (o->cpu_addr != MSDOS_VID_NONE)
            PUT("CPU %04X:%04X (%05X)\n", o->cpu_addr >> 4, o->cpu_addr & 15, o->cpu_addr);
    } else if (view == MSDOSVID_V_FONT) {
        PUT("Character %02X", o->chr);
        if (o->chr >= 32 && o->chr < 127)
            PUT("  '%c'", o->chr);
        PUT("\nrow byte at VRAM/ROM %05X", o->voff);
    } else if (g->text) {
        o->col = sx / g->cw;
        o->row = sy / g->ch;
        int attr = o->value;
        PUT("col %d  row %d  (x %d, y %d)\n", o->col, o->row, sx, sy);
        PUT("char %02X", o->chr);
        if (o->chr >= 32 && o->chr < 127)
            PUT(" '%c'", o->chr);
        PUT("   attr %02X\n", attr);
        if (s->card == MSDOS_VID_MDA)
            PUT("%s%s%s\n", (attr & 7) == 1 ? "underline " : "", (attr & 8) ? "bright " : "",
                (attr & 0x80) ? "blink" : "");
        else
            PUT("fg %X %s on bg %X %s%s\n", attr & 15, rgbi_name(attr & 15), (attr >> 4) & (in->blink ? 7 : 15),
                rgbi_name((attr >> 4) & (in->blink ? 7 : 15)), (in->blink && (attr & 0x80)) ? " blink" : "");
        PUT("#%06X (cell centre)\n", o->rgb);
    } else {
        PUT("x %d  y %d\n", sx, sy);
        if (view == MSDOSVID_V_PLANES)
            PUT("plane bit %d\n", o->value);
        else if (s->card >= MSDOS_VID_VGA && in->mode == MSDOS_VM_PACKED_8)
            PUT("pixel %02X -> DAC %02X,%02X,%02X\n", o->value, s->dac[o->value & 255][0],
                s->dac[o->value & 255][1], s->dac[o->value & 255][2]);
        else if (s->card >= MSDOS_VID_EGA)
            PUT("pixel %X -> attr reg -> colour %02X\n", o->value, s->egapal[o->value & 15]);
        else
            PUT("pixel value %d\n", o->value);
        PUT("#%06X\n", o->rgb);
    }
    if (view != MSDOSVID_V_MEMORY) {
        PUT("VRAM %05X", o->voff);
        if (o->cpu_addr != MSDOS_VID_NONE)
            PUT("  CPU %04X:%04X", o->cpu_addr >> 4, o->cpu_addr & 15);
        PUT("\n");
    }
    PUT("heat %d/255", o->heat);
    return 0;
#undef PUT
}
