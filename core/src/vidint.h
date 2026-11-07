/*
 * The Video tab's internals shared by vidview.c and vidrender.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_VIDINT_H
#define MSDOS_VIDINT_H

#include "msdosvid.h"

int      vid_pcjr_select(const msdos_vid_snap *s);
int      vid_pcjr_banks(const msdos_vid_snap *s);
int      vid_ega_text_cell_w(const msdos_vid_snap *s);
uint32_t vid_rgbi(int i);
void     vid_cga_gfx_palette(const msdos_vid_snap *s, uint32_t pal[4]);

#endif
