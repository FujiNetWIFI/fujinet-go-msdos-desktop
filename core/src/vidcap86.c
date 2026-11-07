/*
 * The Video tab's capture: copies the card's registers, palette, beam and VRAM into a
 * msdos_vid_snap. Emulation thread only (debug.cpp calls it from a host job).
 *
 * It only reads the cards' own structs. It never goes through the CPU's memory path: reading
 * A0000-BFFFF that way runs the card's read handlers, which move the EGA/VGA latches.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "debug86.h"
#include "msdosvid.h"

#include <86box/86box.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/lpt.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/vid_mda.h>
#include <86box/vid_cga.h>
#include <86box/vid_ega.h>
#include <86box/vid_8514a.h>
#include <86box/vid_xga.h>
#include <86box/vid_svga.h>
#include <86box/vid_svga_render.h>
#include <86box/m_pcjr.h>

static uint32_t
pow2_ceil(uint32_t n)
{
    uint32_t p = 1;
    while (p < n && p < (1u << 30))
        p <<= 1;
    return p;
}

/* (re)allocate s->vram for `size` bytes (a power of two) and return it, or NULL */
static uint8_t *
vram_buf(msdos_vid_snap *s, uint32_t size)
{
    if (s->vram && s->vram_size == size)
        return s->vram;
    free(s->vram);
    s->vram = malloc(size);
    s->vram_size = s->vram ? size : 0;
    return s->vram;
}

static void
copy_vram(msdos_vid_snap *s, const uint8_t *src, uint32_t have, uint32_t size)
{
    if (!vram_buf(s, size))
        return;
    memset(s->vram, 0, size);
    if (src)
        memcpy(s->vram, src, have < size ? have : size);
}

/* 86Box's render routines, by name */
static const struct {
    void (*fn)(svga_t *);
    const char *name;
} g_svga_render[] = {
    { svga_render_null, "svga_render_null" },
    { svga_render_blank, "svga_render_blank" },
    { svga_render_text_40, "svga_render_text_40" },
    { svga_render_text_80, "svga_render_text_80" },
    { svga_render_2bpp_lowres, "svga_render_2bpp_lowres" },
    { svga_render_2bpp_highres, "svga_render_2bpp_highres" },
    { svga_render_4bpp_lowres, "svga_render_4bpp_lowres" },
    { svga_render_4bpp_highres, "svga_render_4bpp_highres" },
    { svga_render_8bpp_lowres, "svga_render_8bpp_lowres" },
    { svga_render_8bpp_highres, "svga_render_8bpp_highres" },
    { svga_render_4bpp_tseng_highres, "svga_render_4bpp_tseng_highres" },
    { svga_render_8bpp_clone_highres, "svga_render_8bpp_clone_highres" },
    { svga_render_8bpp_tseng_lowres, "svga_render_8bpp_tseng_lowres" },
    { svga_render_8bpp_tseng_highres, "svga_render_8bpp_tseng_highres" },
    { svga_render_15bpp_lowres, "svga_render_15bpp_lowres" },
    { svga_render_15bpp_highres, "svga_render_15bpp_highres" },
    { svga_render_16bpp_lowres, "svga_render_16bpp_lowres" },
    { svga_render_16bpp_highres, "svga_render_16bpp_highres" },
    { svga_render_24bpp_lowres, "svga_render_24bpp_lowres" },
    { svga_render_24bpp_highres, "svga_render_24bpp_highres" },
    { svga_render_32bpp_lowres, "svga_render_32bpp_lowres" },
    { svga_render_32bpp_highres, "svga_render_32bpp_highres" },
};

static void
set_name(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

/* The fields EGA and SVGA share, copied with one macro (their structs name them alike). */
#define FILL_VGAISH(s, c)                                                                    \
    do {                                                                                     \
        memcpy((s)->crtc, (c)->crtc, 256);                                                   \
        memcpy((s)->seq, (c)->seqregs, 64);                                                  \
        memcpy((s)->gdc, (c)->gdcreg, 16);                                                   \
        memcpy((s)->attr, (c)->attrregs, 32);                                                \
        memcpy((s)->egapal, (c)->egapal, 16);                                                \
        (s)->miscout = (c)->miscout;                                                         \
        (s)->htotal = (c)->htotal;                                                           \
        (s)->hdisp = (c)->hdisp;                                                             \
        (s)->vtotal = (c)->vtotal;                                                           \
        (s)->dispend = (c)->dispend;                                                         \
        (s)->vsyncstart = (c)->vsyncstart;                                                   \
        (s)->split = (c)->split;                                                             \
        (s)->rowoffset = (c)->rowoffset;                                                     \
        (s)->rowcount = (c)->rowcount;                                                       \
        (s)->bpp = (c)->bpp;                                                                 \
        (s)->chain4 = (c)->chain4;                                                           \
        (s)->lowres = (c)->lowres;                                                           \
        (s)->interlace = (c)->interlace;                                                     \
        (s)->linedbl = (c)->linedbl;                                                         \
        (s)->displine = (c)->displine;                                                       \
        (s)->vc = (c)->vc;                                                                   \
        (s)->scanline = (c)->scanline;                                                       \
        (s)->linepos = (c)->linepos;                                                         \
        (s)->dispon = (c)->dispon;                                                           \
        (s)->firstline = (c)->firstline;                                                     \
        (s)->lastline = (c)->lastline;                                                       \
        (s)->memaddr = (uint32_t) (c)->memaddr;                                              \
        (s)->memaddr_latch = (uint32_t) (c)->memaddr_latch;                                  \
        (s)->cursoraddr = (uint32_t) (c)->cursoraddr;                                        \
        (s)->charseta = (uint32_t) (c)->charseta;                                            \
        (s)->charsetb = (uint32_t) (c)->charsetb;                                            \
        (s)->pitch_units = (uint32_t) (c)->rowoffset << 1;                                   \
        (s)->planar = 1;                                                                     \
    } while (0)

/*
 * Ask the card's own CRTC-address -> VRAM-byte mapping. The renderers work on
 * memaddr = (CRTC address << 2), step it by 4 per fetch unit, and hand it to remap_func:
 * so one unit is remap(base+4) - remap(base) bytes (4 in byte mode, 8 in word mode, 16 in
 * dword mode), and the start of the picture is remap(latch << 2).
 */
#define PROBE_REMAP(s, c, mask)                                                              \
    do {                                                                                     \
        uint32_t b4_ = ((s)->memaddr_latch) << 2;                                            \
        (s)->stride = 4;                                                                     \
        (s)->addr_exact = 0;                                                                 \
        (s)->start_byte = b4_ & (mask);                                                      \
        if ((c)->remap_func) {                                                               \
            uint32_t r0 = (c)->remap_func((c), b4_), r1 = (c)->remap_func((c), b4_ + 4),     \
                     r2 = (c)->remap_func((c), b4_ + 8);                                     \
            if (r1 > r0 && r2 - r1 == r1 - r0) {                                             \
                (s)->stride = r1 - r0;                                                       \
                (s)->addr_exact = 1;                                                         \
            }                                                                                \
            (s)->start_byte = r0 & (mask);                                                   \
        }                                                                                    \
    } while (0)

static void
cpu_window(msdos_vid_snap *s)
{
    switch ((s->gdc[6] >> 2) & 3) {
        case 0:
            s->cpu_base = 0xA0000;
            s->cpu_size = 0x20000;
            break;
        case 1:
            s->cpu_base = 0xA0000;
            s->cpu_size = 0x10000;
            break;
        case 2:
            s->cpu_base = 0xB0000;
            s->cpu_size = 0x8000;
            break;
        default:
            s->cpu_base = 0xB8000;
            s->cpu_size = 0x8000;
            break;
    }
}

/* where in the live frame is the beam: the same fraction down the picture */
static void
beam_in_frame(msdos_vid_snap *s, int blank_lines, int active_lines)
{
    s->beam_row = -1;
    if (s->fb_h <= 0 || active_lines <= 0)
        return;
    int line = s->displine - blank_lines;
    if (line < 0 || line >= active_lines)
        return;
    int row = (int) ((int64_t) line * s->fb_h / active_lines);
    s->beam_row = row < s->fb_h ? row : s->fb_h - 1;
}

static void
copy_frame_buffer(msdos_vid_snap *s)
{
    free(s->fb);
    s->fb = NULL;
    s->fb_w = s->fb_h = 0;
    s->beam_row = -1;
    bitmap_t *b = monitors[0].target_buffer;
    int       w = monitors[0].mon_xsize, h = monitors[0].mon_ysize;
    if (!b || w <= 0 || h <= 0 || w > 2048 || h > 2048)
        return;
    int xs = enable_overscan ? 0 : (monitors[0].mon_overscan_x >> 1);
    int ys = enable_overscan ? 0 : (monitors[0].mon_overscan_y >> 1);
    s->fb = malloc((size_t) w * (size_t) h * 4);
    if (!s->fb)
        return;
    for (int y = 0; y < h; y++) {
        const uint32_t *line = b->line[y + ys];
        if (!line) {
            memset(&s->fb[(size_t) y * (size_t) w], 0, (size_t) w * 4);
            continue;
        }
        for (int x = 0; x < w; x++)
            s->fb[(size_t) y * (size_t) w + (size_t) x] = line[x + xs] & 0xFFFFFF;
    }
    s->fb_w = w;
    s->fb_h = h;
}

int
d86_video_snap(msdos_vid_snap *s)
{
    uint8_t  *keep_vram = s->vram;
    uint32_t  keep_size = s->vram_size;
    uint32_t *keep_fb = s->fb;
    memset(s, 0, sizeof *s);
    s->vram = keep_vram;
    s->vram_size = keep_size;
    s->fb = keep_fb;
    s->beam_row = -1;

    const char *nm = video_get_internal_name(gfxcard[0]);
    if (!nm)
        return -1;

    if (!strcmp(nm, "mda")) {
        mda_t *m = device_get_priv(&mda_device);
        if (!m)
            return -1;
        s->card = MSDOS_VID_MDA;
        set_name(s->card_name, sizeof s->card_name, "IBM MDA");
        memcpy(s->crtc, m->crtc, sizeof m->crtc);
        s->crtc_count = 18;
        s->crtc_index = m->crtcreg;
        s->mode_reg = m->mode;
        s->status = m->status;
        s->displine = m->displine;
        s->vc = m->vc;
        s->scanline = m->scanline;
        s->linepos = m->linepos;
        s->dispon = m->dispon;
        s->firstline = m->firstline;
        s->lastline = m->lastline;
        s->memaddr = m->memaddr;
        s->cursoraddr = (uint32_t) (m->crtc[0x0E] << 8 | m->crtc[0x0F]);
        s->vtotal = (m->crtc[4] + 1) * ((m->crtc[9] & 0x1F) + 1) + m->crtc[5];
        s->vsyncstart = m->crtc[7] * ((m->crtc[9] & 0x1F) + 1);
        s->dispend = m->crtc[6] * ((m->crtc[9] & 0x1F) + 1);
        copy_vram(s, m->vram, 4096, 4096);
        s->cpu_base = 0xB0000;
        s->cpu_size = 0x8000;
        s->stride = 2;
        s->addr_exact = 1;
        for (int c = 0; c < 256; c++)
            memcpy(&s->font[c * 16], fontdatm[c + (m->fontbase ? 256 : 0)], 14);
        s->font_h = 14;
        copy_frame_buffer(s);
        beam_in_frame(s, s->vtotal - s->vsyncstart, s->dispend);
        return 0;
    }

    if (!strcmp(nm, "cga")) {
        cga_t *c = device_get_priv(&cga_device);
        if (!c)
            return -1;
        s->card = MSDOS_VID_CGA;
        set_name(s->card_name, sizeof s->card_name, "IBM CGA");
        memcpy(s->crtc, c->crtc, sizeof c->crtc);
        s->crtc_count = 18;
        s->crtc_index = c->crtcreg;
        s->mode_reg = c->cgamode;
        s->color_reg = c->cgacol;
        s->status = c->cgastat;
        s->displine = c->displine;
        s->vc = c->vc;
        s->scanline = c->scanline;
        s->linepos = c->linepos;
        s->dispon = c->cgadispon;
        s->firstline = c->firstline;
        s->lastline = c->lastline;
        s->memaddr = c->memaddr;
        s->cursoraddr = (uint32_t) (c->crtc[0x0E] << 8 | c->crtc[0x0F]);
        s->fontbase = c->fontbase;
        s->vtotal = (c->crtc[4] + 1) * ((c->crtc[9] & 0x1F) + 1) + c->crtc[5];
        s->vsyncstart = c->crtc[7] * ((c->crtc[9] & 0x1F) + 1);
        s->dispend = c->crtc[6] * ((c->crtc[9] & 0x1F) + 1);
        copy_vram(s, c->vram, 16384, 16384);
        s->cpu_base = 0xB8000;
        s->cpu_size = 0x4000;
        s->stride = 2;
        s->addr_exact = 1;
        for (int ch = 0; ch < 256; ch++)
            memcpy(&s->font[ch * 16], fontdat[ch + c->fontbase], 8);
        s->font_h = 8;
        copy_frame_buffer(s);
        beam_in_frame(s, s->vtotal - s->vsyncstart, s->dispend);
        return 0;
    }

    if (!strcmp(nm, "internal")) {
        pcjr_t *p = device_get_priv(&pcjr_device);
        if (!p || !p->vram)
            return -1;
        s->card = MSDOS_VID_PCJR;
        set_name(s->card_name, sizeof s->card_name, "IBM PCjr video");
        memcpy(s->crtc, p->crtc, sizeof p->crtc);
        s->crtc_count = 18;
        s->crtc_index = p->crtcreg;
        memcpy(s->gate, p->array, sizeof p->array);
        s->memctrl = (uint8_t) p->memctrl;
        s->status = p->status;
        s->displine = p->displine;
        s->vc = p->vc;
        s->scanline = p->scanline;
        s->linepos = p->linepos;
        s->dispon = p->dispon;
        s->firstline = p->firstline;
        s->lastline = p->lastline;
        s->memaddr = p->memaddr;
        s->cursoraddr = (uint32_t) (p->crtc[0x0E] << 8 | p->crtc[0x0F]);
        s->vtotal = (p->crtc[4] + 1) * ((p->crtc[9] & 0x1F) + 1) + p->crtc[5];
        s->vsyncstart = p->crtc[7] * ((p->crtc[9] & 0x1F) + 1);
        s->dispend = p->crtc[6] * ((p->crtc[9] & 0x1F) + 1);
        uint32_t phys = (uint32_t) (p->vram - ram);
        uint32_t have = (uint32_t) mem_size * 1024u > phys ? (uint32_t) mem_size * 1024u - phys : 0;
        copy_vram(s, p->vram, have, 32768);
        s->vram_phys = phys;
        s->cpu_base = 0xB8000;
        s->cpu_size = 0x4000;
        s->stride = 2;
        s->addr_exact = 1;
        for (int ch = 0; ch < 256; ch++)
            memcpy(&s->font[ch * 16], fontdat[ch], 8);
        s->font_h = 8;
        copy_frame_buffer(s);
        beam_in_frame(s, s->vtotal - s->vsyncstart, s->dispend);
        return 0;
    }

    if (!strcmp(nm, "ega")) {
        ega_t *e = device_get_priv(&ega_device);
        if (!e || !e->vram)
            return -1;
        s->card = MSDOS_VID_EGA;
        set_name(s->card_name, sizeof s->card_name, "IBM EGA");
        FILL_VGAISH(s, e);
        s->crtc_count = 25;
        s->seq_count = 5;
        s->crtc_index = e->crtcreg;
        s->status = e->status;
        for (int i = 0; i < 16; i++)
            s->pal16[i] = e->pallook ? e->pallook[e->egapal[i] & 63] & 0xFFFFFF : 0;
        uint32_t size = pow2_ceil((uint32_t) e->vrammask + 1);
        if (size > MSDOS_VID_VRAM_CAP)
            size = MSDOS_VID_VRAM_CAP;
        copy_vram(s, e->vram, size, size);
        s->vram_mask = size - 1;
        PROBE_REMAP(s, e, size - 1);
        cpu_window(s);
        if (e->render == ega_render_text)
            set_name(s->render, sizeof s->render, "ega_render_text");
        else if (e->render == ega_render_graphics)
            set_name(s->render, sizeof s->render, "ega_render_graphics");
        else if (e->render == ega_render_blank)
            set_name(s->render, sizeof s->render, "ega_render_blank");
        copy_frame_buffer(s);
        beam_in_frame(s, s->vtotal > s->vsyncstart ? s->vtotal - s->vsyncstart : 0, s->dispend);
        return 0;
    }

    if (!strcmp(nm, "vga") || !strcmp(nm, "tvga8900")) {
        svga_t *v = svga_get_pri();
        if (!v || !v->vram)
            return -1;
        s->card = !strcmp(nm, "vga") ? MSDOS_VID_VGA : MSDOS_VID_TVGA;
        set_name(s->card_name, sizeof s->card_name, s->card == MSDOS_VID_VGA ? "IBM VGA" : "Trident TVGA8900");
        FILL_VGAISH(s, v);
        s->crtc_count = s->card == MSDOS_VID_TVGA ? 0x40 : 25;
        s->seq_count = s->card == MSDOS_VID_TVGA ? 0x10 : 5;
        s->crtc_index = v->crtcreg;
        s->status = v->cgastat;
        s->dac_mask = v->dac_mask;
        s->dac_valid = 1;
        for (int i = 0; i < 256; i++) {
            s->dac[i][0] = v->vgapal[i].r;
            s->dac[i][1] = v->vgapal[i].g;
            s->dac[i][2] = v->vgapal[i].b;
            s->pal256[i] = v->pallook[i & v->dac_mask] & 0xFFFFFF;
        }
        for (int i = 0; i < 16; i++)
            s->pal16[i] = v->pallook[v->egapal[i] & v->dac_mask] & 0xFFFFFF;
        uint32_t size = pow2_ceil(v->vram_mask + 1);
        if (size > MSDOS_VID_VRAM_CAP)
            size = MSDOS_VID_VRAM_CAP;
        copy_vram(s, v->vram, size, size);
        s->vram_mask = size - 1;
        PROBE_REMAP(s, v, size - 1);
        cpu_window(s);
        for (size_t i = 0; i < sizeof g_svga_render / sizeof g_svga_render[0]; i++)
            if (v->render == g_svga_render[i].fn) {
                set_name(s->render, sizeof s->render, g_svga_render[i].name);
                break;
            }
        copy_frame_buffer(s);
        beam_in_frame(s, s->vtotal > s->vsyncstart ? s->vtotal - s->vsyncstart : 0, s->dispend);
        return 0;
    }
    return -1;
}
