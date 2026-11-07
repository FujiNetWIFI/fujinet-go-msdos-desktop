/*
 * The Video tab's decoders, views, hit test, registers, palette and tracker, on synthetic
 * snapshots: no ROMs, no machine. Every pixel buffer must be opaque (0xFF alpha): KWin
 * composites an alpha-0 Format_RGB32 picture as see-through.
 */
#include "test_util.h"

#include "msdosvid.h"

static uint32_t g_buf[1 << 22];

static msdos_vid_snap
new_snap(int card, uint32_t vram)
{
    msdos_vid_snap s;
    memset(&s, 0, sizeof s);
    s.card = card;
    s.vram_size = vram;
    s.vram = calloc(1, vram);
    CHECK(s.vram, "alloc");
    s.stride = 2;
    s.addr_exact = 1;
    return s;
}

static void
all_opaque(const uint32_t *p, int w, int h, const char *what)
{
    for (int i = 0; i < w * h; i++)
        CHECK((p[i] >> 24) == 0xFF, "%s: pixel %d has alpha %02X", what, i, p[i] >> 24);
}

static uint32_t
px(const uint32_t *p, int w, int x, int y, int z)
{
    return p[(y * z + z / 2) * (w) + x * z + z / 2] & 0xFFFFFF;
}

static int
render(const msdosvid_ctx *c, int view, unsigned ov, int zoom, int *w, int *h)
{
    int rc = msdosvid_render(c, view, ov, zoom, g_buf, sizeof g_buf / 4, w, h);
    if (rc == 0)
        all_opaque(g_buf, *w, *h, msdosvid_view_name(view));
    return rc;
}

/* ---- CGA text ---------------------------------------------------------------------- */

static void
cga_text_snap(msdos_vid_snap *s)
{
    *s = new_snap(MSDOS_VID_CGA, 16384);
    s->mode_reg = 0x09; /* 80 columns, video on */
    s->crtc[0] = 0x71;
    s->crtc[1] = 80;
    s->crtc[4] = 0x1F;
    s->crtc[5] = 6;
    s->crtc[6] = 25;
    s->crtc[7] = 0x1C;
    s->crtc[9] = 7;
    s->crtc[10] = 6;
    s->crtc[11] = 7;
    s->cpu_base = 0xB8000;
    /* an 'A' (41h) in yellow on blue at column 3, row 2; the glyph is a solid column */
    s->vram[(2 * 80 + 3) * 2] = 0x41;
    s->vram[(2 * 80 + 3) * 2 + 1] = 0x1E;
    for (int r = 0; r < 8; r++)
        s->font[0x41 * 16 + r] = 0xFF;
    s->font_h = 8;
    s->displine = 100;
    s->dispon = 1;
}

static void
test_cga_text(void)
{
    msdos_vid_snap s;
    cga_text_snap(&s);
    msdosvid_info in;
    CHECK(msdosvid_info_get(&s, &in) == 0, "info");
    CHECK(in.mode == MSDOS_VM_TEXT && in.cols == 80 && in.rows == 25, "cga text: %dx%d", in.cols, in.rows);
    CHECK(in.width == 640 && in.height == 200, "cga text size %dx%d", in.width, in.height);
    CHECK(in.line_khz > 15 && in.line_khz < 16.5, "cga line rate %.2f", in.line_khz);
    CHECK(in.frame_hz > 55 && in.frame_hz < 65, "cga frame rate %.2f", in.frame_hz);

    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render");
    CHECK(w == 640 && h == 200, "size %dx%d", w, h);
    CHECK(px(g_buf, w, 3 * 8 + 4, 2 * 8 + 3, 1) == 0xFFFF55, "fg yellow, got %06X", px(g_buf, w, 3 * 8 + 4, 2 * 8 + 3, 1));
    CHECK(px(g_buf, w, 10 * 8 + 1, 10, 1) == 0x000000, "background black");

    /* attribute 1E -> fg yellow, bg blue on a cell with an empty glyph */
    s.vram[(2 * 80 + 4) * 2] = 0x20;
    s.vram[(2 * 80 + 4) * 2 + 1] = 0x1E;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render 2");
    CHECK(px(g_buf, w, 4 * 8 + 4, 2 * 8 + 3, 1) == 0x0000AA, "bg blue, got %06X", px(g_buf, w, 4 * 8 + 4, 2 * 8 + 3, 1));

    /* hit: zoomed 3x, a point inside cell (3,2) */
    msdosvid_pixel hp;
    CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 3, (3 * 8 + 2) * 3, (2 * 8 + 2) * 3, &hp) == 0, "hit");
    CHECK(hp.col == 3 && hp.row == 2 && hp.chr == 0x41 && hp.value == 0x1E, "hit cell %d,%d chr %02X attr %02X",
          hp.col, hp.row, hp.chr, hp.value);
    CHECK(hp.voff == (uint32_t) ((2 * 80 + 3) * 2), "hit voff %X", hp.voff);
    CHECK(hp.cpu_addr == 0xB8000u + (2 * 80 + 3) * 2, "hit cpu %X", hp.cpu_addr);
    CHECK(strstr(hp.text, "char 41") != NULL, "inspector text: %s", hp.text);
    CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 3, 100000, 5, &hp) == -1, "off the picture");

    /* the cell rectangle */
    int rx, ry, rw, rh;
    CHECK(msdosvid_cell_rect(&c, MSDOSVID_V_DECODED, 3, (3 * 8 + 2) * 3, (2 * 8 + 2) * 3, &rx, &ry, &rw, &rh) == 0, "rect");
    CHECK(rx == 3 * 8 * 3 && ry == 2 * 8 * 3 && rw == 24 && rh == 24, "rect %d,%d %dx%d", rx, ry, rw, rh);

    /* zoom and every overlay stay opaque and in bounds */
    CHECK(render(&c, MSDOSVID_V_DECODED, (1u << MSDOSVID_O_COUNT) - 1, 16, &w, &h) == 0, "all overlays");
    CHECK((size_t) w * h <= MSDOSVID_MAX_PIXELS, "zoom is clamped to the pixel cap (%dx%d)", w, h);
    CHECK(render(&c, MSDOSVID_V_VALUES, msdosvid_overlay_defaults(), 4, &w, &h) == 0, "values");
    CHECK(render(&c, MSDOSVID_V_FONT, 0, 2, &w, &h) == 0, "font");
    CHECK(render(&c, MSDOSVID_V_MEMORY, msdosvid_overlay_defaults(), 2, &w, &h) == 0, "memory");
    CHECK(msdosvid_view_why(&c, MSDOSVID_V_PLANES) != NULL, "planes need an EGA/VGA");
    CHECK(msdosvid_view_why(&c, MSDOSVID_V_DISPLAY) != NULL, "display needs a frame");
    CHECK(render(&c, MSDOSVID_V_DISPLAY, 0, 1, &w, &h) == -1 && w == 0 && h == 0, "an unavailable view is 0x0");

    /* size only */
    CHECK(msdosvid_render(&c, MSDOSVID_V_DECODED, 0, 2, NULL, 0, &w, &h) == 0 && w == 1280 && h == 400, "size query");
    msdos_vid_snap_free(&s);
}

/* ---- CGA 320x200x4 ------------------------------------------------------------------ */

static void
test_cga_gfx(void)
{
    msdos_vid_snap s = new_snap(MSDOS_VID_CGA, 16384);
    s.mode_reg = 0x0A;  /* graphics, video on, colour */
    s.color_reg = 0x21; /* palette 1 (cyan, magenta, white), background blue */
    s.crtc[1] = 40;
    s.crtc[4] = 0x7F;
    s.crtc[6] = 100;
    s.crtc[9] = 1;
    s.cpu_base = 0xB8000;
    /* row 0: pixels 0..3 = 0,1,2,3.  row 1 (the odd bank at 2000h): 3,3,3,3 */
    s.vram[0] = 0x1B;
    s.vram[0x2000] = 0xFF;
    msdosvid_info in;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_GFX_2 && in.width == 320 && in.height == 200, "cga gfx %dx%d", in.width, in.height);
    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render");
    CHECK(px(g_buf, w, 0, 0, 1) == 0x0000AA, "index 0 is the background, got %06X", px(g_buf, w, 0, 0, 1));
    CHECK(px(g_buf, w, 1, 0, 1) == 0x00AAAA, "index 1 cyan, got %06X", px(g_buf, w, 1, 0, 1));
    CHECK(px(g_buf, w, 2, 0, 1) == 0xAA00AA, "index 2 magenta, got %06X", px(g_buf, w, 2, 0, 1));
    CHECK(px(g_buf, w, 3, 0, 1) == 0xAAAAAA, "index 3 white, got %06X", px(g_buf, w, 3, 0, 1));
    CHECK(px(g_buf, w, 2, 1, 1) == 0xAAAAAA, "odd rows come from the second bank, got %06X", px(g_buf, w, 2, 1, 1));
    msdosvid_pixel hp;
    CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 1, 2, 1, &hp) == 0 && hp.voff == 0x2000 && hp.value == 3, "hit in bank 1: %X", hp.voff);
    CHECK(hp.cpu_addr == 0xBA000, "cpu address %X", hp.cpu_addr);

    s.color_reg = 0x11; /* palette 0 intense: light green, light red, yellow */
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render palette 0");
    CHECK(px(g_buf, w, 1, 0, 1) == 0x55FF55 && px(g_buf, w, 2, 0, 1) == 0xFF5555 && px(g_buf, w, 3, 0, 1) == 0xFFFF55,
          "palette 0 intense: %06X %06X %06X", px(g_buf, w, 1, 0, 1), px(g_buf, w, 2, 0, 1), px(g_buf, w, 3, 0, 1));

    /* 640x200x2 */
    s.mode_reg = 0x1A;
    s.color_reg = 0x0F;
    s.crtc[1] = 40;
    s.vram[0] = 0x80;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_GFX_1 && in.width == 640, "mode 6: %d", in.width);
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render mode 6");
    CHECK(px(g_buf, w, 0, 0, 1) == 0xFFFFFF && px(g_buf, w, 1, 0, 1) == 0, "1 bpp");
    msdos_vid_snap_free(&s);
}

/* ---- PCjr --------------------------------------------------------------------------- */

static void
test_pcjr(void)
{
    msdos_vid_snap s = new_snap(MSDOS_VID_PCJR, 32768);
    s.crtc[1] = 40;
    s.crtc[6] = 25;
    s.crtc[9] = 7;
    s.cpu_base = 0xB8000;
    s.gate[0] = 0x0B; /* graphics, video on, 1 = high bandwidth; 16-colour off */
    s.gate[0] = 0x1A; /* 16-colour graphics, low bandwidth: 160x200x16 */
    s.gate[1] = 0x0F;
    for (int i = 0; i < 16; i++)
        s.gate[16 + i] = (uint8_t) i;
    s.memctrl = 0x40 | 0; /* address mode 1: two banks */
    s.crtc[9] = 1;
    s.crtc[6] = 100;
    s.vram[0] = 0x1F; /* pixels 0 and 1: colours 1 and 15 */
    s.vram[1] = 0x23;
    msdosvid_info in;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_GFX_4, "pcjr mode %d", in.mode);
    CHECK(in.bank_count == 2, "banks %d", in.bank_count);
    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, MSDOSVID_O_BANKS, 1, &w, &h) == 0, "render");
    CHECK(px(g_buf, w, 0, 0, 1) != px(g_buf, w, 1, 0, 1), "two different pixels");
    msdos_vid_snap_free(&s);
}

/* ---- VGA ---------------------------------------------------------------------------- */

static void
vga_base(msdos_vid_snap *s, uint32_t vram)
{
    *s = new_snap(MSDOS_VID_VGA, vram);
    s->planar = 1;
    s->stride = 4;
    s->vram_mask = vram - 1;
    s->cpu_base = 0xA0000;
    s->cpu_size = 0x10000;
    s->dac_valid = 1;
    s->dac_mask = 0xFF;
    for (int i = 0; i < 16; i++) {
        s->egapal[i] = (uint8_t) i;
        s->pal16[i] = 0x111111u * (uint32_t) i;
    }
    for (int i = 0; i < 256; i++) {
        s->dac[i][0] = (uint8_t) (i & 63);
        s->pal256[i] = (uint32_t) i * 0x010101u;
    }
}

static void
test_vga_planar(void)
{
    msdos_vid_snap s;
    vga_base(&s, 262144);
    s.gdc[6] = 5; /* graphics, A0000 64K */
    s.attr[0x10] = 1;
    s.crtc[1] = 79;
    s.crtc[9] = 0x40;
    s.dispend = 480;
    s.rowoffset = 40;
    s.pitch_units = 80;
    s.crtc_count = 25;
    s.seq[1] = 1;
    s.seq[2] = 15;
    s.seq[4] = 6;
    /* mode 12h: pixel (0,0) = plane bits 1,0,1,1 -> 13; pixel (1,0) = 0 ; pixel (9,1) = 5 */
    s.vram[0] = 0x80;
    s.vram[1] = 0x00;
    s.vram[2] = 0x80;
    s.vram[3] = 0x80;
    s.vram[80 * 4 + 4] = 0x40;
    s.vram[80 * 4 + 6] = 0x40;
    msdosvid_info in;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_PLANAR_16 && in.width == 640 && in.height == 480, "mode 12h: %dx%d mode %d", in.width,
          in.height, in.mode);
    CHECK(in.bios_mode == 0x12, "bios mode %02X", in.bios_mode);
    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0 && w == 640 && h == 480, "render");
    CHECK(px(g_buf, w, 0, 0, 1) == 0xDDDDDD, "pixel (0,0) is index 13: %06X", px(g_buf, w, 0, 0, 1));
    CHECK(px(g_buf, w, 9, 1, 1) == 0x555555, "pixel (9,1) index 5: %06X", px(g_buf, w, 9, 1, 1));
    msdosvid_pixel hp;
    CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 2, 9 * 2, 1 * 2, &hp) == 0 && hp.value == 5, "hit value %d", hp.value);
    CHECK(hp.voff == 80 * 4 + 4, "hit voff %X", hp.voff);
    CHECK(hp.cpu_addr == 0xA0000u + 80 + 1, "cpu addr %X", hp.cpu_addr);

    /* planes: plane 0 of pixel (0,0) is set, plane 1 is not */
    CHECK(render(&c, MSDOSVID_V_PLANES, 0, 1, &w, &h) == 0 && w == 640 * 2 + 4 && h == 480 * 2 + 4, "planes %dx%d", w, h);
    CHECK(px(g_buf, w, 0, 0, 1) != 0x14141C, "plane 0 bit set");
    CHECK(px(g_buf, w, 644 + 0, 0, 1) == 0x14141C, "plane 1 bit clear: %06X", px(g_buf, w, 644, 0, 1));

    /* split screen: rows from 200 down show the picture from address 0 again */
    s.split = 200;
    msdosvid_info_get(&s, &in);
    CHECK(in.split_row == 200, "split row %d", in.split_row);
    s.start_byte = 80 * 4 * 100;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render split");
    CHECK(px(g_buf, w, 0, 200, 1) == 0xDDDDDD, "the split part starts at address 0: %06X", px(g_buf, w, 0, 200, 1));
    CHECK(px(g_buf, w, 0, 0, 1) != 0xDDDDDD, "above the split it starts at the start address: %06X", px(g_buf, w, 0, 0, 1));
    msdos_vid_snap_free(&s);
}

static void
test_vga_256(void)
{
    msdos_vid_snap s;
    vga_base(&s, 262144);
    s.gdc[6] = 5;
    s.gdc[5] = 0x40;
    s.attr[0x10] = 0x41;
    s.seq[4] = 0x0E;
    s.crtc[1] = 79;
    s.crtc[9] = 0x41;
    s.dispend = 400;
    s.rowoffset = 40;
    s.pitch_units = 80;
    s.vram[0] = 7;
    s.vram[319] = 200;
    s.vram[320] = 9;
    s.vram[320 * 199 + 5] = 33;
    msdosvid_info in;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_PACKED_8 && in.width == 320 && in.height == 200, "mode 13h: %dx%d", in.width, in.height);
    CHECK(in.bios_mode == 0x13, "bios mode %02X", in.bios_mode);
    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0 && w == 320, "render");
    CHECK(px(g_buf, w, 0, 0, 1) == 0x070707, "pixel 0: %06X", px(g_buf, w, 0, 0, 1));
    CHECK(px(g_buf, w, 319, 0, 1) == 0xC8C8C8, "pixel 319: %06X", px(g_buf, w, 319, 0, 1));
    CHECK(px(g_buf, w, 0, 1, 1) == 0x090909, "row 1: %06X", px(g_buf, w, 0, 1, 1));
    CHECK(px(g_buf, w, 5, 199, 1) == 0x212121, "last row: %06X", px(g_buf, w, 5, 199, 1));
    msdosvid_pixel hp;
    CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 4, 5 * 4 + 1, 199 * 4 + 1, &hp) == 0 && hp.value == 33 && hp.voff == 320 * 199 + 5,
          "hit %d @%X", hp.value, hp.voff);
    /* digits at a big zoom still render and stay opaque */
    CHECK(render(&c, MSDOSVID_V_DECODED, MSDOSVID_O_DIGITS | MSDOSVID_O_GRID, 12, &w, &h) == 0, "digits");
    msdosvid_pal_entry pe[256];
    CHECK(msdosvid_palette(&s, NULL, pe, 256) == 256, "256 colour palette");
    s.attr[0x10] = 0x01;
    s.gdc[5] = 0;
    s.seq[4] = 6;
    CHECK(msdosvid_palette(&s, NULL, pe, 256) == 16, "16 colour palette");
    msdos_vid_snap_free(&s);
}

static void
test_vga_text(void)
{
    msdos_vid_snap s;
    vga_base(&s, 262144);
    s.gdc[6] = 0x0E;
    s.cpu_base = 0xB8000;
    s.attr[0x10] = 0x0C; /* text, line graphics, blink */
    s.crtc[1] = 79;
    s.crtc[9] = 15;
    s.dispend = 400;
    s.rowoffset = 40;
    s.pitch_units = 80;
    s.seq[1] = 0;     /* 9 dots */
    s.seq[4] = 3;     /* odd/even */
    s.charseta = 2;   /* font at plane 2 of the start of VRAM */
    s.charsetb = 2;
    /* cell (0,0): 'A' attr 1F; the font for 'A' has row 0 = 0xFF */
    s.vram[0] = 0x41;
    s.vram[1] = 0x1F;
    s.vram[2 + 0x41 * 128] = 0xFF;
    msdosvid_info in;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_TEXT && in.cols == 80 && in.rows == 25 && in.cell_w == 9 && in.cell_h == 16,
          "vga text %dx%d cell %dx%d", in.cols, in.rows, in.cell_w, in.cell_h);
    CHECK(in.width == 720 && in.height == 400, "720x400: %dx%d", in.width, in.height);
    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render");
    CHECK(px(g_buf, w, 3, 0, 1) == s.pal16[15], "glyph row 0 is fg: %06X", px(g_buf, w, 3, 0, 1));
    CHECK(px(g_buf, w, 3, 1, 1) == s.pal16[1], "glyph row 1 is bg: %06X", px(g_buf, w, 3, 1, 1));
    msdosvid_pixel hp;
    CHECK(msdosvid_hit(&c, MSDOSVID_V_DECODED, 1, 3, 1, &hp) == 0 && hp.chr == 0x41 && hp.value == 0x1F && hp.col == 0, "text hit");
    CHECK(hp.cpu_addr == 0xB8000, "text cpu address %X", hp.cpu_addr);
    msdos_vid_snap_free(&s);
}

/* ---- MDA ---------------------------------------------------------------------------- */

static void
test_mda(void)
{
    msdos_vid_snap s = new_snap(MSDOS_VID_MDA, 4096);
    s.mode_reg = 0x29;
    s.crtc[0] = 0x61;
    s.crtc[1] = 80;
    s.crtc[4] = 0x19;
    s.crtc[5] = 6;
    s.crtc[6] = 25;
    s.crtc[7] = 0x19;
    s.crtc[9] = 13;
    s.cpu_base = 0xB0000;
    s.vram[0] = 0x41;
    s.vram[1] = 0x70; /* inverse */
    for (int r = 0; r < 14; r++)
        s.font[0x41 * 16 + r] = 0xFF;
    msdosvid_info in;
    msdosvid_info_get(&s, &in);
    CHECK(in.mode == MSDOS_VM_TEXT && in.width == 720 && in.height == 350, "mda %dx%d", in.width, in.height);
    CHECK(in.line_khz > 18 && in.line_khz < 19, "mda line rate %.2f", in.line_khz);
    msdosvid_ctx c = { .snap = &s };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "render");
    CHECK(px(g_buf, w, 2, 2, 1) == 0x000000, "inverse video: ink is black, got %06X", px(g_buf, w, 2, 2, 1));
    CHECK(px(g_buf, w, 20, 2, 1) == 0x000000, "an empty cell is black");
    msdos_vid_snap_free(&s);
}

/* ---- the tracker, registers, palette, regions ----------------------------------------- */

static void
test_tracker(void)
{
    msdosvid_tracker t;
    msdosvid_tracker_init(&t);
    msdos_vid_snap a, b, c3;
    cga_text_snap(&a);
    msdosvid_tracker_push(&t, &a);
    CHECK(a.vram == NULL, "push takes the buffers");
    CHECK(t.cur.card == MSDOS_VID_CGA && !t.have_prev, "first push");
    cga_text_snap(&b);
    b.vram[100] = 0x55;
    b.crtc[10] = 0x20; /* hide the cursor */
    msdosvid_tracker_push(&t, &b);
    CHECK(t.have_prev && t.heat_n == 16384, "second push");
    CHECK(t.heat[100] == 255 && t.changed[100] == 1, "a changed byte is hot: %d", t.heat[100]);
    CHECK(t.heat[101] == 0 && t.changed[101] == 0, "an unchanged byte is cold");
    CHECK(t.reg_age[0][10] == 1 && t.reg_age[0][11] == 0, "register 10 just changed");
    cga_text_snap(&c3);
    c3.vram[100] = 0x55;
    c3.crtc[10] = 0x20;
    msdosvid_tracker_push(&t, &c3);
    CHECK(t.heat[100] == 204 && t.changed[100] == 0, "heat cools 20%%: %d", t.heat[100]);
    CHECK(t.reg_age[0][10] == 2, "register age grows: %d", t.reg_age[0][10]);
    int last = 255;
    for (int i = 0; i < 40; i++) {
        cga_text_snap(&c3);
        c3.vram[100] = 0x55;
        c3.crtc[10] = 0x20;
        msdosvid_tracker_push(&t, &c3);
        CHECK(t.heat[100] <= last, "heat never rises");
        last = t.heat[100];
    }
    CHECK(t.heat[100] == 0, "and reaches zero: %d", t.heat[100]);

    /* heat shows in the picture */
    cga_text_snap(&c3);
    c3.vram[(5 * 80) * 2] = 0x41;
    c3.vram[(5 * 80) * 2 + 1] = 0x07;
    msdosvid_tracker_push(&t, &c3);
    msdosvid_ctx c = { .snap = &t.cur, .track = &t };
    int          w, h;
    CHECK(render(&c, MSDOSVID_V_DECODED, MSDOSVID_O_HEAT, 1, &w, &h) == 0, "heat render");
    uint32_t hot = px(g_buf, w, 2, 5 * 8 + 3, 1);
    CHECK(render(&c, MSDOSVID_V_DECODED, 0, 1, &w, &h) == 0, "plain render");
    CHECK(hot != px(g_buf, w, 2, 5 * 8 + 3, 1), "the heat overlay tints a changed cell");

    msdosvid_reg regs[220];
    int          n = msdosvid_regs(&t.cur, &t, regs, 220);
    CHECK(n > 18, "registers: %d", n);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(regs[i].group, "CRTC") && regs[i].index == 10) {
            found = 1;
            CHECK(strstr(regs[i].name, "Cursor Start"), "name %s", regs[i].name);
        }
    CHECK(found, "CRTC R10 listed");
    msdosvid_pal_entry pe[256];
    CHECK(msdosvid_palette(&t.cur, &t, pe, 256) == 16, "cga palette");
    msdosvid_region rg[12];
    CHECK(msdosvid_regions(&t.cur, rg, 12) >= 2, "regions");
    char txt[8192];
    CHECK(msdosvid_text(&t.cur, "regs", txt, sizeof txt) > 100, "regs text");
    CHECK(msdosvid_text(&t.cur, "", txt, sizeof txt) > 20 && strstr(txt, "CGA"), "info text: %s", txt);
    msdosvid_tracker_free(&t);
}

/* A guest can program any register value: nothing may crash, overrun or come out transparent. */
static uint32_t g_rng = 0x12345678;

static uint32_t
rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void
test_fuzz(void)
{
    static const struct {
        int      card;
        uint32_t vram;
    } cards[] = { { MSDOS_VID_MDA, 4096 }, { MSDOS_VID_CGA, 16384 }, { MSDOS_VID_PCJR, 32768 },
                  { MSDOS_VID_EGA, 262144 }, { MSDOS_VID_VGA, 262144 }, { MSDOS_VID_TVGA, 1048576 } };
    msdosvid_tracker t;
    msdosvid_tracker_init(&t);
    for (int ci = 0; ci < 6; ci++)
        for (int it = 0; it < 14; it++) {
            msdos_vid_snap s = new_snap(cards[ci].card, cards[ci].vram);
            for (uint32_t i = 0; i < s.vram_size; i++)
                s.vram[i] = (uint8_t) (it % 3 == 0 ? 0 : rnd());
            for (int i = 0; i < 256; i++)
                s.crtc[i] = (uint8_t) rnd();
            for (int i = 0; i < 64; i++)
                s.seq[i] = (uint8_t) rnd();
            for (int i = 0; i < 16; i++) {
                s.gdc[i] = (uint8_t) rnd();
                s.egapal[i] = (uint8_t) rnd();
                s.pal16[i] = rnd() & 0xFFFFFF;
            }
            for (int i = 0; i < 32; i++) {
                s.attr[i] = (uint8_t) rnd();
                s.gate[i] = (uint8_t) rnd();
            }
            for (int i = 0; i < 256; i++)
                s.pal256[i] = rnd() & 0xFFFFFF;
            for (int i = 0; i < 256 * 16; i++)
                s.font[i] = (uint8_t) rnd();
            s.mode_reg = (uint8_t) rnd();
            s.color_reg = (uint8_t) rnd();
            s.memctrl = (uint8_t) rnd();
            s.dac_mask = (uint8_t) rnd();
            s.dac_valid = 1;
            s.planar = cards[ci].card >= MSDOS_VID_EGA;
            s.stride = s.planar ? (uint32_t) (1u << (rnd() % 5)) : 2;
            s.pitch_units = rnd() % 4096;
            s.start_byte = rnd() % s.vram_size;
            s.rowoffset = (int) (rnd() % 256);
            s.dispend = (int) (rnd() % 1200);
            s.vtotal = (int) (rnd() % 1500);
            s.vsyncstart = (int) (rnd() % 1500);
            s.split = (int) (rnd() % 1200);
            s.displine = (int) (rnd() % 2000);
            s.bpp = (int) (rnd() % 33);
            s.charseta = rnd() % (s.vram_size * 2);
            s.charsetb = rnd() % (s.vram_size * 2);
            s.cpu_base = 0xA0000;
            s.cpu_size = 0x10000;
            s.fb_w = (int) (rnd() % 700) + 1;
            s.fb_h = (int) (rnd() % 500) + 1;
            s.fb = calloc((size_t) s.fb_w * s.fb_h, 4);
            s.beam_row = (int) (rnd() % 600) - 50;
            msdosvid_tracker_push(&t, &s);
            msdosvid_ctx c = { .snap = &t.cur, .track = &t, .token = (uint64_t) (ci * 1000 + it + 1) };
            uint32_t disp[64 * 64];
            memset(disp, 0x55, sizeof disp);
            c.display = disp;
            c.display_w = 64;
            c.display_h = 64;
            msdosvid_info in;
            msdosvid_info_get(&t.cur, &in);
            for (int v = 0; v < MSDOSVID_V_COUNT; v++) {
                const unsigned ov = (rnd() & ((1u << MSDOSVID_O_COUNT) - 1));
                const int zoom = rnd() % 25 == 0 ? 16 : 1 + (int) (rnd() % 2);
                int w = 0, h = 0;
                if (render(&c, v, ov, zoom, &w, &h) == 0) {
                    CHECK((size_t) w * h <= MSDOSVID_MAX_PIXELS, "card %d view %d: %dx%d over the cap", ci, v, w, h);
                    msdosvid_pixel hp;
                    for (int k = 0; k < 20; k++) {
                        int rc = msdosvid_hit(&c, v, zoom, (int) (rnd() % (uint32_t) (w + 8)) - 4,
                                              (int) (rnd() % (uint32_t) (h + 8)) - 4, &hp);
                        CHECK(rc == 0 || rc == -1, "hit rc");
                        if (rc == 0) CHECK(strlen(hp.text) < sizeof hp.text, "hit text");
                    }
                }
                CHECK(msdosvid_fit_zoom(&c, v, 800, 600, 8) >= 1, "fit zoom");
            }
            int counts[256];
            msdosvid_usage(&c, counts, 256);
            msdosvid_reg regs[300];
            CHECK(msdosvid_regs(&t.cur, &t, regs, 300) <= 300, "regs");
            msdosvid_pal_entry pe[256];
            CHECK(msdosvid_palette(&t.cur, &t, pe, 256) <= 256, "palette");
            msdosvid_region rg[12];
            CHECK(msdosvid_regions(&t.cur, rg, 12) <= 12, "regions");
            static char txt[16384];
            const char *kinds[] = { "", "beam", "regs", "pal", "map" };
            for (int k = 0; k < 5; k++)
                CHECK(msdosvid_text(&t.cur, kinds[k], txt, (int) sizeof txt) < (int) sizeof txt, "text");
        }
    msdosvid_tracker_free(&t);
}

static void
test_names(void)
{
    for (int v = 0; v < MSDOSVID_V_COUNT; v++)
        CHECK(msdosvid_view_from_key(msdosvid_view_key(v)) == v, "view key %d", v);
    CHECK(msdosvid_view_from_key("nope") == -1, "unknown view");
    int                         n;
    const msdosvid_overlay_def *o = msdosvid_overlays(&n);
    CHECK(n == MSDOSVID_O_COUNT, "overlays");
    unsigned all = 0;
    for (int i = 0; i < n; i++) {
        CHECK(o[i].bit == (1u << i), "overlay %d has bit %u", i, o[i].bit);
        all |= o[i].bit;
        CHECK(msdosvid_overlays_from_keys(o[i].key) == o[i].bit, "overlay key %s", o[i].key);
    }
    CHECK(msdosvid_overlays_from_keys("all") == all, "all");
    CHECK(msdosvid_overlays_from_keys("none") == 0, "none");
    CHECK(msdosvid_overlays_from_keys("heat,grid") == (MSDOSVID_O_HEAT | MSDOSVID_O_GRID), "csv");
    CHECK(msdosvid_section_from_key("registers") == MSDOSVID_S_REGISTERS, "section key");
    msdos_vid_snap off;
    memset(&off, 0, sizeof off);
    char txt[64];
    CHECK(msdosvid_text(&off, "", txt, sizeof txt) > 0 && strstr(txt, "off"), "the PC is off: %s", txt);
    msdosvid_ctx c = { .snap = &off };
    int          w, h;
    CHECK(msdosvid_render(&c, MSDOSVID_V_DECODED, 0, 1, g_buf, 100, &w, &h) == -1, "nothing to draw when off");
}

int
main(void)
{
    test_names();
    test_fuzz();
    test_cga_text();
    test_cga_gfx();
    test_pcjr();
    test_vga_planar();
    test_vga_256();
    test_vga_text();
    test_mda();
    test_tracker();
    printf("vidview: ok\n");
    return 0;
}
