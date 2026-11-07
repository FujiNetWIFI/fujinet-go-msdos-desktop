/*
 * msdosvid -- the debugger's Video tab: a snapshot of the video card (registers,
 * palette, VRAM, beam) and everything the tab draws from one.
 *
 * Two halves:
 *   - msdosdebug_video_snap() (msdosdebug.h) fills a msdos_vid_snap on the emulation
 *     thread. It only copies; it never runs the card's read handlers (those move the
 *     EGA/VGA latches).
 *   - Everything here is pure C on a snapshot, called from the UI thread. It includes
 *     no 86Box headers and is tested on synthetic snapshots.
 *
 * All pixel buffers are 0xFFRRGGBB: the alpha byte is set, so a frontend that forgets to
 * ignore it (Qt's Format_RGB32 on KWin) still shows an opaque picture.
 *
 * Return conventions: functions that look something up return 0 on success and -1 when
 * there is nothing there (off the picture, no such register); counts return >= 0.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSVID_H
#define MSDOSVID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MSDOS_VID_OFF = 0, /* no card (the PC is off) */
    MSDOS_VID_MDA,
    MSDOS_VID_CGA,
    MSDOS_VID_PCJR,
    MSDOS_VID_EGA,
    MSDOS_VID_VGA,
    MSDOS_VID_TVGA /* Trident TVGA 8900: a VGA with extended registers */
} msdos_vid_card;

#define MSDOS_VID_NONE (~0u)     /* "no VRAM byte" in an offset map */
#define MSDOS_VID_VRAM_CAP (1u << 20)

typedef struct msdos_vid_snap {
    int  card;                   /* msdos_vid_card */
    char card_name[40];
    char render[40];             /* 86Box's own render routine, e.g. "svga_render_8bpp_lowres" */

    /* ---- registers */
    uint8_t crtc[256];
    int     crtc_count;          /* how many CRTC registers the card has (18, 25, or more) */
    int     crtc_index;          /* the index register */
    uint8_t seq[64];
    int     seq_count;
    uint8_t gdc[16];
    uint8_t attr[32];
    uint8_t miscout;
    uint8_t mode_reg;            /* MDA 3B8, CGA 3D8 */
    uint8_t color_reg;           /* CGA 3D9 */
    uint8_t status;              /* 3BA / 3DA */
    uint8_t gate[32];            /* PCjr gate array */
    uint8_t memctrl;             /* PCjr 3DF page register */
    uint8_t dac_mask;            /* VGA pel mask */

    /* ---- palette */
    uint8_t  egapal[16];         /* EGA/VGA: attribute register -> colour index */
    uint32_t pal16[16];          /* the 16 colours the card shows (0x00RRGGBB) */
    uint32_t pal256[256];        /* VGA/TVGA: DAC through the pel mask (0x00RRGGBB) */
    uint8_t  dac[256][3];        /* the DAC's 6-bit RGB */
    int      dac_valid;

    /* ---- timing the card decoded from the registers */
    int htotal, hdisp, vtotal, dispend, vsyncstart, split, rowoffset, rowcount;
    int bpp, chain4, lowres, interlace, linedbl;
    int fontbase;                /* CGA: 0 or 256 (thin/thick) */

    /* ---- the beam, wherever the emulation thread stopped */
    int      displine;           /* the line the card is on */
    int      vc, scanline;       /* character row, scan line in it */
    int      linepos;            /* 1 once the active part of this line is drawn */
    int      dispon;             /* display enabled (not vertical blank) */
    int      firstline, lastline;
    uint32_t memaddr, memaddr_latch, cursoraddr;

    /* ---- how CRTC addresses become VRAM bytes (asked of the card's own mapping) */
    uint32_t start_byte;         /* VRAM byte offset of the first displayed unit */
    uint32_t stride;             /* bytes per CRTC address unit (4 on a planar card) */
    uint32_t pitch_units;        /* CRTC address units per displayed row */
    uint32_t vram_mask;          /* display wraps at this mask */
    int      addr_exact;         /* 0: the card's mapping was not linear; stride is a guess */

    /* ---- memory */
    uint32_t vram_size;          /* bytes in vram[] */
    uint8_t *vram;               /* a copy; EGA/VGA/TVGA interleave planes: [(addr<<2)|plane] */
    uint32_t vram_phys;          /* PCjr: where the displayed page is in system RAM */
    uint32_t cpu_base;           /* where the CPU sees this memory (B8000h, A0000h...) */
    uint32_t cpu_size;
    int      planar;             /* vram is 4 interleaved planes */
    uint32_t charseta, charsetb; /* EGA/VGA: byte offsets of the two fonts in vram (plane 2) */

    /* ---- the character ROM of MDA/CGA/PCjr */
    uint8_t font[256 * 16];
    int     font_h;              /* 14 (MDA) or 8 */

    /* ---- the live picture, for the Beam view */
    int       fb_w, fb_h;
    uint32_t *fb;                /* fb_w x fb_h, 0x00RRGGBB; this frame above the beam */
    int       beam_row;          /* the row of fb the beam is on, or -1 */
    uint64_t  frame;             /* the session's frame serial when taken */
} msdos_vid_snap;

void msdos_vid_snap_free(msdos_vid_snap *s);     /* frees the buffers; s may be reused */
int  msdos_vid_snap_copy(msdos_vid_snap *dst, const msdos_vid_snap *src); /* deep copy; 0 or -1 */
void msdos_vid_snap_move(msdos_vid_snap *dst, msdos_vid_snap *src);       /* dst takes src */

/* ---- what the screen is showing, in words ---------------------------------------- */
typedef enum {
    MSDOS_VM_OFF = 0,
    MSDOS_VM_TEXT,               /* character cells */
    MSDOS_VM_GFX_1,              /* 1 bit per pixel */
    MSDOS_VM_GFX_2,              /* CGA-style 2 bits (interleaved banks) */
    MSDOS_VM_GFX_4,              /* PCjr 16 colours, 4 bits packed */
    MSDOS_VM_PLANAR_16,          /* EGA/VGA 4 bit planes */
    MSDOS_VM_PACKED_8,           /* 256 colours: mode 13h, mode X, SVGA 8 bpp */
    MSDOS_VM_UNKNOWN             /* the card is in a mode we do not decode */
} msdos_vid_mode;

typedef struct msdosvid_info {
    int  mode;                   /* msdos_vid_mode */
    char mode_name[64];          /* "80x25 text", "320x200, 4 colours" */
    int  width, height;          /* the picture in its own pixels (text: cols*cw x rows*ch) */
    int  cols, rows, cell_w, cell_h;  /* text modes */
    int  colors;
    int  bank_count;             /* 2 or 4 interleaved banks (CGA, PCjr), else 1 */
    int  blink;                  /* attribute bit 7 blinks (else it is a background intensity) */
    int  cursor_x, cursor_y;     /* text cursor cell, or -1 */
    int  cursor_top, cursor_bottom;
    int  cursor_visible;
    int  split_row;              /* first row of the split screen (line compare), or -1 */
    double dot_mhz, line_khz, frame_hz;   /* from the registers; 0 when unknown */
    int  bios_mode;              /* a guess at the BIOS mode number, or -1 */
    int  line_double;            /* each logical row is drawn this many scan lines tall */
    char line1[160];             /* "VGA  80x25 text  start 0000  offset 80" */
    char line2[160];             /* "line 183/449  active  31.5 kHz 70 Hz" */
} msdosvid_info;

int msdosvid_info_get(const msdos_vid_snap *s, msdosvid_info *out);

/* ---- views, overlays -------------------------------------------------------------- */
typedef enum {
    MSDOSVID_V_DECODED = 0,      /* built from VRAM and the registers as they are now */
    MSDOSVID_V_DISPLAY,          /* the last finished frame: what the monitor shows */
    MSDOSVID_V_BEAM,             /* this frame above the beam, the last one below (dimmed) */
    MSDOSVID_V_VALUES,           /* false colour by the value in VRAM (index, attribute) */
    MSDOSVID_V_PLANES,           /* EGA/VGA: the four bit planes, side by side */
    MSDOSVID_V_FONT,             /* the character set in use as a 16x16 sheet */
    MSDOSVID_V_MEMORY,           /* all of VRAM as a block map */
    MSDOSVID_V_COUNT
} msdosvid_view;

typedef struct {
    unsigned    bit;
    const char *key;             /* stable name: env var, tests */
    const char *label;
    const char *tip;
    int         on_by_default;
} msdosvid_overlay_def;

#define MSDOSVID_O_GRID    (1u << 0)   /* character cells (text) or pixels (zoom >= 4) */
#define MSDOSVID_O_CURSOR  (1u << 1)
#define MSDOSVID_O_BEAM    (1u << 2)
#define MSDOSVID_O_HEAT    (1u << 3)   /* VRAM written lately, fading */
#define MSDOSVID_O_CHANGED (1u << 4)   /* VRAM changed since the previous refresh */
#define MSDOSVID_O_ACTIVE  (1u << 5)   /* outline the active area / border */
#define MSDOSVID_O_SPLIT   (1u << 6)   /* the split-screen line */
#define MSDOSVID_O_BANKS   (1u << 7)   /* tint interleaved banks */
#define MSDOSVID_O_DIGITS  (1u << 8)   /* the value on each pixel/cell, once big enough */
#define MSDOSVID_O_COUNT   9

const msdosvid_overlay_def *msdosvid_overlays(int *count);
const char *msdosvid_view_name(int view);          /* "Decoded" ... */
const char *msdosvid_view_key(int view);           /* "decoded" ... for env vars */
int         msdosvid_view_from_key(const char *key);   /* -1 if unknown */
unsigned    msdosvid_overlay_defaults(void);
unsigned    msdosvid_overlays_from_keys(const char *csv);  /* "heat,grid" */
int         msdosvid_view_available(const msdos_vid_snap *s, int view); /* 1 when it can be drawn */

/* ---- the tracker: what changed, and what has been written lately ---------------- */
#define MSDOSVID_REG_FADE 12      /* refreshes a changed register stays marked */
#define MSDOSVID_HEAT_MAX 255

typedef struct msdosvid_tracker {
    msdos_vid_snap cur, prev;
    int            have_prev;
    uint8_t       *heat;          /* per VRAM byte: 255 when it changed, x0.8 per refresh */
    uint8_t       *changed;       /* per VRAM byte: changed since the previous snapshot */
    uint32_t       heat_n;
    uint8_t        reg_age[4][256];  /* per group: refreshes since it last changed (0 = never/old) */
    uint8_t        pal_age[256];
    uint64_t       refreshes;
    uint64_t       changed_bytes;     /* last refresh */
} msdosvid_tracker;

void msdosvid_tracker_init(msdosvid_tracker *t);
void msdosvid_tracker_free(msdosvid_tracker *t);
/* takes ownership of *fresh's buffers; fresh is left empty */
void msdosvid_tracker_push(msdosvid_tracker *t, msdos_vid_snap *fresh);

/* ---- registers --------------------------------------------------------------------- */
typedef struct msdosvid_reg {
    const char *group;           /* "CRTC", "Sequencer", "Graphics", "Attribute", "Misc",
                                    "Mode", "Gate array" */
    int         index;           /* -1: a port, not an indexed register */
    int         port;            /* the I/O port, or 0 */
    char        name[40];
    unsigned    value;
    int         digits;          /* hex digits to show */
    int         age;             /* refreshes since it changed; < MSDOSVID_REG_FADE is "new" */
    char        meaning[120];    /* decoded bits */
} msdosvid_reg;

/* fills up to max rows; returns the count. t may be NULL (no ages). */
int msdosvid_regs(const msdos_vid_snap *s, const msdosvid_tracker *t, msdosvid_reg *out, int max);

/* ---- palette ----------------------------------------------------------------------- */
typedef struct msdosvid_pal_entry {
    int      index;
    uint32_t rgb;                /* 0x00RRGGBB */
    char     detail[48];         /* "attr 3 -> DAC 3F -> 3F,3F,3F" */
    int      used;               /* pixels/cells using it in the Decoded picture; -1 unknown */
    int      age;                /* refreshes since it changed */
    int      active;             /* 1 when the current mode can reach it */
} msdosvid_pal_entry;
/* The palette the mode draws with: 16 or 256 entries. Returns the count. */
int msdosvid_palette(const msdos_vid_snap *s, const msdosvid_tracker *t, msdosvid_pal_entry *out, int max);

/* ---- memory layout ----------------------------------------------------------------- */
typedef struct msdosvid_region {
    char     name[40];
    uint32_t vram_off, size;     /* inside the VRAM copy */
    uint32_t cpu_addr;           /* where the CPU reaches the start, or MSDOS_VID_NONE */
    uint32_t color;              /* 0x00RRGGBB, as the Memory view paints it */
} msdosvid_region;
int msdosvid_regions(const msdos_vid_snap *s, msdosvid_region *out, int max);

/* ---- drawing ------------------------------------------------------------------------ */
/*
 * What a draw needs. display/display_w/h is the last finished frame (the session's), which
 * the tab keeps itself; it is optional.
 */
typedef struct msdosvid_ctx {
    const msdos_vid_snap   *snap;
    const msdosvid_tracker *track;     /* optional: heat, changed */
    const uint32_t         *display;   /* optional */
    int                     display_w, display_h;
    /*
     * Names this snapshot: bump it whenever snap/track/display change. A non-zero token lets
     * msdosvid_render and msdosvid_hit share one decoded picture (a hover then costs a
     * lookup, not a decode). 0 turns the cache off. UI thread only.
     */
    uint64_t                token;
} msdosvid_ctx;

#define MSDOSVID_MAX_PIXELS (2048 * 2048)

/*
 * Draws `view` at `zoom` (1..16) into dst (0xFFRRGGBB, w*h pixels). With dst NULL it only
 * reports the size. Returns 0, or -1 when that view cannot be drawn for this snapshot
 * (the size is then 0x0 and the tab says why via msdosvid_view_why).
 */
int msdosvid_render(const msdosvid_ctx *c, int view, unsigned overlays, int zoom, uint32_t *dst,
                    size_t cap_pixels, int *w, int *h);
const char *msdosvid_view_why(const msdosvid_ctx *c, int view);   /* NULL when it can be drawn */
/*
 * How many pixels (text: how many cells, counting foreground and background) use each palette
 * index in the Decoded picture. counts[n] is filled; returns 0, or -1 when there is no picture.
 */
int msdosvid_usage(const msdosvid_ctx *c, int *counts, int n);
/* the largest zoom <= max_zoom that fits (avail_w x avail_h), at least 1 */
int msdosvid_fit_zoom(const msdosvid_ctx *c, int view, int avail_w, int avail_h, int max_zoom);

typedef struct msdosvid_pixel {
    int      x, y;               /* in the view's own pixels (zoom divided out) */
    int      col, row;           /* text cell, or -1 */
    uint32_t voff;               /* VRAM byte behind it, or MSDOS_VID_NONE */
    uint32_t cpu_addr;           /* where the CPU reaches that byte, or MSDOS_VID_NONE */
    uint32_t linear;             /* same as cpu_addr, for breakpoints */
    int      value;              /* palette index (graphics) / attribute (text) / -1 */
    int      chr;                /* text: character code, else -1 */
    uint32_t rgb;                /* 0x00RRGGBB */
    int      heat;
    char     text[320];          /* the inspector, ready to show (lines separated by \n) */
} msdosvid_pixel;

/* x, y are positions in the drawn (zoomed) image. 0, or -1 when off the picture. */
int msdosvid_hit(const msdosvid_ctx *c, int view, int zoom, int x, int y, msdosvid_pixel *out);
/* where a pixel of the view lands in the drawn image: the rectangle of its cell */
int msdosvid_cell_rect(const msdosvid_ctx *c, int view, int zoom, int x, int y,
                       int *rx, int *ry, int *rw, int *rh);

/* ---- text for the console ------------------------------------------------------------ */
/* kind: "" or "info", "regs", "pal", "beam", "map". Returns bytes written. */
int msdosvid_text(const msdos_vid_snap *s, const char *kind, char *dst, int dstsz);

/* ---- the tab's section/view names for scripted captures and tests ---------------------- */
int msdosvid_section_from_key(const char *key);   /* "pixel","registers","palette","memory","timing" */
#define MSDOSVID_S_PIXEL    0
#define MSDOSVID_S_REGISTERS 1
#define MSDOSVID_S_PALETTE  2
#define MSDOSVID_S_MEMORY   3
#define MSDOSVID_S_TIMING   4
#define MSDOSVID_S_COUNT    5
const char *msdosvid_section_name(int section);

#ifdef __cplusplus
}
#endif

#endif
