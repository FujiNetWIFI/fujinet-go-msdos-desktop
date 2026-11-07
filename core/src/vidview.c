/*
 * The Video tab, part 1: snapshot housekeeping, the tracker (heat, changed), the mode
 * description, registers, palette, memory regions and the console text.
 * Pure C on a msdos_vid_snap; no 86Box headers.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msdosvid.h"
#include "vidint.h"

#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------------------------------------- */
/* snapshot housekeeping                                                                  */
/* ------------------------------------------------------------------------------------- */

void
msdos_vid_snap_free(msdos_vid_snap *s)
{
    if (!s)
        return;
    free(s->vram);
    free(s->fb);
    memset(s, 0, sizeof *s);
}

int
msdos_vid_snap_copy(msdos_vid_snap *dst, const msdos_vid_snap *src)
{
    uint8_t  *v = NULL;
    uint32_t *f = NULL;
    if (src->vram && src->vram_size) {
        v = malloc(src->vram_size);
        if (!v)
            return -1;
        memcpy(v, src->vram, src->vram_size);
    }
    if (src->fb && src->fb_w > 0 && src->fb_h > 0) {
        f = malloc((size_t) src->fb_w * (size_t) src->fb_h * 4);
        if (!f) {
            free(v);
            return -1;
        }
        memcpy(f, src->fb, (size_t) src->fb_w * (size_t) src->fb_h * 4);
    }
    msdos_vid_snap_free(dst);
    *dst      = *src;
    dst->vram = v;
    dst->fb   = f;
    return 0;
}

void
msdos_vid_snap_move(msdos_vid_snap *dst, msdos_vid_snap *src)
{
    if (dst == src)
        return;
    msdos_vid_snap_free(dst);
    *dst = *src;
    memset(src, 0, sizeof *src);
}

/* ------------------------------------------------------------------------------------- */
/* the tracker                                                                            */
/* ------------------------------------------------------------------------------------- */

void
msdosvid_tracker_init(msdosvid_tracker *t)
{
    memset(t, 0, sizeof *t);
}

void
msdosvid_tracker_free(msdosvid_tracker *t)
{
    msdos_vid_snap_free(&t->cur);
    msdos_vid_snap_free(&t->prev);
    free(t->heat);
    free(t->changed);
    memset(t, 0, sizeof *t);
}

static void
age_bytes(uint8_t *ages, const uint8_t *a, const uint8_t *b, int n, int have_prev)
{
    for (int i = 0; i < n; i++) {
        if (have_prev && a[i] != b[i])
            ages[i] = 1;
        else if (ages[i] && ages[i] < 250)
            ages[i]++;
    }
}

/* the slot a register's age lives in: group 0 CRTC, 1 SEQ(0-63)+GDC(64-79), 2 the rest */
static void
age_registers(msdosvid_tracker *t, const msdos_vid_snap *cur, const msdos_vid_snap *prev, int hp)
{
    age_bytes(t->reg_age[0], cur->crtc, prev->crtc, 256, hp);
    uint8_t a[80], b[80];
    memcpy(a, cur->seq, 64);
    memcpy(a + 64, cur->gdc, 16);
    memcpy(b, prev->seq, 64);
    memcpy(b + 64, prev->gdc, 16);
    age_bytes(t->reg_age[1], a, b, 80, hp);
    uint8_t c[80], d[80];
    memset(c, 0, sizeof c);
    memset(d, 0, sizeof d);
    memcpy(c, cur->attr, 32);
    memcpy(d, prev->attr, 32);
    c[32] = cur->miscout;
    d[32] = prev->miscout;
    c[33] = cur->mode_reg;
    d[33] = prev->mode_reg;
    c[34] = cur->color_reg;
    d[34] = prev->color_reg;
    c[35] = cur->memctrl;
    d[35] = prev->memctrl;
    c[36] = cur->dac_mask;
    d[36] = prev->dac_mask;
    memcpy(c + 40, cur->gate, 32);
    memcpy(d + 40, prev->gate, 32);
    age_bytes(t->reg_age[2], c, d, 80, hp);
}

void
msdosvid_tracker_push(msdosvid_tracker *t, msdos_vid_snap *fresh)
{
    msdos_vid_snap_move(&t->prev, &t->cur);
    t->have_prev = t->prev.card != MSDOS_VID_OFF;
    msdos_vid_snap_move(&t->cur, fresh);
    t->refreshes++;
    t->changed_bytes = 0;

    const msdos_vid_snap *c = &t->cur, *p = &t->prev;
    int hp = t->have_prev && c->card == p->card;
    age_registers(t, c, p, hp);
    for (int i = 0; i < 256; i++) {
        if (hp && (c->pal256[i] != p->pal256[i] || (i < 16 && c->pal16[i] != p->pal16[i])))
            t->pal_age[i] = 1;
        else if (t->pal_age[i] && t->pal_age[i] < 250)
            t->pal_age[i]++;
    }

    /* VRAM: a byte that differs from last time is hot; hot bytes cool 20% per refresh */
    int same = hp && c->vram_size == p->vram_size && c->vram_phys == p->vram_phys &&
               c->cpu_base == p->cpu_base;
    if (t->heat_n != c->vram_size) {
        free(t->heat);
        free(t->changed);
        t->heat    = c->vram_size ? calloc(c->vram_size, 1) : NULL;
        t->changed = c->vram_size ? calloc(c->vram_size, 1) : NULL;
        t->heat_n  = t->heat && t->changed ? c->vram_size : 0;
        if (!t->heat_n) {
            free(t->heat);
            free(t->changed);
            t->heat = t->changed = NULL;
        }
        same = 0;
    }
    if (!t->heat_n || !c->vram)
        return;
    for (uint32_t i = 0; i < t->heat_n; i++) {
        t->heat[i] = (uint8_t) (t->heat[i] * 4 / 5);
        int ch = same && c->vram[i] != p->vram[i];
        t->changed[i] = (uint8_t) ch;
        if (ch) {
            t->heat[i] = MSDOSVID_HEAT_MAX;
            t->changed_bytes++;
        }
    }
}

/* ------------------------------------------------------------------------------------- */
/* names, keys, overlays                                                                  */
/* ------------------------------------------------------------------------------------- */

static const msdosvid_overlay_def g_overlays[MSDOSVID_O_COUNT] = {
    { MSDOSVID_O_GRID, "grid", "Grid", "Character cells; pixels once zoomed in", 0 },
    { MSDOSVID_O_CURSOR, "cursor", "Cursor", "The text cursor", 1 },
    { MSDOSVID_O_BEAM, "beam", "Beam", "The line the card is drawing", 1 },
    { MSDOSVID_O_HEAT, "heat", "Heat", "VRAM written lately, fading out", 1 },
    { MSDOSVID_O_CHANGED, "changed", "Changed", "VRAM that changed since the last refresh", 0 },
    { MSDOSVID_O_ACTIVE, "active", "Active area", "Outline the active area inside the border", 0 },
    { MSDOSVID_O_SPLIT, "split", "Split", "Where the split screen (line compare) begins", 1 },
    { MSDOSVID_O_BANKS, "banks", "Banks", "Tint the interleaved banks (CGA, PCjr)", 0 },
    { MSDOSVID_O_DIGITS, "digits", "Values", "The value on each pixel or cell once zoomed in far enough", 0 },
};

const msdosvid_overlay_def *
msdosvid_overlays(int *count)
{
    if (count)
        *count = MSDOSVID_O_COUNT;
    return g_overlays;
}

unsigned
msdosvid_overlay_defaults(void)
{
    unsigned m = 0;
    for (int i = 0; i < MSDOSVID_O_COUNT; i++)
        if (g_overlays[i].on_by_default)
            m |= g_overlays[i].bit;
    return m;
}

unsigned
msdosvid_overlays_from_keys(const char *csv)
{
    unsigned m = 0;
    if (!csv)
        return msdosvid_overlay_defaults();
    if (!strcmp(csv, "none"))
        return 0;
    if (!strcmp(csv, "all"))
        return (1u << MSDOSVID_O_COUNT) - 1;
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s", csv);
    for (char *tok = strtok(tmp, ", "); tok; tok = strtok(NULL, ", "))
        for (int i = 0; i < MSDOSVID_O_COUNT; i++)
            if (!strcmp(tok, g_overlays[i].key))
                m |= g_overlays[i].bit;
    return m;
}

static const struct {
    const char *name, *key;
} g_views[MSDOSVID_V_COUNT] = {
    { "Decoded (from VRAM)", "decoded" },
    { "Display (last frame)", "display" },
    { "Beam (live frame)", "beam" },
    { "Values", "values" },
    { "Bit planes", "planes" },
    { "Character set", "font" },
    { "Memory map", "memory" },
};

const char *
msdosvid_view_name(int v)
{
    return v >= 0 && v < MSDOSVID_V_COUNT ? g_views[v].name : "?";
}

const char *
msdosvid_view_key(int v)
{
    return v >= 0 && v < MSDOSVID_V_COUNT ? g_views[v].key : "?";
}

int
msdosvid_view_from_key(const char *key)
{
    if (!key || !*key)
        return -1;
    for (int i = 0; i < MSDOSVID_V_COUNT; i++)
        if (!strcmp(key, g_views[i].key))
            return i;
    return -1;
}

static const char *const g_sections[MSDOSVID_S_COUNT] = { "pixel", "registers", "palette",
                                                          "memory", "timing" };

int
msdosvid_section_from_key(const char *key)
{
    if (!key)
        return -1;
    for (int i = 0; i < MSDOSVID_S_COUNT; i++)
        if (!strcmp(key, g_sections[i]))
            return i;
    return -1;
}

const char *
msdosvid_section_name(int s)
{
    static const char *const n[MSDOSVID_S_COUNT] = { "Pixel", "Registers", "Palette", "Memory",
                                                     "Timing" };
    return s >= 0 && s < MSDOSVID_S_COUNT ? n[s] : "?";
}

/* ------------------------------------------------------------------------------------- */
/* the mode                                                                               */
/* ------------------------------------------------------------------------------------- */

static int
is_6845(const msdos_vid_snap *s)
{
    return s->card == MSDOS_VID_MDA || s->card == MSDOS_VID_CGA || s->card == MSDOS_VID_PCJR;
}

static int
is_ega_family(const msdos_vid_snap *s)
{
    return s->card == MSDOS_VID_EGA || s->card == MSDOS_VID_VGA || s->card == MSDOS_VID_TVGA;
}

static const char *
card_label(const msdos_vid_snap *s)
{
    switch (s->card) {
        case MSDOS_VID_MDA:  return "MDA";
        case MSDOS_VID_CGA:  return "CGA";
        case MSDOS_VID_PCJR: return "PCjr";
        case MSDOS_VID_EGA:  return "EGA";
        case MSDOS_VID_VGA:  return "VGA";
        case MSDOS_VID_TVGA: return "TVGA8900";
        default:             return "no video";
    }
}

int
vid_pcjr_select(const msdos_vid_snap *s)
{
    return (s->gate[0] & 0x13) | ((s->gate[3] & 0x08) << 5);
}

int
vid_pcjr_banks(const msdos_vid_snap *s)
{
    switch (s->memctrl >> 6) {
        case 1:  return 2;
        case 3:  return 4;
        default: return 1;
    }
}

int
vid_ega_text_cell_w(const msdos_vid_snap *s)
{
    return (s->seq[1] & 1) ? 8 : 9;
}

/* logical rows of a graphics mode */
static int
ega_gfx_height(const msdos_vid_snap *s)
{
    int per = ((s->crtc[9] & 0x1F) + 1) * ((s->crtc[9] & 0x80) ? 2 : 1);
    return s->dispend > 0 ? s->dispend / per : 0;
}

static void
mode_name(msdosvid_info *o, const char *what)
{
    snprintf(o->mode_name, sizeof o->mode_name, "%s", what);
}

int
msdosvid_info_get(const msdos_vid_snap *s, msdosvid_info *o)
{
    memset(o, 0, sizeof *o);
    o->cursor_x = o->cursor_y = o->split_row = o->bios_mode = -1;
    o->bank_count = 1;
    o->line_double = 1;
    if (s->card == MSDOS_VID_OFF) {
        mode_name(o, "the PC is off");
        snprintf(o->line1, sizeof o->line1, "No video card");
        return -1;
    }

    char b[64];
    uint32_t cursor_ma = s->crtc[0x0E] << 8 | s->crtc[0x0F];
    int      cstart = s->crtc[0x0A] & 0x1F, cend = s->crtc[0x0B] & 0x1F;
    o->cursor_top = cstart;
    o->cursor_bottom = cend;

    if (s->card == MSDOS_VID_MDA) {
        o->mode = MSDOS_VM_TEXT;
        o->cols = s->crtc[1] ? s->crtc[1] : 80;
        o->rows = s->crtc[6] ? s->crtc[6] : 25;
        o->cell_w = 9;
        o->cell_h = (s->crtc[9] & 0x1F) + 1;
        o->colors = 2;
        o->blink = (s->mode_reg & 0x20) != 0;
        o->width = o->cols * o->cell_w;
        o->height = o->rows * o->cell_h;
        o->bios_mode = 7;
        snprintf(b, sizeof b, "%dx%d text", o->cols, o->rows);
        mode_name(o, b);
    } else if (s->card == MSDOS_VID_CGA) {
        int gfx = (s->mode_reg & 2) != 0;
        if (gfx && (s->mode_reg & 0x10)) {
            o->mode = MSDOS_VM_GFX_1;
            o->width = s->crtc[1] * 16;
            o->height = s->crtc[6] * ((s->crtc[9] & 0x1F) + 1);
            o->colors = 2;
            o->bank_count = 2;
            o->bios_mode = 6;
            snprintf(b, sizeof b, "%dx%d, 2 colours", o->width, o->height);
            mode_name(o, b);
        } else if (gfx) {
            o->mode = MSDOS_VM_GFX_2;
            o->width = s->crtc[1] * 8;
            o->height = s->crtc[6] * ((s->crtc[9] & 0x1F) + 1);
            o->colors = 4;
            o->bank_count = 2;
            o->bios_mode = 4;
            snprintf(b, sizeof b, "%dx%d, 4 colours", o->width, o->height);
            mode_name(o, b);
        } else {
            o->mode = MSDOS_VM_TEXT;
            o->cols = s->crtc[1] ? s->crtc[1] : ((s->mode_reg & 1) ? 80 : 40);
            o->rows = s->crtc[6] ? s->crtc[6] : 25;
            o->cell_w = 8;
            o->cell_h = (s->crtc[9] & 0x1F) + 1;
            o->colors = 16;
            o->blink = (s->mode_reg & 0x20) != 0;
            o->width = o->cols * 8;
            o->height = o->rows * o->cell_h;
            o->bios_mode = o->cols == 80 ? 3 : 1;
            snprintf(b, sizeof b, "%dx%d text", o->cols, o->rows);
            mode_name(o, b);
        }
    } else if (s->card == MSDOS_VID_PCJR) {
        int sel = vid_pcjr_select(s);
        int rows = s->crtc[6] * ((s->crtc[9] & 0x1F) + 1);
        switch (sel) {
            case 0x13:
            case 0x12:
                o->mode = MSDOS_VM_GFX_4;
                o->width = s->crtc[1] * 4;
                o->height = rows;
                o->colors = 16;
                o->bank_count = vid_pcjr_banks(s);
                o->bios_mode = sel == 0x13 ? 9 : 8;
                snprintf(b, sizeof b, "%dx%d, 16 colours", o->width, o->height);
                mode_name(o, b);
                break;
            case 0x03:
            case 0x02:
                o->mode = MSDOS_VM_GFX_2;
                o->width = s->crtc[1] * 8;
                o->height = rows;
                o->colors = 4;
                o->bank_count = vid_pcjr_banks(s);
                o->bios_mode = sel == 0x03 ? 0x0A : 5;
                snprintf(b, sizeof b, "%dx%d, 4 colours", o->width, o->height);
                mode_name(o, b);
                break;
            case 0x102:
                o->mode = MSDOS_VM_GFX_1;
                o->width = s->crtc[1] * 16;
                o->height = rows;
                o->colors = 2;
                o->bank_count = vid_pcjr_banks(s);
                o->bios_mode = 6;
                snprintf(b, sizeof b, "%dx%d, 2 colours", o->width, o->height);
                mode_name(o, b);
                break;
            case 0x01:
            case 0x00:
                o->mode = MSDOS_VM_TEXT;
                o->cols = s->crtc[1] ? s->crtc[1] : (sel ? 80 : 40);
                o->rows = s->crtc[6] ? s->crtc[6] : 25;
                o->cell_w = 8;
                o->cell_h = (s->crtc[9] & 0x1F) + 1;
                o->colors = 16;
                o->blink = (s->gate[3] & 4) != 0;
                o->width = o->cols * 8;
                o->height = o->rows * o->cell_h;
                o->bios_mode = o->cols == 80 ? 3 : 1;
                snprintf(b, sizeof b, "%dx%d text", o->cols, o->rows);
                mode_name(o, b);
                break;
            default:
                o->mode = MSDOS_VM_UNKNOWN;
                mode_name(o, "an undecoded PCjr mode");
                break;
        }
    } else if (is_ega_family(s)) {
        int gfx = (s->attr[0x10] & 1) != 0;
        if (!gfx) {
            o->mode = MSDOS_VM_TEXT;
            o->cell_w = vid_ega_text_cell_w(s);
            o->cell_h = (s->crtc[9] & 0x1F) + 1;
            o->cols = s->crtc[1] + 1;
            o->rows = s->dispend > 0 ? s->dispend / o->cell_h : 25;
            o->colors = 16;
            o->blink = (s->attr[0x10] & 8) != 0;
            o->width = o->cols * o->cell_w * ((s->seq[1] & 8) ? 2 : 1);
            o->height = o->rows * o->cell_h;
            o->bios_mode = o->cols == 80 ? 3 : 1;
            snprintf(b, sizeof b, "%dx%d text, %dx%d cells", o->cols, o->rows, o->cell_w,
                     o->cell_h);
            mode_name(o, b);
        } else if (s->card != MSDOS_VID_EGA && ((s->gdc[5] & 0x40) || (s->seq[4] & 8))) {
            o->mode = MSDOS_VM_PACKED_8;
            o->width = (s->crtc[1] + 1) * 4;
            o->height = ega_gfx_height(s);
            o->colors = 256;
            o->line_double = (s->crtc[9] & 0x1F) + 1;
            o->bios_mode = (o->width == 320 && o->height == 200) ? 0x13 : -1;
            snprintf(b, sizeof b, "%dx%d, 256 colours%s", o->width, o->height,
                     (s->seq[4] & 8) ? " (chain-4)" : " (unchained)");
            mode_name(o, b);
        } else if (s->gdc[5] & 0x20) {
            /* BIOS modes 4/5 on an EGA/VGA: the CGA's interleave in odd/even planes */
            o->mode = MSDOS_VM_GFX_2;
            o->width = (s->crtc[1] + 1) * 8;
            o->height = ega_gfx_height(s);
            o->colors = 4;
            o->bank_count = 2;
            o->bios_mode = 4;
            snprintf(b, sizeof b, "%dx%d, 4 colours (CGA compatible)", o->width, o->height);
            mode_name(o, b);
        } else if (s->bpp > 8 && s->card == MSDOS_VID_TVGA) {
            o->mode = MSDOS_VM_UNKNOWN;
            mode_name(o, "a true-colour mode");
        } else {
            o->mode = MSDOS_VM_PLANAR_16;
            o->width = (s->crtc[1] + 1) * 8;
            o->height = ega_gfx_height(s);
            o->colors = 16;
            snprintf(b, sizeof b, "%dx%d, 16 colours", o->width, o->height);
            mode_name(o, b);
            if (o->width == 320 && o->height == 200)
                o->bios_mode = 0x0D;
            else if (o->width == 640 && o->height == 200)
                o->bios_mode = 0x0E;
            else if (o->width == 640 && o->height == 350)
                o->bios_mode = 0x10;
            else if (o->width == 640 && o->height == 480)
                o->bios_mode = 0x12;
        }
        if (s->split > 0 && s->split < s->dispend) {
            int per = ((s->crtc[9] & 0x1F) + 1) * ((s->crtc[9] & 0x80) ? 2 : 1);
            o->split_row = s->split / (o->mode == MSDOS_VM_TEXT ? o->cell_h : per);
        }
        cursor_ma = s->cursoraddr ? s->cursoraddr : cursor_ma;
        if (o->mode == MSDOS_VM_TEXT)
            cursor_ma = s->crtc[0x0E] << 8 | s->crtc[0x0F];
    }

    if (o->mode == MSDOS_VM_TEXT && o->cols > 0) {
        uint32_t start = is_6845(s) ? (uint32_t) (s->crtc[12] << 8 | s->crtc[13]) :
                                      (uint32_t) (s->crtc[12] << 8 | s->crtc[13]);
        int rel = (int) cursor_ma - (int) start;
        if (rel >= 0 && rel < o->cols * o->rows) {
            o->cursor_x = rel % o->cols;
            o->cursor_y = rel / o->cols;
        }
        o->cursor_visible = !(s->crtc[0x0A] & 0x20) && cstart <= cend;
    }

    /* clocks, from the registers */
    double dot = 0;
    int    cw = 8;
    int    chars = 0;
    int    lines = 0;
    if (s->card == MSDOS_VID_MDA) {
        dot = 16.257;
        cw = 9;
        chars = s->crtc[0] + 1;
        lines = (s->crtc[4] + 1) * ((s->crtc[9] & 0x1F) + 1) + s->crtc[5];
    } else if (s->card == MSDOS_VID_CGA || s->card == MSDOS_VID_PCJR) {
        int hi = s->card == MSDOS_VID_CGA ? (s->mode_reg & 1) : (s->gate[0] & 1);
        dot = 14.318;
        cw = hi ? 8 : 16;
        chars = s->crtc[0] + 1;
        lines = (s->crtc[4] + 1) * ((s->crtc[9] & 0x1F) + 1) + s->crtc[5];
    } else if (is_ega_family(s)) {
        int sel = (s->miscout >> 2) & 3;
        if (s->card == MSDOS_VID_EGA)
            dot = sel == 0 ? 14.318 : 16.257;
        else
            dot = sel == 0 ? 25.175 : 28.322;
        cw = (s->seq[1] & 1) ? 8 : 9;
        if (s->seq[1] & 8)
            cw *= 2;
        chars = s->crtc[0] + (s->card == MSDOS_VID_EGA ? 2 : 5);
        int vt = s->crtc[6] | ((s->crtc[7] & 1) << 8) | ((s->crtc[7] & 0x20) << 4);
        lines = vt + 2;
    }
    if (dot > 0 && chars > 0 && cw > 0) {
        o->dot_mhz = dot;
        o->line_khz = dot * 1000.0 / cw / chars;
        if (lines > 0)
            o->frame_hz = o->line_khz * 1000.0 / lines;
    }

    snprintf(o->line1, sizeof o->line1, "%s  %s  start %04X  offset %d", card_label(s),
             o->mode_name, s->crtc[12] << 8 | s->crtc[13], s->rowoffset ? s->rowoffset * 2 :
                                                                       s->crtc[1] * (is_6845(s) ? 2 : 1));
    snprintf(o->line2, sizeof o->line2, "line %d  %s%s  %.1f kHz  %.0f Hz", s->displine,
             s->dispon ? "active" : "blanking", s->linepos ? " (line end)" : "", o->line_khz,
             o->frame_hz);
    return 0;
}

/* ------------------------------------------------------------------------------------- */
/* registers                                                                              */
/* ------------------------------------------------------------------------------------- */

static const char *const g_crtc6845[18] = {
    "Horizontal Total", "Horizontal Displayed", "H Sync Position", "Sync Widths",
    "Vertical Total", "V Total Adjust", "Vertical Displayed", "V Sync Position",
    "Interlace Mode", "Max Scan Line", "Cursor Start", "Cursor End", "Start Address High",
    "Start Address Low", "Cursor High", "Cursor Low", "Light Pen High", "Light Pen Low"
};

static const char *const g_crtcvga[25] = {
    "Horizontal Total", "H Display End", "Start H Blanking", "End H Blanking",
    "Start H Retrace", "End H Retrace", "Vertical Total", "Overflow", "Preset Row Scan",
    "Max Scan Line", "Cursor Start", "Cursor End", "Start Address High", "Start Address Low",
    "Cursor Loc High", "Cursor Loc Low", "V Retrace Start", "V Retrace End", "V Display End",
    "Offset", "Underline Location", "Start V Blanking", "End V Blanking", "Mode Control",
    "Line Compare"
};

static const char *const g_seq[5] = { "Reset", "Clocking Mode", "Map Mask", "Char Map Select",
                                      "Memory Mode" };
static const char *const g_gdc[9] = { "Set/Reset", "Enable Set/Reset", "Colour Compare",
                                      "Data Rotate", "Read Map Select", "Mode", "Miscellaneous",
                                      "Colour Don't Care", "Bit Mask" };
static const char *const g_attr[5] = { "Mode Control", "Overscan Colour", "Colour Plane Enable",
                                       "H Pixel Panning", "Colour Select" };

static void
add_reg(msdosvid_reg *out, int *n, int max, const char *group, int index, int port,
        const char *name, unsigned value, int digits, int age, const char *meaning)
{
    if (*n >= max)
        return;
    msdosvid_reg *r = &out[(*n)++];
    memset(r, 0, sizeof *r);
    r->group = group;
    r->index = index;
    r->port = port;
    snprintf(r->name, sizeof r->name, "%s", name);
    r->value = value;
    r->digits = digits;
    r->age = age;
    if (meaning)
        snprintf(r->meaning, sizeof r->meaning, "%s", meaning);
}

#define AGE(t, g, i) ((t) ? (t)->reg_age[g][i] : 0)

static void
meaning_cat(char *dst, size_t n, const char *piece)
{
    size_t l = strlen(dst);
    if (l && l + 2 < n) {
        dst[l++] = ',';
        dst[l++] = ' ';
        dst[l] = 0;
    }
    snprintf(dst + l, n - l, "%s", piece);
}

static void
crtc_meaning(const msdos_vid_snap *s, int i, char *m, size_t n)
{
    m[0] = 0;
    char b[64];
    int  ega = is_ega_family(s);
    switch (i) {
        case 0x0A:
            snprintf(b, sizeof b, "starts line %d", s->crtc[i] & 0x1F);
            meaning_cat(m, n, b);
            meaning_cat(m, n, (s->crtc[i] & 0x20) ? "cursor off" :
                              ((s->crtc[i] & 0x40) ? "blinks" : "steady"));
            break;
        case 0x0B:
            snprintf(b, sizeof b, "ends line %d", s->crtc[i] & 0x1F);
            meaning_cat(m, n, b);
            break;
        case 0x0C:
            snprintf(b, sizeof b, "start %04X", s->crtc[12] << 8 | s->crtc[13]);
            meaning_cat(m, n, b);
            break;
        case 0x0E:
            snprintf(b, sizeof b, "cursor %04X", s->crtc[14] << 8 | s->crtc[15]);
            meaning_cat(m, n, b);
            break;
        case 0x09:
            snprintf(b, sizeof b, "%d lines per row", (s->crtc[i] & 0x1F) + 1);
            meaning_cat(m, n, b);
            if (ega && (s->crtc[i] & 0x80))
                meaning_cat(m, n, "double scan");
            break;
        case 0x01:
            snprintf(b, sizeof b, "%d columns", s->crtc[i] + (ega ? 1 : 0));
            meaning_cat(m, n, b);
            break;
        case 0x06:
            if (!ega) {
                snprintf(b, sizeof b, "%d rows", s->crtc[i]);
                meaning_cat(m, n, b);
            }
            break;
        case 0x13:
            if (ega) {
                snprintf(b, sizeof b, "%d units per row", s->crtc[i] * 2);
                meaning_cat(m, n, b);
            }
            break;
        case 0x14:
            if (ega) {
                meaning_cat(m, n, (s->crtc[i] & 0x40) ? "dword mode" : "no dword");
            }
            break;
        case 0x17:
            if (ega) {
                meaning_cat(m, n, (s->crtc[i] & 0x40) ? "byte mode" : "word mode");
                if (!(s->crtc[i] & 0x80))
                    meaning_cat(m, n, "SYNC OFF");
                if (!(s->crtc[i] & 1))
                    meaning_cat(m, n, "CGA bank bit");
            }
            break;
        case 0x18:
            if (ega) {
                snprintf(b, sizeof b, "line compare %d", s->split);
                meaning_cat(m, n, b);
            }
            break;
        default:
            break;
    }
}

static void
ext_meaning(const msdos_vid_snap *s, int group, int i, char *m, size_t n)
{
    m[0] = 0;
    char b[64];
    if (group == 1) { /* sequencer */
        if (i == 1) {
            meaning_cat(m, n, (s->seq[1] & 1) ? "8 dots" : "9 dots");
            if (s->seq[1] & 8)
                meaning_cat(m, n, "dot clock /2");
            if (s->seq[1] & 0x20)
                meaning_cat(m, n, "SCREEN OFF");
        } else if (i == 2) {
            snprintf(b, sizeof b, "planes %s%s%s%s", (s->seq[2] & 1) ? "0" : "-",
                     (s->seq[2] & 2) ? "1" : "-", (s->seq[2] & 4) ? "2" : "-",
                     (s->seq[2] & 8) ? "3" : "-");
            meaning_cat(m, n, b);
        } else if (i == 3) {
            snprintf(b, sizeof b, "font A at %05X, B at %05X", s->charseta, s->charsetb);
            meaning_cat(m, n, b);
        } else if (i == 4) {
            meaning_cat(m, n, (s->seq[4] & 8) ? "chain-4" : "planar");
            meaning_cat(m, n, (s->seq[4] & 4) ? "sequential" : "odd/even");
            if (s->seq[4] & 2)
                meaning_cat(m, n, "extended memory");
        }
    } else if (group == 2) { /* graphics controller */
        if (i == 5) {
            snprintf(b, sizeof b, "write mode %d, read mode %d", s->gdc[5] & 3,
                     (s->gdc[5] >> 3) & 1);
            meaning_cat(m, n, b);
            if (s->gdc[5] & 0x10)
                meaning_cat(m, n, "odd/even");
            if (s->gdc[5] & 0x20)
                meaning_cat(m, n, "2-bit shift (CGA)");
            if (s->gdc[5] & 0x40)
                meaning_cat(m, n, "256 colour");
        } else if (i == 6) {
            static const char *const map[4] = { "A0000-BFFFF", "A0000-AFFFF", "B0000-B7FFF",
                                                "B8000-BFFFF" };
            meaning_cat(m, n, (s->gdc[6] & 1) ? "graphics" : "text");
            meaning_cat(m, n, map[(s->gdc[6] >> 2) & 3]);
            if (s->gdc[6] & 2)
                meaning_cat(m, n, "chain odd/even");
        }
    } else if (group == 3) { /* attribute */
        if (i == 0x10) {
            meaning_cat(m, n, (s->attr[i] & 1) ? "graphics" : "text");
            if (s->attr[i] & 2)
                meaning_cat(m, n, "mono attrs");
            if (s->attr[i] & 4)
                meaning_cat(m, n, "line graphics");
            meaning_cat(m, n, (s->attr[i] & 8) ? "blink" : "bright bg");
            if (s->attr[i] & 0x20)
                meaning_cat(m, n, "pan stops at split");
            if (s->attr[i] & 0x40)
                meaning_cat(m, n, "8-bit pixels");
            if (s->attr[i] & 0x80)
                meaning_cat(m, n, "P5/P4 from colour select");
        } else if (i < 16) {
            snprintf(b, sizeof b, "-> colour %02X", s->egapal[i]);
            meaning_cat(m, n, b);
        }
    }
}

int
msdosvid_regs(const msdos_vid_snap *s, const msdosvid_tracker *t, msdosvid_reg *out, int max)
{
    int  n = 0;
    char m[120];
    if (s->card == MSDOS_VID_OFF)
        return 0;

    if (s->card == MSDOS_VID_MDA) {
        snprintf(m, sizeof m, "%s%s%s", (s->mode_reg & 1) ? "80x25 text" : "?",
                 (s->mode_reg & 8) ? ", display on" : ", blanked",
                 (s->mode_reg & 0x20) ? ", blink" : "");
        add_reg(out, &n, max, "Mode", -1, 0x3B8, "Mode Control", s->mode_reg, 2, AGE(t, 2, 33), m);
        add_reg(out, &n, max, "Mode", -1, 0x3BA, "Status", s->status, 2, 0,
                (s->status & 8) ? "video on" : "video off");
    } else if (s->card == MSDOS_VID_CGA) {
        snprintf(m, sizeof m, "%s%s%s%s%s", (s->mode_reg & 1) ? "80 col" : "40 col",
                 (s->mode_reg & 2) ? ", graphics" : ", text",
                 (s->mode_reg & 4) ? ", B/W" : ", colour",
                 (s->mode_reg & 0x10) ? ", 640x200" : "",
                 (s->mode_reg & 0x20) ? ", blink" : "");
        add_reg(out, &n, max, "Mode", -1, 0x3D8, "Mode Control", s->mode_reg, 2, AGE(t, 2, 33), m);
        snprintf(m, sizeof m, "background/border %X%s, palette %d", s->color_reg & 15,
                 (s->color_reg & 0x10) ? ", intense" : "", (s->color_reg >> 5) & 1);
        add_reg(out, &n, max, "Mode", -1, 0x3D9, "Colour Select", s->color_reg, 2, AGE(t, 2, 34), m);
        add_reg(out, &n, max, "Mode", -1, 0x3DA, "Status", s->status, 2, 0,
                (s->status & 8) ? "vertical retrace" : ((s->status & 1) ? "display off" : ""));
    } else if (s->card == MSDOS_VID_PCJR) {
        snprintf(m, sizeof m, "page register: CRT page %d, CPU page %d, address mode %d",
                 s->memctrl & 7, (s->memctrl >> 3) & 7, s->memctrl >> 6);
        add_reg(out, &n, max, "Mode", -1, 0x3DF, "CRT/CPU Page", s->memctrl, 2, AGE(t, 2, 35), m);
        static const char *const gn[4] = { "Mode Control 1", "Palette Mask", "Border Colour",
                                           "Mode Control 2" };
        for (int i = 0; i < 4; i++) {
            m[0] = 0;
            if (i == 0) {
                snprintf(m, sizeof m, "%s%s%s%s%s", (s->gate[0] & 1) ? "high bandwidth" : "low bandwidth",
                         (s->gate[0] & 2) ? ", graphics" : ", text",
                         (s->gate[0] & 4) ? ", B/W" : "", (s->gate[0] & 8) ? "" : ", video off",
                         (s->gate[0] & 0x10) ? ", 16 colours" : "");
            } else if (i == 3) {
                snprintf(m, sizeof m, "%s%s", (s->gate[3] & 4) ? "blink" : "bright bg",
                         (s->gate[3] & 8) ? ", 640x200" : "");
            }
            add_reg(out, &n, max, "Gate array", i, 0x3DA, gn[i], s->gate[i], 2, AGE(t, 2, 40 + i), m);
        }
        for (int i = 0; i < 16; i++) {
            char nm[40];
            snprintf(nm, sizeof nm, "Palette %X", i);
            add_reg(out, &n, max, "Gate array", 16 + i, 0x3DA, nm, s->gate[16 + i], 2,
                    AGE(t, 2, 56 + i), NULL);
        }
    }

    if (is_6845(s) || is_ega_family(s)) {
        int total = is_ega_family(s) ? 25 : 18;
        for (int i = 0; i < total; i++) {
            const char *nm = is_ega_family(s) ? g_crtcvga[i] : g_crtc6845[i];
            crtc_meaning(s, i, m, sizeof m);
            add_reg(out, &n, max, "CRTC", i, 0, nm, s->crtc[i], 2, AGE(t, 0, i), m);
        }
        if (s->card == MSDOS_VID_TVGA)
            for (int i = 25; i < s->crtc_count && i < 256; i++)
                if (s->crtc[i]) {
                    char nm[40];
                    snprintf(nm, sizeof nm, "Extended %02X", i);
                    add_reg(out, &n, max, "CRTC", i, 0, nm, s->crtc[i], 2, AGE(t, 0, i), NULL);
                }
    }
    if (is_ega_family(s)) {
        snprintf(m, sizeof m, "%s, %s, %s sync, %s %s", (s->miscout & 1) ? "colour (3Dx)" : "mono (3Bx)",
                 (s->miscout & 2) ? "RAM enabled" : "RAM off",
                 (s->miscout & 0x40) ? "H-" : "H+", (s->miscout & 0x80) ? "V-" : "V+",
                 s->card == MSDOS_VID_EGA ? ((s->miscout & 0x0C) ? "16.2 MHz" : "14.3 MHz") :
                                            (((s->miscout >> 2) & 3) ? "28.3 MHz" : "25.2 MHz"));
        add_reg(out, &n, max, "Misc", -1, 0x3C2, "Miscellaneous Output", s->miscout, 2,
                AGE(t, 2, 32), m);
        int sc = s->seq_count ? s->seq_count : 5;
        for (int i = 0; i < sc && i < 64; i++) {
            if (i >= 5 && !s->seq[i])
                continue;
            char nm[40];
            if (i < 5)
                snprintf(nm, sizeof nm, "%s", g_seq[i]);
            else
                snprintf(nm, sizeof nm, "Extended %02X", i);
            ext_meaning(s, 1, i, m, sizeof m);
            add_reg(out, &n, max, "Sequencer", i, 0, nm, s->seq[i], 2, AGE(t, 1, i), m);
        }
        for (int i = 0; i < 9; i++) {
            ext_meaning(s, 2, i, m, sizeof m);
            add_reg(out, &n, max, "Graphics", i, 0, g_gdc[i], s->gdc[i], 2, AGE(t, 1, 64 + i), m);
        }
        for (int i = 0; i < 16; i++) {
            char nm[40];
            snprintf(nm, sizeof nm, "Palette %X", i);
            ext_meaning(s, 3, i, m, sizeof m);
            add_reg(out, &n, max, "Attribute", i, 0, nm, s->attr[i], 2, AGE(t, 2, i), m);
        }
        for (int i = 0x10; i <= 0x14; i++) {
            ext_meaning(s, 3, i, m, sizeof m);
            add_reg(out, &n, max, "Attribute", i, 0, g_attr[i - 0x10], s->attr[i], 2, AGE(t, 2, i), m);
        }
        if (s->dac_valid)
            add_reg(out, &n, max, "Misc", -1, 0x3C6, "DAC Pel Mask", s->dac_mask, 2,
                    AGE(t, 2, 36), NULL);
    }
    return n;
}

/* ------------------------------------------------------------------------------------- */
/* palette                                                                                */
/* ------------------------------------------------------------------------------------- */

#define RGB(r, g, b) ((uint32_t) (((r) << 16) | ((g) << 8) | (b)))

/* the IBM RGBI colours */
uint32_t
vid_rgbi(int i)
{
    static const uint32_t t[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    return t[i & 15];
}

/* the colours a CGA 4-colour mode draws with: index 0..3 */
void
vid_cga_gfx_palette(const msdos_vid_snap *s, uint32_t pal[4])
{
    int bg = s->color_reg & 15;
    int c1, c2, c3;
    if (s->mode_reg & 4) {
        c1 = 3;
        c2 = 4;
        c3 = 7;
    } else if (s->color_reg & 0x20) {
        c1 = 3;
        c2 = 5;
        c3 = 7;
    } else {
        c1 = 2;
        c2 = 4;
        c3 = 6;
    }
    if (s->color_reg & 0x10) {
        c1 |= 8;
        c2 |= 8;
        c3 |= 8;
    }
    pal[0] = vid_rgbi(bg);
    pal[1] = vid_rgbi(c1);
    pal[2] = vid_rgbi(c2);
    pal[3] = vid_rgbi(c3);
}

static void
set_entry(msdosvid_pal_entry *e, int index, uint32_t rgb, const char *detail, int active,
          const msdosvid_tracker *t)
{
    memset(e, 0, sizeof *e);
    e->index = index;
    e->rgb = rgb & 0xFFFFFF;
    snprintf(e->detail, sizeof e->detail, "%s", detail);
    e->used = -1;
    e->active = active;
    e->age = t ? t->pal_age[index & 255] : 0;
}

int
msdosvid_palette(const msdos_vid_snap *s, const msdosvid_tracker *t, msdosvid_pal_entry *out, int max)
{
    int n = 0;
    char d[48];
    msdosvid_info inf;
    msdosvid_info_get(s, &inf);
    int colors = inf.colors;
    switch (s->card) {
        case MSDOS_VID_MDA:
            break;
        case MSDOS_VID_CGA: {
            uint32_t g[4];
            vid_cga_gfx_palette(s, g);
            for (int i = 0; i < 16 && n < max; i++) {
                int act = 1;
                if (inf.mode == MSDOS_VM_GFX_2)
                    act = i == (s->color_reg & 15) || (g[1] == vid_rgbi(i)) || (g[2] == vid_rgbi(i)) ||
                          (g[3] == vid_rgbi(i));
                else if (inf.mode == MSDOS_VM_GFX_1)
                    act = i == (s->color_reg & 15) || i == 0;
                snprintf(d, sizeof d, "RGBI %X", i);
                set_entry(&out[n++], i, vid_rgbi(i), d, act, t);
            }
            break;
        }
        case MSDOS_VID_PCJR:
            for (int i = 0; i < 16 && n < max; i++) {
                int ix = s->gate[16 + i] & 15;
                snprintf(d, sizeof d, "register %X -> RGBI %X", i, ix);
                set_entry(&out[n++], i, vid_rgbi(ix), d, colors == 0 || i < colors, t);
            }
            break;
        case MSDOS_VID_EGA:
            for (int i = 0; i < 16 && n < max; i++) {
                snprintf(d, sizeof d, "attr %X -> %02X", i, s->egapal[i]);
                set_entry(&out[n++], i, s->pal16[i], d, 1, t);
            }
            break;
        case MSDOS_VID_VGA:
        case MSDOS_VID_TVGA:
            if (inf.mode == MSDOS_VM_PACKED_8 || (inf.mode == MSDOS_VM_UNKNOWN && s->bpp == 8)) {
                for (int i = 0; i < 256 && n < max; i++) {
                    snprintf(d, sizeof d, "DAC %02X: %02X,%02X,%02X", i, s->dac[i][0], s->dac[i][1],
                             s->dac[i][2]);
                    set_entry(&out[n++], i, s->pal256[i], d, (i & s->dac_mask) == i, t);
                }
            } else {
                for (int i = 0; i < 16 && n < max; i++) {
                    int di = s->egapal[i];
                    snprintf(d, sizeof d, "attr %X -> DAC %02X: %02X,%02X,%02X", i, di, s->dac[di][0],
                             s->dac[di][1], s->dac[di][2]);
                    set_entry(&out[n++], i, s->pal16[i], d, 1, t);
                }
            }
            break;
        default:
            break;
    }
    return n;
}

/* ------------------------------------------------------------------------------------- */
/* memory regions                                                                         */
/* ------------------------------------------------------------------------------------- */

static void
add_region(msdosvid_region *out, int *n, int max, const char *name, uint32_t off, uint32_t size,
           uint32_t cpu, uint32_t color)
{
    if (*n >= max)
        return;
    msdosvid_region *r = &out[(*n)++];
    snprintf(r->name, sizeof r->name, "%s", name);
    r->vram_off = off;
    r->size = size;
    r->cpu_addr = cpu;
    r->color = color;
}

int
msdosvid_regions(const msdos_vid_snap *s, msdosvid_region *out, int max)
{
    int n = 0;
    msdosvid_info inf;
    if (s->card == MSDOS_VID_OFF || !s->vram_size)
        return 0;
    msdosvid_info_get(s, &inf);
    char nm[56];
    add_region(out, &n, max, "Video RAM", 0, s->vram_size, s->cpu_base, 0x303038);
    uint32_t shown = 0;
    if (inf.mode == MSDOS_VM_TEXT)
        shown = (uint32_t) (inf.cols * inf.rows) * (s->planar ? s->stride : 2u);
    else if (inf.height > 0 && s->pitch_units)
        shown = (uint32_t) inf.height * s->pitch_units * s->stride / (uint32_t) (inf.bank_count ? 1 : 1);
    if (is_6845(s)) {
        uint32_t startb = (uint32_t) ((s->crtc[12] << 8 | s->crtc[13]) * 2u) & (s->vram_size - 1);
        if (inf.bank_count > 1) {
            for (int b = 0; b < inf.bank_count; b++) {
                snprintf(nm, sizeof nm, "Bank %d (rows %d, %d, ...)", b, b, b + inf.bank_count);
                add_region(out, &n, max, nm, (uint32_t) b * 0x2000u, 0x2000, s->cpu_base + (uint32_t) b * 0x2000u,
                           b & 1 ? 0x2E6A9A : 0x3A8CC8);
            }
        } else {
            add_region(out, &n, max, "Displayed page", startb, shown ? shown : 2000, s->cpu_base + startb, 0x3A8CC8);
        }
    } else {
        uint32_t startb = s->start_byte;
        add_region(out, &n, max, "Displayed picture", startb, shown ? shown : s->vram_size / 4,
                   s->cpu_base, 0x3A8CC8);
        if (inf.split_row > 0)
            add_region(out, &n, max, "Split screen (from 0)", 0, (uint32_t) (inf.height > 0 ? inf.height - inf.split_row : 0) * s->pitch_units * s->stride, s->cpu_base, 0xC87A3A);
    }
    return n;
}

/* ------------------------------------------------------------------------------------- */
/* console text                                                                           */
/* ------------------------------------------------------------------------------------- */

int
msdosvid_text(const msdos_vid_snap *s, const char *kind, char *dst, int dstsz)
{
    int  n = 0;
    char *p = dst;
#define PUT(...)                                                                     \
    do {                                                                              \
        if (n < dstsz - 1) {                                                         \
            int w_ = snprintf(p + n, (size_t) (dstsz - n), __VA_ARGS__);              \
            n += w_ > 0 ? (w_ < dstsz - n ? w_ : dstsz - n - 1) : 0;                 \
        }                                                                             \
    } while (0)
    if (dstsz > 0)
        dst[0] = 0;
    if (s->card == MSDOS_VID_OFF) {
        PUT("The PC is off\n");
        return n;
    }
    msdosvid_info inf;
    msdosvid_info_get(s, &inf);
    if (!kind || !*kind || !strcmp(kind, "info")) {
        PUT("%s\n%s\n", inf.line1, inf.line2);
        PUT("Picture %dx%d, %d colours", inf.width, inf.height, inf.colors);
        if (inf.bios_mode >= 0)
            PUT(", probably BIOS mode %02Xh", inf.bios_mode);
        PUT("\n");
        if (s->render[0])
            PUT("86Box draws it with %s\n", s->render);
        PUT("VRAM %u KB, start byte %05X, stride %u, pitch %u units%s\n", s->vram_size / 1024,
            s->start_byte, s->stride, s->pitch_units, s->addr_exact ? "" : " (guessed)");
    } else if (!strcmp(kind, "beam")) {
        PUT("Line %d of %d (vc %d, scanline %d), %s, display %s\n", s->displine,
            s->vtotal ? s->vtotal : (s->crtc[4] + 1) * ((s->crtc[9] & 0x1F) + 1), s->vc, s->scanline,
            s->linepos ? "end of line" : "active", s->dispon ? "on" : "off (vertical blank)");
        PUT("%.2f MHz dots, %.2f kHz lines, %.1f Hz frames\n", inf.dot_mhz, inf.line_khz, inf.frame_hz);
    } else if (!strcmp(kind, "regs")) {
        msdosvid_reg r[220];
        int          k = msdosvid_regs(s, NULL, r, 220);
        for (int i = 0; i < k; i++) {
            if (r[i].port && r[i].index < 0)
                PUT("%-10s %03X          %-22s %0*X  %s\n", r[i].group, r[i].port, r[i].name,
                    r[i].digits, r[i].value, r[i].meaning);
            else
                PUT("%-10s %02X           %-22s %0*X  %s\n", r[i].group, r[i].index, r[i].name,
                    r[i].digits, r[i].value, r[i].meaning);
        }
    } else if (!strcmp(kind, "pal")) {
        msdosvid_pal_entry e[256];
        int                k = msdosvid_palette(s, NULL, e, 256);
        if (!k)
            PUT("This card has no palette\n");
        for (int i = 0; i < k; i++)
            PUT("%3d  #%06X  %s%s\n", e[i].index, e[i].rgb, e[i].detail, e[i].active ? "" : "  (unused)");
    } else if (!strcmp(kind, "map")) {
        msdosvid_region r[12];
        int             k = msdosvid_regions(s, r, 12);
        for (int i = 0; i < k; i++) {
            PUT("%-26s VRAM %05X + %05X", r[i].name, r[i].vram_off, r[i].size);
            if (r[i].cpu_addr != MSDOS_VID_NONE)
                PUT("  CPU %05X", r[i].cpu_addr);
            PUT("\n");
        }
    } else {
        PUT("video [info|beam|regs|pal|map]\n");
    }
    return n;
#undef PUT
}
