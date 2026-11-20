/*
 * The Win32 Controllers panel (Ctrl+Shift+K): the current machine's own
 * keyboard on screen (the Model F XT of the 5150 and 5160, the PCjr
 * Freeboard, the Model M of the 286 and later), the two Kraft joysticks of
 * the game port below it, and the gamepads that drive them. Everything is
 * drawn from the session's tables (msdossession_keyboard_skin_layout,
 * msdossession_controls_layout), so every frontend shows the same parts.
 *
 * The art is drawn at twice the size into a memory bitmap and shrunk with
 * HALFTONE (GDI has no anti-aliasing of its own); the text goes on after at
 * the window's own size.
 *
 * MOUSE: a key is HELD while the mouse button is down on it, exactly like the
 * real one (the panel's keys are the PC's own, whatever the bindings say). A
 * press on a Kraft's knob holds button 1, on the side pad button 2, on a
 * pill buttons 3 and 4; a press anywhere else on the plate steers the stick
 * (analog, absolute), and it springs back when let go. The mouse is captured
 * on press; letting go anywhere, losing the capture, hiding or deactivating
 * the window releases whatever the pointer holds.
 *
 * What the PC sees held -- host keyboard, gamepads and this panel together --
 * is lit with the held colour (a 2 px outline and a wash), so the panel
 * doubles as a gamepad tester. The Model M's lock lights follow the real
 * lock state.
 *
 * MAP: Map arms a two-step rebind -- click a control (a button, or a side of
 * the stick's well for a direction), then press a host key OR a gamepad
 * button; Esc steps back. While armed the controls show what drives them, the
 * keyboard is greyed and typing does not reach the PC (Ctrl+Shift shortcuts
 * still work), and rebinding steals the input from whatever held it. Clear
 * takes the picked target's inputs away, Defaults restores all 16. Show
 * bindings shows the same legends outside Map mode.
 *
 * FIXED SIZE: laid out for the largest keyboard, the two sticks and four
 * gamepad rows whatever is fitted, in a caption-only tool window with no
 * thick frame and no maximize box, so neither the machine nor a gamepad
 * resizes it. Hidden at start and hidden, not destroyed, when closed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <math.h>
#include <string.h>

#include "../app.h"
#include "../help/help_window.h"

#define PANEL_CLASS   "FujiNetGoMSDOSControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; SCL() scales them to the window's. */
#define MARGIN      12
#define KEY_UNIT    30      /* a keyboard layout unit in pixels */
#define CTL_UNIT    26      /* a joystick layout unit in pixels */
#define KEY_GAP     1.5     /* shaved off each side of a keycap */
#define BTN_H       26
#define HINT_H      20
#define HEAD_H      24
#define ROW_H       30      /* one gamepad's row */
#define PAD_ROWS     4      /* gamepad rows the window has room for */
#define COMBO_W    150
#define MAX_SKIN   160
#define MIN_CONTENT 560

#define IDT_TICK     1
#define IDT_CAPTURE  2
#define IDC_PAD0   100      /* + row: its assignment combo */

#define RGBX(val) RGB(((val) >> 16) & 0xFF, ((val) >> 8) & 0xFF, (val) & 0xFF)
#define NOFILL    ((COLORREF)0xFFFFFFFFu)
#define PICK_RGB  RGB(0x2A, 0x6F, 0xDB)         /* the target Map picked */

enum { BTN_MAP, BTN_CLEAR, BTN_DEFAULTS, BTN_SHOW, BTN_HELP, BTN_COUNT };
static const int k_btn_width[BTN_COUNT] = { 56, 56, 74, 118, 60 };
static const char *const k_btn_label[BTN_COUNT] = { "Map", "Clear", "Defaults", "Show bindings", "Help" };

static HWND g_panel, g_parent;
static msdossession *g_session;
static int g_dpi = 96;

static HWND g_combo[PAD_ROWS];
static int g_combo_joys[PAD_ROWS];
static int g_npads = -1;
static unsigned g_pad_generation;

/* the keyboard skin of the machine built for */
static const msdos_skin_item *g_skin;
static int g_nskin;
static float g_skin_w, g_skin_h;
static int g_built_machine = -1, g_built_mode = -1;
static unsigned char g_key_lit[MAX_SKIN];       /* by skin item */
static unsigned g_lights = ~0u;

/* the sticks */
static const msdos_control *g_ctl;
static int g_nctl;
static int g_port_mode;
static int g_shown_x[MSDOS_JOYSTICKS], g_shown_y[MSDOS_JOYSTICKS];
static unsigned g_shown_btn[MSDOS_JOYSTICKS];

/* what the captured mouse holds */
enum { HOLD_NONE, HOLD_KEY, HOLD_STICK, HOLD_BUTTON };
static int g_hold = HOLD_NONE, g_hold_key, g_hold_ctl;
static int g_ptr_x[MSDOS_JOYSTICKS], g_ptr_y[MSDOS_JOYSTICKS];
static unsigned g_ptr_btn[MSDOS_JOYSTICKS];

/* Map: -2 off, -1 armed (pick a control), >= 0 the target waiting for an input */
static int g_map_state = -2;
static int g_show_bindings;
static WPARAM g_swallow_vkey;
static char g_hint[300];
static int g_hover_target = -1;
static int g_tracking;

/* geometry, in pixels at the window's dpi */
static int g_total_w, g_total_h;
static double g_kunit, g_kx, g_ky, g_sunit, g_sx, g_sy;
static double g_gap;
static RECT g_art_rc, g_kbd_rc, g_stick_rc, g_btn_rc[BTN_COUNT], g_hint_rc, g_pad_head_rc, g_pad_row_rc[PAD_ROWS];
static int g_pressed_btn = -1;

static HFONT g_font, g_bold, g_key_font, g_key_small, g_key_tiny, g_sym_font, g_logo_font, g_kraft_font;
static int g_key_h, g_key_small_h;

/* the 2x drawing pass: art pixel = (client pixel - origin) * factor */
static int g_art_k = 1, g_art_x, g_art_y;

static int SCL(int val)
{
    return msdos_scale(val, g_dpi);
}

static int art_xpos(double val) { return (int)floor((val - g_art_x) * g_art_k + 0.5); }
static int art_ypos(double val) { return (int)floor((val - g_art_y) * g_art_k + 0.5); }
static int art_len(double val) { return (int)floor(val * g_art_k + 0.5); }

/* ---- fonts and text ------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *fonts[] = { &g_font, &g_bold, &g_key_font, &g_key_small, &g_key_tiny, &g_sym_font, &g_logo_font, &g_kraft_font };
    size_t idx;
    for (idx = 0; idx < sizeof fonts / sizeof fonts[0]; idx++)
        if (*fonts[idx]) { DeleteObject(*fonts[idx]); *fonts[idx] = NULL; }
}

static int font_has(HDC hdc, HFONT font, const wchar_t *wide, int num)
{
    WORD glyphs[32];
    HGDIOBJ old;
    int idx, good = 1;
    if (!font || num <= 0 || num > 32) return 0;
    old = SelectObject(hdc, font);
    if (GetGlyphIndicesW(hdc, wide, num, glyphs, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) good = 0;
    for (idx = 0; good && idx < num; idx++)
        if (glyphs[idx] == 0xffff) good = 0;
    SelectObject(hdc, old);
    return good;
}

static int font_height(HDC hdc, HFONT font)
{
    TEXTMETRICA metrics;
    HGDIOBJ old = SelectObject(hdc, font);
    int height = GetTextMetricsA(hdc, &metrics) ? metrics.tmHeight : 12;
    SelectObject(hdc, old);
    return height;
}

/* The message font, bold, the key caps' (11 px bold, the shifted legends 9 px)
 * and the Kraft's italic. The arrows on the cursor keys are symbols the UI
 * font may lack: the first symbol face that has them draws them. */
static void make_fonts(void)
{
    static const char *const symbol_faces[] = { "Segoe UI Symbol", "Segoe UI", "DejaVu Sans", "Arial" };
    HDC hdc;
    size_t idx;
    LOGFONTA logfont;

    free_fonts();
    g_font = msdos_make_font(g_dpi, 0, FW_NORMAL, 0, NULL);
    g_bold = msdos_make_font(g_dpi, 0, FW_BOLD, 0, NULL);
    g_key_font = msdos_make_font(g_dpi, 11, FW_BOLD, 0, NULL);
    g_key_small = msdos_make_font(g_dpi, 9, FW_NORMAL, 0, NULL);
    g_key_tiny = msdos_make_font(g_dpi, 7, FW_BOLD, 0, NULL);
    g_logo_font = msdos_make_font(g_dpi, 15, FW_BOLD, 0, "Times New Roman");
    if (GetObjectA(g_bold, sizeof logfont, &logfont)) {
        logfont.lfItalic = TRUE;
        logfont.lfHeight = -SCL(13);
        g_kraft_font = CreateFontIndirectA(&logfont);
    }

    hdc = GetDC(NULL);
    for (idx = 0; idx < sizeof symbol_faces / sizeof symbol_faces[0] && !g_sym_font; idx++) {
        NONCLIENTMETRICSA metrics;
        LOGFONTA lfont;
        HFONT font;
        memset(&metrics, 0, sizeof metrics);
        metrics.cbSize = sizeof metrics;
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0)) lfont = metrics.lfMessageFont;
        else GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lfont, &lfont);
        lfont.lfHeight = -SCL(13);
        lfont.lfWidth = 0;
        lfont.lfWeight = FW_BOLD;
        snprintf(lfont.lfFaceName, sizeof lfont.lfFaceName, "%s", symbol_faces[idx]);
        font = CreateFontIndirectA(&lfont);
        if (font_has(hdc, font, L"\x2190\x2191\x2192\x2193", 4)) g_sym_font = font;
        else if (font) DeleteObject(font);
    }
    g_key_h = font_height(hdc, g_key_font);
    g_key_small_h = font_height(hdc, g_key_small);
    ReleaseDC(NULL, hdc);
}

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC hdc, const char *utf8, RECT *rect, UINT fmt)
{
    wchar_t wide[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0) return 0;
    return DrawTextW(hdc, wide, -1, rect, fmt | DT_NOPREFIX);
}

static void text_line(HDC hdc, const char *utf8, const RECT *rect, UINT fmt)
{
    RECT copy = *rect;
    text_out(hdc, utf8, &copy, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Centred both ways, wrapped ("Scroll Lock" on a narrow key). */
static void text_block(HDC hdc, const char *utf8, const RECT *rect)
{
    RECT box = *rect;
    int height;
    InflateRect(&box, -SCL(2), 0);
    height = text_out(hdc, utf8, &box, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    box.left = rect->left + SCL(2);
    box.right = rect->right - SCL(2);
    box.top = rect->top + ((rect->bottom - rect->top) - height) / 2;
    if (box.top < rect->top) box.top = rect->top;
    box.bottom = rect->bottom;
    text_out(hdc, utf8, &box, DT_CENTER | DT_WORDBREAK);
}

/* A legend in the key font, or the symbol font when the key font lacks it. */
static void select_legend_font(HDC hdc, HFONT base, const char *utf8)
{
    SelectObject(hdc, base);
    if (g_sym_font && utf8 && (unsigned char)utf8[0] >= 0x80) {
        wchar_t wide[16];
        const int num = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 16) - 1;
        if (num > 0 && !font_has(hdc, base, wide, num)) SelectObject(hdc, g_sym_font);
    }
}

/* The legend font for a width: the first of the key fonts (as given, largest
 * first) the text fits in, else the smallest. */
static HFONT fit_font(HDC hdc, const char *utf8, int maxw, HFONT first, HFONT second)
{
    wchar_t wide[64];
    SIZE size;
    int num;
    HFONT choices[3];
    int idx;
    choices[0] = first;
    choices[1] = second;
    choices[2] = g_key_tiny;
    num = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 64) - 1;
    if (num <= 0) return first;
    for (idx = 0; idx < 3; idx++) {
        HGDIOBJ old = SelectObject(hdc, choices[idx]);
        const BOOL good = GetTextExtentPoint32W(hdc, wide, num, &size) && size.cx <= maxw;
        SelectObject(hdc, old);
        if (good) return choices[idx];
    }
    return g_key_tiny;
}

/* ---- colours and shapes ---------------------------------------------------------- */

static COLORREF mix_color(COLORREF start, COLORREF finish, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return RGB((int)(GetRValue(start) + (GetRValue(finish) - GetRValue(start)) * frac + 0.5),
               (int)(GetGValue(start) + (GetGValue(finish) - GetGValue(start)) * frac + 0.5),
               (int)(GetBValue(start) + (GetBValue(finish) - GetBValue(start)) * frac + 0.5));
}

/* A dimmed part (an unfitted stick, the keyboard in Map mode): 40% of its
 * own colours over the window's face. */
static COLORREF fade(COLORREF col, int live)
{
    return live ? col : mix_color(col, GetSysColor(COLOR_BTNFACE), 0.6);
}

static COLORREF edge_of(COLORREF col)
{
    return mix_color(col, RGB(0, 0, 0), 0.28);
}

static void ellipse(HDC hdc, double ctrx, double ctry, double radx, double rady, COLORREF fill, COLORREF line,
                    double width)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, width > 0 ? art_len(width) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, width > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    Ellipse(hdc, art_xpos(ctrx - radx), art_ypos(ctry - rady), art_xpos(ctrx + radx), art_ypos(ctry + rady));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void rrect(HDC hdc, double xpos, double ypos, double width, double height, double radius, COLORREF fill,
                  COLORREF line, double linew)
{
    HBRUSH brush = fill == NOFILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, linew > 0 ? art_len(linew) : 0, line);
    HGDIOBJ oldb = SelectObject(hdc, brush);
    HGDIOBJ oldp = SelectObject(hdc, linew > 0 ? (HGDIOBJ)pen : GetStockObject(NULL_PEN));
    RoundRect(hdc, art_xpos(xpos), art_ypos(ypos), art_xpos(xpos + width), art_ypos(ypos + height),
              art_len(radius * 2), art_len(radius * 2));
    SelectObject(hdc, oldp);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
    if (fill != NOFILL) DeleteObject(brush);
}

static void line_seg(HDC hdc, double x0, double y0, double x1, double y1, COLORREF col, double width)
{
    HPEN pen = CreatePen(PS_SOLID, art_len(width) > 0 ? art_len(width) : 1, col);
    HGDIOBJ oldp = SelectObject(hdc, pen);
    MoveToEx(hdc, art_xpos(x0), art_ypos(y0), NULL);
    LineTo(hdc, art_xpos(x1), art_ypos(y1));
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

/* ---- geometry ----------------------------------------------------------------------- */

static void skin_box(const msdos_skin_item *itm, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_kx + itm->x * g_kunit;
    *ypos = g_ky + itm->y * g_kunit;
    *width = itm->w * g_kunit;
    *height = itm->h * g_kunit;
}

static void ctl_box(const msdos_control *ctl, double *xpos, double *ypos, double *width, double *height)
{
    *xpos = g_sx + ctl->x * g_sunit;
    *ypos = g_sy + ctl->y * g_sunit;
    *width = ctl->w * g_sunit;
    *height = ctl->h * g_sunit;
}

static void ctl_rect(const msdos_control *ctl, RECT *rect)
{
    double xpos, ypos, width, height;
    ctl_box(ctl, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos), (int)floor(ypos), (int)ceil(xpos + width), (int)ceil(ypos + height));
}

/* The knob's centre now (merged state of the stick), layout units. */
static void knob_center(const msdos_control *stick, double *cenx, double *ceny)
{
    double fracx = (double)g_shown_x[stick->stick] / 32768.0, fracy = (double)g_shown_y[stick->stick] / 32768.0;
    const double len = sqrt(fracx * fracx + fracy * fracy);
    if (len > 1.0) {
        fracx /= len;
        fracy /= len;
    }
    *cenx = stick->cenx + fracx * stick->reach;
    *ceny = stick->ceny + fracy * stick->reach;
}

static const msdos_control *stick_part(int stick)
{
    int idx;
    for (idx = 0; idx < g_nctl; idx++)
        if (g_ctl[idx].kind == MSDOS_CTL_STICK && g_ctl[idx].stick == stick) return &g_ctl[idx];
    return NULL;
}

/* The Kraft's whole picture, for repaints. */
static void stick_block_rect(int stick, RECT *rect)
{
    const double pitch = 11.0;
    SetRect(rect, (int)(g_sx + stick * pitch * g_sunit) - SCL(2), (int)g_sy - SCL(2),
            (int)(g_sx + (stick * pitch + 10.0) * g_sunit) + SCL(2), (int)(g_sy + MSDOS_CONTROLS_H * g_sunit) + SCL(2));
}

static void item_rect(const msdos_skin_item *itm, RECT *rect)
{
    double xpos, ypos, width, height;
    skin_box(itm, &xpos, &ypos, &width, &height);
    SetRect(rect, (int)floor(xpos) - SCL(2), (int)floor(ypos) - SCL(2), (int)ceil(xpos + width) + SCL(2),
            (int)ceil(ypos + height) + SCL(2));
}

/* The largest keyboard of any machine: the window's keyboard area. */
static void largest_skin(float *width, float *height)
{
    int machine;
    *width = *height = 0;
    for (machine = 0; machine < MSDOS_M_COUNT; machine++) {
        float skinw = 0, skinh = 0;
        int count = 0;
        msdossession_keyboard_skin_layout(machine, &count, &skinw, &skinh);
        if (skinw > *width) *width = skinw;
        if (skinh > *height) *height = skinh;
    }
}

/* Everything's place: the window is this size for every machine and port. */
static void layout(void)
{
    const int left = SCL(MARGIN);
    float maxw, maxh;
    int content_w, kbd_w, kbd_h, stick_w, stick_h, ypos, idx, xpos;

    g_kunit = (double)SCL(KEY_UNIT);
    g_sunit = (double)SCL(CTL_UNIT);
    g_gap = KEY_GAP * g_dpi / 96.0;
    largest_skin(&maxw, &maxh);
    kbd_w = (int)ceil(maxw * g_kunit);
    kbd_h = (int)ceil(maxh * g_kunit);
    stick_w = (int)ceil(MSDOS_CONTROLS_W * g_sunit);
    stick_h = (int)ceil(MSDOS_CONTROLS_H * g_sunit);
    content_w = kbd_w > stick_w ? kbd_w : stick_w;
    if (content_w < SCL(MIN_CONTENT)) content_w = SCL(MIN_CONTENT);
    g_total_w = 2 * left + content_w;

    ypos = SCL(MARGIN);
    SetRect(&g_kbd_rc, left + (content_w - kbd_w) / 2, ypos, left + (content_w - kbd_w) / 2 + kbd_w, ypos + kbd_h);
    g_kx = g_kbd_rc.left + (kbd_w - g_skin_w * g_kunit) / 2.0;
    g_ky = g_kbd_rc.top + (kbd_h - g_skin_h * g_kunit) / 2.0;
    ypos = g_kbd_rc.bottom + SCL(10);
    g_sx = left + (content_w - stick_w) / 2.0;
    g_sy = ypos;
    SetRect(&g_stick_rc, (int)g_sx, ypos, (int)g_sx + stick_w, ypos + stick_h);
    SetRect(&g_art_rc, left, g_kbd_rc.top, left + content_w, g_stick_rc.bottom);
    ypos = g_stick_rc.bottom + SCL(10);

    xpos = left;
    for (idx = 0; idx < BTN_COUNT; idx++) {
        const int width = SCL(k_btn_width[idx]);
        if (idx == BTN_HELP) xpos = left + content_w - width;
        SetRect(&g_btn_rc[idx], xpos, ypos, xpos + width, ypos + SCL(BTN_H));
        xpos += width + SCL(6);
    }
    ypos += SCL(BTN_H) + SCL(4);
    SetRect(&g_hint_rc, left, ypos, left + content_w, ypos + SCL(HINT_H));
    ypos += SCL(HINT_H) + SCL(8);

    SetRect(&g_pad_head_rc, left, ypos, left + content_w, ypos + SCL(HEAD_H));
    ypos += SCL(HEAD_H) + SCL(2);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        SetRect(&g_pad_row_rc[idx], left, ypos, left + content_w, ypos + SCL(ROW_H));
        if (g_combo[idx])
            MoveWindow(g_combo[idx], left + content_w - SCL(COMBO_W), ypos + (SCL(ROW_H) - SCL(24)) / 2,
                       SCL(COMBO_W), SCL(200), FALSE);
        ypos += SCL(ROW_H);
    }
    g_total_h = ypos + SCL(MARGIN);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wnd, client;
    GetWindowRect(g_panel, &wnd);
    GetClientRect(g_panel, &client);
    if (client.right == g_total_w && client.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wnd.right - wnd.left) - client.right + g_total_w,
                 (wnd.bottom - wnd.top) - client.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *dims)
{
    RECT wnd, client;
    if (hwnd && GetWindowRect(hwnd, &wnd) && GetClientRect(hwnd, &client) && client.right > 0) {
        dims->cx = (wnd.right - wnd.left) - client.right + g_total_w;
        dims->cy = (wnd.bottom - wnd.top) - client.bottom + g_total_h;
    } else {
        SetRect(&wnd, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wnd, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        dims->cx = wnd.right - wnd.left;
        dims->cy = wnd.bottom - wnd.top;
    }
}

/* ---- bindings, as text ----------------------------------------------------------------- */

/* A target's legend: its key, else its first gamepad input, else a dash. */
static void binding_label(int target, char *out, int outsz)
{
    const msdos_binding bind = msdossession_binding_get(g_session, target);
    const char *name = NULL;
    out[0] = '\0';
    if (bind.key) name = msdossession_hid_name(bind.key);
    if (!name && MSDOS_PAD_BTN_IS_NAMED(bind.button)) name = msdos_pad_button_name(bind.button);
    else if (!name && bind.button != MSDOS_PAD_BTN_NONE) name = msdos_pad_button_name(bind.button);
    snprintf(out, (size_t)outsz, "%s", name ? name : "\xe2\x80\x94");
    /* the legends are small: "Keypad 5" reads "KP 5", "D-pad Left" reads "Pad Left" */
    if (strncmp(out, "Keypad ", 7) == 0) {
        memmove(out + 3, out + 7, strlen(out + 7) + 1);
        memcpy(out, "KP ", 3);
    } else if (strncmp(out, "D-pad ", 6) == 0) {
        memmove(out + 4, out + 6, strlen(out + 6) + 1);
        memcpy(out, "Pad ", 4);
    }
}

/* The hint for a target: everything that drives it. */
static void target_hint(int target, char *out, int outsz)
{
    const msdos_binding bind = msdossession_binding_get(g_session, target);
    const char *keyname = bind.key ? msdossession_hid_name(bind.key) : NULL;
    char pads[96];
    pads[0] = '\0';
    if (bind.button != MSDOS_PAD_BTN_NONE) {
        snprintf(pads, sizeof pads, "%s", msdos_pad_button_name(bind.button));
        if (bind.button2 != MSDOS_PAD_BTN_NONE) {
            const size_t used = strlen(pads);
            snprintf(pads + used, sizeof pads - used, " or %s", msdos_pad_button_name(bind.button2));
        }
    }
    snprintf(out, (size_t)outsz, "%s: key %s, gamepad %s", msdos_target_name(target),
             keyname ? keyname : "none", pads[0] ? pads : "none");
}

static const char *idle_hint(void)
{
    if (g_port_mode == MSDOS_PORT_NONE)
        return "No joystick on the game port (Preferences \xe2\x96\xb8 Ports). Map still rebinds.";
    return "Drag a stick to steer it, press a button to hold it. Map rebinds the 16 controls.";
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    g_hover_target = -1;
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

/* ---- the art -------------------------------------------------------------------------- */

static int held_item(int key)
{
    return key >= 0 && msdossession_keyboard_held(g_session, key) != 0;
}

static void art_keycap(HDC hdc, const msdos_skin_item *itm, int lit, int live)
{
    double xpos, ypos, width, height;
    double left, top, right, bottom, outl_top, outl_bottom;
    const double radius = (itm->cap_class == MSDOS_CAP_CHICLET ? 0.2 : 0.09) * g_kunit;
    COLORREF fill = RGBX(itm->fill_rgb);
    HRGN clip = NULL;
    const int below = itm->shape == MSDOS_SHAPE_JOIN_BELOW, above = itm->shape == MSDOS_SHAPE_JOIN_ABOVE;

    skin_box(itm, &xpos, &ypos, &width, &height);
    left = xpos + g_gap;
    right = xpos + width - g_gap;
    top = above ? ypos : ypos + g_gap;
    bottom = below ? ypos + height : ypos + height - g_gap;
    if (lit) fill = mix_color(fill, RGBX(MSDOS_HELD_RGB), 0.2);
    fill = fade(fill, live);
    outl_top = top + 1.5;
    outl_bottom = bottom - 1.5;
    if (below || above) {
        /* the L-shaped Enter: each half is drawn past the seam and clipped to
         * its own part, so the two meet with no line between them */
        clip = CreateRectRgn(art_xpos(left) - art_len(2), art_ypos(top), art_xpos(right) + art_len(2),
                             art_ypos(bottom));
        SelectClipRgn(hdc, clip);
        if (below) { bottom += radius * 2; outl_bottom = bottom; }
        if (above) { top -= radius * 2; outl_top = top; }
    }
    rrect(hdc, left, top, right - left, bottom - top, radius, fill, edge_of(fill), 1.0);
    if (lit)
        rrect(hdc, left + 1.5, outl_top, right - left - 3.0, outl_bottom - outl_top, radius, NOFILL,
              fade(RGBX(MSDOS_HELD_RGB), live), 2.0);
    if (clip) {
        SelectClipRgn(hdc, NULL);
        DeleteObject(clip);
    }
}

static void art_keyboard(HDC hdc)
{
    const int live = g_map_state == -2;
    int idx;
    for (idx = 0; idx < g_nskin; idx++) {
        const msdos_skin_item *itm = &g_skin[idx];
        double xpos, ypos, width, height;
        COLORREF fill = fade(RGBX(itm->fill_rgb), live);
        skin_box(itm, &xpos, &ypos, &width, &height);
        if (itm->kind == MSDOS_SKIN_KEYCAP) {
            art_keycap(hdc, itm, g_key_lit[idx], live);
        } else if (itm->kind == MSDOS_SKIN_LED) {
            const int on = g_lights != ~0u && (g_lights & (1u << itm->key));
            const COLORREF light = on ? RGBX(itm->alt_rgb) : fill;
            rrect(hdc, xpos, ypos, width, height, height / 2.0, light, edge_of(light), 1.0);
        } else if (!strcmp(itm->label, "Logo")) {
            continue;                       /* text only */
        } else if (!strcmp(itm->label, "FnBracket") || !strcmp(itm->label, "AltBracket")) {
            rrect(hdc, xpos, ypos, width, height, 0.2 * g_kunit, NOFILL, fade(RGBX(itm->alt_rgb), live), 2.0);
        } else if (!strcmp(itm->label, "Case")) {
            rrect(hdc, xpos, ypos, width, height, 0.35 * g_kunit, fill, edge_of(fill), 1.0);
        } else if (!strcmp(itm->label, "Badge")) {
            rrect(hdc, xpos, ypos, width, height, 0.12 * g_kunit, fill, edge_of(fill), 1.0);
        } else {                            /* Slab, Plate, Well */
            rrect(hdc, xpos, ypos, width, height, 0.2 * g_kunit, fill, edge_of(fill), 1.0);
        }
    }
}

static int ctl_lit(const msdos_control *ctl)
{
    const int bit = (ctl->target % MSDOS_ACT_COUNT) - MSDOS_ACT_B1;
    return ctl->target >= 0 && bit >= 0 && bit < 4 && (g_shown_btn[ctl->stick] & (1u << bit)) != 0;
}

static int ctl_picked(const msdos_control *ctl)
{
    return g_map_state >= 0 && ctl->target == g_map_state;
}

/* A held or picked outline round a button, over its fill. */
static void button_marks(HDC hdc, const msdos_control *ctl, double xpos, double ypos, double width, double height,
                         double radius, int round)
{
    if (ctl_lit(ctl)) {
        if (round) ellipse(hdc, xpos + width / 2, ypos + height / 2, width / 2 - 1.5, height / 2 - 1.5, NOFILL,
                           RGBX(MSDOS_HELD_RGB), 2.0);
        else rrect(hdc, xpos + 1.5, ypos + 1.5, width - 3.0, height - 3.0, radius, NOFILL, RGBX(MSDOS_HELD_RGB), 2.0);
    }
    if (ctl_picked(ctl)) {
        if (round) ellipse(hdc, xpos + width / 2, ypos + height / 2, width / 2 + 2, height / 2 + 2, NOFILL,
                           PICK_RGB, 2.5);
        else rrect(hdc, xpos - 2, ypos - 2, width + 4, height + 4, radius + 2, NOFILL, PICK_RGB, 2.5);
    }
}

static void art_sticks(HDC hdc)
{
    int idx;
    for (idx = 0; idx < g_nctl; idx++) {
        const msdos_control *ctl = &g_ctl[idx];
        const int live = ctl->enabled;
        double xpos, ypos, width, height;
        ctl_box(ctl, &xpos, &ypos, &width, &height);
        switch (ctl->kind) {
        case MSDOS_CTL_BODY:
            if (!strcmp(ctl->label, "Base")) {
                const COLORREF fill = fade(RGBX(MSDOS_KRAFT_BODY_RGB), live);
                rrect(hdc, xpos, ypos, width, height, 0.9 * g_sunit, fill, fade(RGBX(MSDOS_KRAFT_RIM_RGB), live),
                      0.22 * g_sunit);
            } else if (!strcmp(ctl->label, "Plate")) {
                const COLORREF fill = fade(RGBX(MSDOS_KRAFT_PLATE_RGB), live);
                rrect(hdc, xpos, ypos, width, height, 0.4 * g_sunit, fill, edge_of(fill), 1.0);
            } else if (!strcmp(ctl->label, "Well")) {
                const COLORREF fill = fade(RGBX(MSDOS_KRAFT_WELL_RGB), live);
                ellipse(hdc, xpos + width / 2, ypos + height / 2, width / 2, height / 2, fill, edge_of(fill), 1.5);
                ellipse(hdc, xpos + width / 2, ypos + height / 2, width / 2 - 0.2 * g_sunit,
                        height / 2 - 0.2 * g_sunit, NOFILL, mix_color(fill, RGB(0, 0, 0), 0.12), 1.0);
            } else if (!strcmp(ctl->label, "Label")) {
                const COLORREF fill = fade(RGB(0xF6, 0xF6, 0xF1), live);
                rrect(hdc, xpos, ypos, width, height, 0.15 * g_sunit, fill, fade(RGBX(MSDOS_KRAFT_RIM_RGB), live), 1.0);
            }
            break;
        case MSDOS_CTL_TRIM: {
            const COLORREF slot = fade(mix_color(RGBX(MSDOS_KRAFT_TRIM_RGB), RGB(0, 0, 0), 0.35), live);
            const COLORREF tab = fade(mix_color(RGBX(MSDOS_KRAFT_TRIM_RGB), RGB(255, 255, 255), 0.45), live);
            rrect(hdc, xpos, ypos, width, height, 0.12 * g_sunit, slot, edge_of(slot), 1.0);
            if (width > height)
                rrect(hdc, xpos + (width - 0.9 * g_sunit) / 2, ypos - 0.05 * g_sunit, 0.9 * g_sunit,
                      height + 0.1 * g_sunit, 0.1 * g_sunit, tab, edge_of(tab), 1.0);
            else
                rrect(hdc, xpos - 0.05 * g_sunit, ypos + (height - 0.9 * g_sunit) / 2, width + 0.1 * g_sunit,
                      0.9 * g_sunit, 0.1 * g_sunit, tab, edge_of(tab), 1.0);
            break;
        }
        case MSDOS_CTL_STICK: {
            /* the shaft tilts from the rest centre to the knob; the grip sits on it */
            double cenx, ceny;
            const COLORREF grip = fade(RGBX(MSDOS_KRAFT_GRIP_RGB), live);
            knob_center(ctl, &cenx, &ceny);
            if (fabs(cenx - ctl->cenx) > 0.02 || fabs(ceny - ctl->ceny) > 0.02)
                line_seg(hdc, g_sx + ctl->cenx * g_sunit, g_sy + ctl->ceny * g_sunit, g_sx + cenx * g_sunit,
                         g_sy + ceny * g_sunit, fade(RGBX(MSDOS_KRAFT_SHAFT_RGB), live), 0.35 * g_sunit);
            ellipse(hdc, g_sx + cenx * g_sunit, g_sy + ceny * g_sunit, 1.05 * g_sunit, 1.05 * g_sunit, grip,
                    edge_of(grip), 1.5);
            ellipse(hdc, g_sx + cenx * g_sunit, g_sy + ceny * g_sunit, 0.9 * g_sunit, 0.9 * g_sunit, NOFILL,
                    mix_color(grip, RGB(255, 255, 255), 0.5), 1.0);
            break;
        }
        case MSDOS_CTL_BUTTON:
            if (ctl->reach > 0.0f) {
                /* button 1: the cap on top of the knob */
                const msdos_control *stick = stick_part(ctl->stick);
                double cenx = ctl->cenx, ceny = ctl->ceny, radius = ctl->reach * g_sunit;
                COLORREF fill = fade(RGBX(MSDOS_KRAFT_CAP_RGB), live);
                if (stick) knob_center(stick, &cenx, &ceny);
                if (ctl_lit(ctl)) fill = mix_color(fill, RGBX(MSDOS_HELD_RGB), 0.2);
                ellipse(hdc, g_sx + cenx * g_sunit, g_sy + ceny * g_sunit, radius, radius, fill, edge_of(fill), 1.5);
                button_marks(hdc, ctl, g_sx + cenx * g_sunit - radius, g_sy + ceny * g_sunit - radius, 2 * radius,
                             2 * radius, radius, 1);
            } else {
                COLORREF fill = fade(RGBX(MSDOS_KRAFT_SIDE_RGB), live);
                if (ctl_lit(ctl)) fill = mix_color(fill, RGBX(MSDOS_HELD_RGB), 0.2);
                rrect(hdc, xpos, ypos, width, height, 0.3 * g_sunit, fill, edge_of(fill), 1.0);
                button_marks(hdc, ctl, xpos, ypos, width, height, 0.3 * g_sunit, 0);
            }
            break;
        case MSDOS_CTL_PILL: {
            COLORREF fill = fade(RGBX(MSDOS_KRAFT_SIDE_RGB), live);
            if (ctl_lit(ctl)) fill = mix_color(fill, RGBX(MSDOS_HELD_RGB), 0.2);
            rrect(hdc, xpos, ypos, width, height, height / 2.0, fill, edge_of(fill), 1.0);
            button_marks(hdc, ctl, xpos, ypos, width, height, height / 2.0, 0);
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the text over the art ------------------------------------------------------------ */

static void text_keycap(HDC hdc, const msdos_skin_item *itm, int live)
{
    RECT rect, band;
    double xpos, ypos, width, height;
    const int shifted = itm->shifted && *itm->shifted, fnleg = itm->fn_legend && *itm->fn_legend;
    skin_box(itm, &xpos, &ypos, &width, &height);
    SetRect(&rect, (int)(xpos + g_gap), (int)(ypos + g_gap), (int)(xpos + width - g_gap), (int)(ypos + height - g_gap));
    if (itm->shape == MSDOS_SHAPE_JOIN_BELOW) rect.bottom = (int)(ypos + height);
    if (itm->shape == MSDOS_SHAPE_JOIN_ABOVE) rect.top = (int)ypos;
    /* the L-shaped Enter's legend sits in its wider, lower half */
    if (itm->shape == MSDOS_SHAPE_JOIN_BELOW) return;
    SetTextColor(hdc, fade(RGBX(itm->ink_rgb), live));
    if (shifted || fnleg) {
        const int top = rect.top + SCL(2);
        if (shifted) {
            SetRect(&band, rect.left + SCL(3), top, rect.right - SCL(2), top + g_key_small_h);
            select_legend_font(hdc, fit_font(hdc, itm->shifted, band.right - band.left - SCL(1), g_key_small, g_key_tiny),
                               itm->shifted);
            text_line(hdc, itm->shifted, &band, fnleg ? DT_LEFT : DT_CENTER | DT_END_ELLIPSIS);
        }
        if (fnleg) {
            SetRect(&band, rect.left + SCL(2), top, rect.right - SCL(3), top + g_key_small_h);
            SetTextColor(hdc, fade(RGBX(itm->alt_rgb), live));
            select_legend_font(hdc, fit_font(hdc, itm->fn_legend, band.right - band.left, g_key_small, g_key_tiny),
                               itm->fn_legend);
            text_line(hdc, itm->fn_legend, &band, shifted ? DT_RIGHT : DT_CENTER);
            SetTextColor(hdc, fade(RGBX(itm->ink_rgb), live));
        }
        SetRect(&band, rect.left + 1, top + g_key_small_h, rect.right - 1, rect.bottom - 1);
        select_legend_font(hdc, fit_font(hdc, itm->label, band.right - band.left - SCL(2), g_key_font, g_key_small),
                           itm->label);
        text_line(hdc, itm->label, &band, DT_CENTER | DT_END_ELLIPSIS);
        return;
    }
    /* "Num Lock" wraps onto two lines: the widest word decides the size */
    {
        char words[64], *word, *save = NULL;
        HFONT best = g_key_font;
        snprintf(words, sizeof words, "%s", itm->label);
        for (word = strtok_r(words, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
            const HFONT got = fit_font(hdc, word, rect.right - rect.left - SCL(4), g_key_font, g_key_small);
            if (got == g_key_tiny || (got == g_key_small && best == g_key_font)) best = got;
        }
        select_legend_font(hdc, best, itm->label);
    }
    text_block(hdc, itm->label, &rect);
}

static void text_keyboard(HDC hdc)
{
    const int live = g_map_state == -2;
    int idx;
    for (idx = 0; idx < g_nskin; idx++) {
        const msdos_skin_item *itm = &g_skin[idx];
        double xpos, ypos, width, height;
        RECT rect;
        skin_box(itm, &xpos, &ypos, &width, &height);
        SetRect(&rect, (int)xpos, (int)ypos, (int)(xpos + width), (int)(ypos + height));
        if (itm->kind == MSDOS_SKIN_KEYCAP) {
            text_keycap(hdc, itm, live);
        } else if (itm->kind == MSDOS_SKIN_LED) {
            RECT cap = rect;
            cap.top = rect.bottom + SCL(1);
            cap.bottom = cap.top + g_key_small_h;
            InflateRect(&cap, SCL(8), 0);
            SelectObject(hdc, g_key_small);
            SetTextColor(hdc, fade(RGBX(itm->ink_rgb), live));
            text_line(hdc, itm->label, &cap, DT_CENTER);
        } else if (itm->shifted && *itm->shifted) {
            SetTextColor(hdc, fade(RGBX(itm->ink_rgb), live));
            if (!strcmp(itm->label, "Logo")) {
                SelectObject(hdc, g_logo_font);
                text_line(hdc, itm->shifted, &rect, DT_LEFT);
            } else {
                SelectObject(hdc, g_key_small);
                text_line(hdc, itm->shifted, &rect, DT_CENTER);
            }
        }
    }
}

/* Where a direction's legend sits on a stick: in the well round the knob. */
static void direction_rect(const msdos_control *stick, int act, RECT *rect)
{
    const double cenx = g_sx + stick->cenx * g_sunit, ceny = g_sy + stick->ceny * g_sunit;
    const double halfw = 1.0 * g_sunit, halfh = 0.3 * g_sunit;
    double midx = cenx, midy = ceny;
    switch (act) {
    case MSDOS_ACT_UP: midy = ceny - 2.25 * g_sunit; break;
    case MSDOS_ACT_DOWN: midy = ceny + 2.25 * g_sunit; break;
    case MSDOS_ACT_LEFT: midx = cenx - 1.85 * g_sunit; break;
    default: midx = cenx + 1.85 * g_sunit; break;
    }
    SetRect(rect, (int)(midx - halfw), (int)(midy - halfh), (int)(midx + halfw), (int)(midy + halfh));
    if (act == MSDOS_ACT_LEFT || act == MSDOS_ACT_RIGHT) {
        rect->left = (int)(midx - 0.85 * g_sunit);
        rect->right = (int)(midx + 0.85 * g_sunit);
    }
}

static void legend(HDC hdc, int target, const RECT *rect, COLORREF ink, int live)
{
    char label[96];
    RECT box = *rect;
    binding_label(target, label, sizeof label);
    SelectObject(hdc, fit_font(hdc, label, rect->right - rect->left - 2, g_key_small, g_key_tiny));
    SetTextColor(hdc, fade(ink, live));
    text_line(hdc, label, &box, DT_CENTER | DT_END_ELLIPSIS);
    if (g_map_state == target) {
        HBRUSH brush = CreateSolidBrush(PICK_RGB);
        InflateRect(&box, 2, 1);
        FrameRect(hdc, &box, brush);
        InflateRect(&box, -1, -1);
        FrameRect(hdc, &box, brush);
        DeleteObject(brush);
    }
}

static void text_sticks(HDC hdc)
{
    const int show = g_show_bindings || g_map_state != -2;
    int idx;
    for (idx = 0; idx < g_nctl; idx++) {
        const msdos_control *ctl = &g_ctl[idx];
        const int live = ctl->enabled;
        RECT rect;
        char number[140];
        ctl_rect(ctl, &rect);
        switch (ctl->kind) {
        case MSDOS_CTL_CAPTION:
            if (!strcmp(ctl->label, "KRAFT")) {
                SelectObject(hdc, g_kraft_font);
                SetTextColor(hdc, fade(RGBX(MSDOS_KRAFT_LOGO_RGB), live));
            } else {
                SelectObject(hdc, g_bold);
                SetTextColor(hdc, GetSysColor(live ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
            }
            text_line(hdc, ctl->label, &rect, DT_CENTER);
            break;
        case MSDOS_CTL_BUTTON:
        case MSDOS_CTL_PILL: {
            RECT lab = rect;
            const msdos_control *stick = stick_part(ctl->stick);
            SelectObject(hdc, g_key_font);
            if (ctl->kind == MSDOS_CTL_BUTTON && ctl->reach > 0.0f) {
                double cenx = ctl->cenx, ceny = ctl->ceny;
                const double radius = ctl->reach * g_sunit;
                if (stick) knob_center(stick, &cenx, &ceny);
                SetRect(&lab, (int)(g_sx + cenx * g_sunit - radius), (int)(g_sy + ceny * g_sunit - radius),
                        (int)(g_sx + cenx * g_sunit + radius), (int)(g_sy + ceny * g_sunit + radius));
                SetTextColor(hdc, fade(RGB(255, 255, 255), live));
                text_line(hdc, ctl->legend, &lab, DT_CENTER);
                if (show && live && stick) {
                    SetRect(&lab, (int)(g_sx + stick->cenx * g_sunit - 1.1 * g_sunit),
                            (int)(g_sy + stick->ceny * g_sunit + 0.88 * g_sunit),
                            (int)(g_sx + stick->cenx * g_sunit + 1.1 * g_sunit),
                            (int)(g_sy + stick->ceny * g_sunit + 1.46 * g_sunit));
                    legend(hdc, ctl->target, &lab, RGB(0x20, 0x20, 0x20), live);
                }
            } else if (ctl->kind == MSDOS_CTL_BUTTON) {
                SetTextColor(hdc, fade(RGB(0x20, 0x20, 0x20), live));
                text_line(hdc, ctl->legend, &lab, DT_CENTER);
                if (show && live) {
                    SetRect(&lab, rect.left - (int)(0.4 * g_sunit), rect.bottom + SCL(2),
                            rect.right + (int)(0.4 * g_sunit), rect.bottom + SCL(2) + (int)(0.6 * g_sunit));
                    legend(hdc, ctl->target, &lab, RGB(0x20, 0x20, 0x20), live);
                }
            } else {
                char label[96];
                SetTextColor(hdc, fade(RGB(0x20, 0x20, 0x20), live));
                if (show && live) {
                    binding_label(ctl->target, label, sizeof label);
                    snprintf(number, sizeof number, "%s %s", ctl->legend, label);
                } else {
                    snprintf(number, sizeof number, "%s", ctl->legend);
                }
                SelectObject(hdc, show && live ? g_key_small : g_key_font);
                text_line(hdc, number, &lab, DT_CENTER | DT_END_ELLIPSIS);
                if (ctl_picked(ctl)) {
                    HBRUSH brush = CreateSolidBrush(PICK_RGB);
                    FrameRect(hdc, &lab, brush);
                    DeleteObject(brush);
                }
            }
            break;
        }
        case MSDOS_CTL_STICK:
            if (show && live) {
                int act;
                for (act = MSDOS_ACT_UP; act <= MSDOS_ACT_RIGHT; act++) {
                    direction_rect(ctl, act, &rect);
                    legend(hdc, MSDOS_TARGET(ctl->stick, act), &rect, RGB(0x20, 0x20, 0x20), live);
                }
            }
            break;
        default:
            break;
        }
    }
}

/* ---- gamepad rows, buttons, hint ------------------------------------------------------- */

static void draw_face(HDC hdc, const RECT *bounds, int pushed, int armed, int inactive)
{
    RECT rect = *bounds;
    if (armed) {
        HBRUSH brush = CreateSolidBrush(msdos_accent_rgb());
        FillRect(hdc, &rect, brush);
        DeleteObject(brush);
        FrameRect(hdc, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, MSDOS_INK_RGB);
    } else {
        DrawFrameControl(hdc, &rect, DFC_BUTTON,
                         DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0) | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(hdc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static void paint_strip(HDC hdc)
{
    int idx;
    char hover[260];
    SelectObject(hdc, g_font);
    for (idx = 0; idx < BTN_COUNT; idx++) {
        const int armed = (idx == BTN_MAP && g_map_state != -2) || (idx == BTN_SHOW && g_show_bindings);
        const int inactive = idx == BTN_CLEAR && g_map_state < 0;
        draw_face(hdc, &g_btn_rc[idx], idx == g_pressed_btn, armed, inactive);
        text_line(hdc, k_btn_label[idx], &g_btn_rc[idx], DT_CENTER);
    }
    hover[0] = '\0';
    if (g_map_state == -2 && g_hover_target >= 0) target_hint(g_hover_target, hover, sizeof hover);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    text_line(hdc, hover[0] ? hover : g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

static void paint_gamepads(HDC hdc)
{
    char text[160];
    int idx;
    SelectObject(hdc, g_bold);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    text_line(hdc, "Gamepads", &g_pad_head_rc, DT_LEFT);
    SelectObject(hdc, g_font);
    if (!msdossession_get_int(g_session, "enable_gamepad", 1)) {
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, "Gamepads are off (Preferences \xe2\x96\xb8 Ports).", &g_pad_row_rc[0], DT_LEFT);
        return;
    }
    if (g_npads <= 0) {
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, "No gamepads. One plugged in drives Joystick A, the next B.", &g_pad_row_rc[0], DT_LEFT);
        return;
    }
    if (g_npads > PAD_ROWS) {
        snprintf(text, sizeof text, "%d more not shown", g_npads - PAD_ROWS);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, text, &g_pad_head_rc, DT_RIGHT);
    }
    for (idx = 0; idx < g_npads && idx < PAD_ROWS; idx++) {
        const int eff = msdossession_gamepad_effective_stick(g_session, idx);
        char name[128];
        RECT name_rc = g_pad_row_rc[idx], eff_rc;
        name_rc.right -= SCL(COMBO_W) + SCL(150);
        name[0] = '\0';
        msdossession_gamepad_name(g_session, idx, name, sizeof name);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
        text_line(hdc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (eff >= 0) snprintf(text, sizeof text, "drives Joystick %c", 'A' + eff);
        else snprintf(text, sizeof text, "drives nothing");
        SetRect(&eff_rc, name_rc.right + SCL(8), name_rc.top, g_pad_row_rc[idx].right - SCL(COMBO_W) - SCL(10),
                name_rc.bottom);
        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
        text_line(hdc, text, &eff_rc, DT_RIGHT);
    }
}

/* The whole window: the art supersampled, its text, the strips. Only what the
 * update region needs is drawn. */
static void paint_rect(HDC hdc, const RECT *inval)
{
    RECT art;
    const int scale = 2;

    FillRect(hdc, inval, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(hdc, TRANSPARENT);

    IntersectRect(&art, inval, &g_art_rc);
    if (!IsRectEmpty(&art)) {
        const int width = art.right - art.left, height = art.bottom - art.top;
        BITMAPINFO bmi;
        void *bits = NULL;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp;
        memset(&bmi, 0, sizeof bmi);
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = width * scale;
        bmi.bmiHeader.biHeight = -height * scale;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HBRUSH face = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            RECT all = { 0, 0, width * scale, height * scale };
            HGDIOBJ old = SelectObject(mem, bmp);
            FillRect(mem, &all, face);
            DeleteObject(face);
            SetBkMode(mem, TRANSPARENT);
            g_art_k = scale;
            g_art_x = art.left;
            g_art_y = art.top;
            art_keyboard(mem);
            art_sticks(mem);
            g_art_k = 1;
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
            StretchBlt(hdc, art.left, art.top, width, height, mem, 0, 0, width * scale, height * scale, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        }
        DeleteDC(mem);
        text_keyboard(hdc);
        text_sticks(hdc);
    }
    paint_gamepads(hdc);
    paint_strip(hdc);
}

/* ---- invalidation and pressing ------------------------------------------------------------ */

static void invalidate_key(int key)
{
    int idx;
    if (!g_panel) return;
    for (idx = 0; idx < g_nskin; idx++)
        if (g_skin[idx].kind == MSDOS_SKIN_KEYCAP && g_skin[idx].key == key) {
            RECT rect;
            item_rect(&g_skin[idx], &rect);
            InvalidateRect(g_panel, &rect, FALSE);
        }
}

static void invalidate_stick(int stick)
{
    RECT rect;
    if (!g_panel) return;
    stick_block_rect(stick, &rect);
    InvalidateRect(g_panel, &rect, FALSE);
}

static void send_pointer(int stick)
{
    msdossession_pointer_set(g_session, stick, g_ptr_x[stick], g_ptr_y[stick], g_ptr_btn[stick]);
}

/* Lets go of what the mouse holds in this panel (only that: the main
 * window's keys are its own). */
static void mouse_release(void)
{
    const int what = g_hold;
    g_hold = HOLD_NONE;
    if (what == HOLD_KEY) {
        msdossession_keyboard_press(g_session, g_hold_key, 0);
        invalidate_key(g_hold_key);
    } else if (what == HOLD_STICK || what == HOLD_BUTTON) {
        const int stick = g_ctl && g_hold_ctl < g_nctl ? g_ctl[g_hold_ctl].stick : 0;
        if (what == HOLD_STICK) g_ptr_x[stick] = g_ptr_y[stick] = 0;      /* springs back */
        else g_ptr_btn[stick] &= ~(1u << ((g_ctl[g_hold_ctl].target % MSDOS_ACT_COUNT) - MSDOS_ACT_B1));
        send_pointer(stick);
        invalidate_stick(stick);
    }
}

static void drag_to(POINT pnt)
{
    const msdos_control *ctl = &g_ctl[g_hold_ctl];
    int xaxis, yaxis;
    msdossession_stick_from_vector((float)((pnt.x - g_sx) / g_sunit) - ctl->cenx,
                                   (float)((pnt.y - g_sy) / g_sunit) - ctl->ceny, ctl->reach, &xaxis, &yaxis);
    g_ptr_x[ctl->stick] = xaxis;
    g_ptr_y[ctl->stick] = yaxis;
    send_pointer(ctl->stick);
    invalidate_stick(ctl->stick);
}

/* The control under a client point, or -1. */
static int control_at(POINT pnt)
{
    return msdossession_controls_hit(g_port_mode, (float)((pnt.x - g_sx) / g_sunit),
                                     (float)((pnt.y - g_sy) / g_sunit));
}

/* Map: which target a click on this control picks. A stick's direction is the
 * side of the well the click is on. */
static int map_target_for(const msdos_control *ctl, POINT pnt)
{
    if (ctl->kind == MSDOS_CTL_BUTTON || ctl->kind == MSDOS_CTL_PILL) return ctl->target;
    if (ctl->kind == MSDOS_CTL_STICK) {
        const double delx = (pnt.x - g_sx) / g_sunit - ctl->cenx, dely = (pnt.y - g_sy) / g_sunit - ctl->ceny;
        if (fabs(delx) > fabs(dely)) return MSDOS_TARGET(ctl->stick, delx < 0 ? MSDOS_ACT_LEFT : MSDOS_ACT_RIGHT);
        return MSDOS_TARGET(ctl->stick, dely < 0 ? MSDOS_ACT_UP : MSDOS_ACT_DOWN);
    }
    return -1;
}

static void set_map_state(int state)
{
    const int was = g_map_state;
    g_map_state = state;
    if (state < 0 && g_panel) KillTimer(g_panel, IDT_CAPTURE);
    if (state < 0 || was != state) msdossession_gamepad_capture_cancel(g_session);
    if (was == -2 && state != -2) {
        /* entering Map: nothing the pointer or keyboard holds may stay held */
        mouse_release();
        if (g_panel && GetCapture() == g_panel) ReleaseCapture();
        msdossession_release_all(g_session);
    }
    if (state == -2) {
        set_hint(idle_hint());
    } else if (state == -1) {
        set_hint("Click a control to remap (a side of the well for a direction). Esc finishes.");
    } else {
        char msg[200];
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc steps back.", msdos_target_name(state));
        msdossession_gamepad_capture_begin(g_session);
        set_hint(msg);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    if (g_panel) InvalidateRect(g_panel, NULL, FALSE);
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, int target, const char *stolen)
{
    char msg[300];
    if (stolen && *stolen) snprintf(msg, sizeof msg, "%s is now %s (it was %s's)", msdos_target_name(target), what, stolen);
    else snprintf(msg, sizeof msg, "%s is now %s", msdos_target_name(target), what);
    set_hint(msg);
}

static void press_at(POINT pnt)
{
    int idx;
    if (g_map_state == -2) {
        const int item = msdossession_keyboard_skin_hit(g_built_machine, (float)((pnt.x - g_kx) / g_kunit),
                                                       (float)((pnt.y - g_ky) / g_kunit));
        if (item >= 0 && item < g_nskin && g_skin[item].key >= 0) {
            g_hold = HOLD_KEY;
            g_hold_key = g_skin[item].key;
            msdossession_keyboard_press(g_session, g_hold_key, 1);
            invalidate_key(g_hold_key);
            SetCapture(g_panel);
            return;
        }
    }
    idx = control_at(pnt);
    if (idx < 0) return;
    if (g_map_state != -2) {
        const int target = map_target_for(&g_ctl[idx], pnt);
        if (target >= 0) set_map_state(target);
        return;
    }
    g_hold_ctl = idx;
    if (g_ctl[idx].kind == MSDOS_CTL_STICK) {
        g_hold = HOLD_STICK;
        drag_to(pnt);
    } else {
        const int stick = g_ctl[idx].stick;
        g_hold = HOLD_BUTTON;
        g_ptr_btn[stick] |= 1u << ((g_ctl[idx].target % MSDOS_ACT_COUNT) - MSDOS_ACT_B1);
        send_pointer(stick);
        invalidate_stick(stick);
    }
    SetCapture(g_panel);
}

/* ---- refreshing ------------------------------------------------------------------------- */

/* The gamepads' rows: a combo each for the ones there are. A pad can drive
 * Joystick B only when the game port has two sticks; with no game port they
 * are inert. */
static void refresh_pads(void)
{
    const int joys = msdossession_joystick_count(g_session);
    int idx;
    g_npads = msdossession_gamepad_count(g_session);
    for (idx = 0; idx < PAD_ROWS; idx++) {
        if (!g_combo[idx]) continue;
        if (idx < g_npads) {
            const int assigned = msdossession_gamepad_assignment(g_session, idx);
            const int want = assigned < 0 || assigned >= joys ? 0 : assigned + 1;
            if (g_combo_joys[idx] != joys) {
                SendMessageA(g_combo[idx], CB_RESETCONTENT, 0, 0);
                SendMessageA(g_combo[idx], CB_ADDSTRING, 0, (LPARAM)"Automatic");
                if (joys >= 1) SendMessageA(g_combo[idx], CB_ADDSTRING, 0, (LPARAM)"Joystick A");
                if (joys >= 2) SendMessageA(g_combo[idx], CB_ADDSTRING, 0, (LPARAM)"Joystick B");
                g_combo_joys[idx] = joys;
            }
            if ((int)SendMessageA(g_combo[idx], CB_GETCURSEL, 0, 0) != want)
                SendMessageA(g_combo[idx], CB_SETCURSEL, (WPARAM)want, 0);
            EnableWindow(g_combo[idx], joys > 0);
            ShowWindow(g_combo[idx], SW_SHOW);
        } else {
            ShowWindow(g_combo[idx], SW_HIDE);
        }
    }
    if (g_panel) {
        RECT rect;
        UnionRect(&rect, &g_pad_head_rc, &g_pad_row_rc[PAD_ROWS - 1]);
        InvalidateRect(g_panel, &rect, FALSE);
    }
}

/* The keyboard for this machine and the sticks for this game port. */
static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_built_machine = msdossession_machine(g_session);
    g_built_mode = msdossession_game_port_mode(g_session);
    g_port_mode = g_built_mode;
    {
        /* the session's own key table (what keyboard_press and _held index) must be the
         * machine's before the first press */
        const msdos_kbd_key *keys = NULL;
        msdossession_keyboard_layout(g_session, &keys);
    }
    g_skin = msdossession_keyboard_skin_layout(g_built_machine, &g_nskin, &g_skin_w, &g_skin_h);
    if (g_nskin > MAX_SKIN) g_nskin = MAX_SKIN;
    if (!g_skin) g_nskin = 0;
    g_ctl = msdossession_controls_layout(g_port_mode, &g_nctl, NULL, NULL);
    memset(g_key_lit, 0, sizeof g_key_lit);
    memset(g_shown_x, 0, sizeof g_shown_x);
    memset(g_shown_y, 0, sizeof g_shown_y);
    memset(g_shown_btn, 0, sizeof g_shown_btn);
    g_show_bindings = msdossession_controls_show_bindings(g_session) != 0;
    g_lights = ~0u;
    layout();
    if (g_panel) {
        fit_client();
        InvalidateRect(g_panel, NULL, FALSE);
    }
}

/* What the PC sees held, lit; the lock lights; the sticks as the gamepads
 * move them. */
static void tick(void)
{
    const unsigned lights = msdossession_lock_lights(g_session);
    unsigned gen;
    int idx;

    if (msdossession_machine(g_session) != g_built_machine || msdossession_game_port_mode(g_session) != g_built_mode) {
        rebuild();
        refresh_pads();
        return;
    }
    for (idx = 0; idx < g_nskin; idx++) {
        if (g_skin[idx].kind == MSDOS_SKIN_KEYCAP) {
            const unsigned char on = (unsigned char)held_item(g_skin[idx].key);
            if (on != g_key_lit[idx]) {
                RECT rect;
                g_key_lit[idx] = on;
                item_rect(&g_skin[idx], &rect);
                InvalidateRect(g_panel, &rect, FALSE);
            }
        }
    }
    if (lights != g_lights) {
        g_lights = lights;
        InvalidateRect(g_panel, &g_kbd_rc, FALSE);
    }
    for (idx = 0; idx < MSDOS_JOYSTICKS; idx++) {
        int xaxis = 0, yaxis = 0;
        unsigned buttons = 0;
        msdossession_joystick_state(g_session, idx, &xaxis, &yaxis, &buttons);
        buttons &= 15u;
        if (xaxis != g_shown_x[idx] || yaxis != g_shown_y[idx] || buttons != g_shown_btn[idx]) {
            g_shown_x[idx] = xaxis;
            g_shown_y[idx] = yaxis;
            g_shown_btn[idx] = buttons;
            invalidate_stick(idx);
        }
    }
    gen = msdossession_gamepad_generation(g_session);
    if (gen != g_pad_generation || msdossession_gamepad_count(g_session) != g_npads) {
        g_pad_generation = gen;
        refresh_pads();
    }
}

/* ---- the window ------------------------------------------------------------------------- */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    msdossession_release_all(g_session);
    ShowWindow(g_panel, SW_HIDE);
}

static void rescale(int dpi)
{
    int idx;
    g_dpi = dpi;
    make_fonts();
    for (idx = 0; idx < PAD_ROWS; idx++)
        if (g_combo[idx]) SendMessageA(g_combo[idx], WM_SETFONT, (WPARAM)g_font, FALSE);
    rebuild();
}

/* A key with the panel focused: 1 if taken. While Map is armed the keys are
 * Map's (the Ctrl+Shift shortcuts and Alt+F4 excepted); otherwise they are
 * the main window's: the PC's keys and the app's shortcuts. */
static int panel_key(UINT msg, WPARAM vkey, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (g_swallow_vkey && vkey == g_swallow_vkey) {
        if (!down) g_swallow_vkey = 0;
        return 1;
    }
    if (g_map_state == -2) return msdos_main_key(msg, vkey, lparam);

    if (down && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0 && vkey >= 'A' && vkey <= 'Z')
        return msdos_main_key(msg, vkey, lparam);
    if (msg == WM_SYSKEYDOWN && vkey == VK_F4) return 0;
    if (!down || (lparam & (1L << 30))) return 1;
    if (vkey == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vkey = vkey;
        return 1;
    }
    if (g_map_state >= 0 && vkey != VK_LWIN && vkey != VK_RWIN) {
        const int target = g_map_state;
        const uint32_t hid = msdos_main_hid(vkey, lparam);
        char stolen[160];
        if (!hid) return 1;                 /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        msdossession_binding_set_key(g_session, target, hid, stolen, sizeof stolen);
        g_swallow_vkey = vkey;
        set_map_state(-1);                  /* stay armed: remapping several in a row is normal */
        {
            const char *name = msdossession_hid_name(hid);
            bound_hint(name ? name : "that key", target, stolen);
        }
    }
    return 1;
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC hdc = BeginPaint(hwnd, &paint);
        /* off-screen first: the held highlights repaint at 25 Hz */
        const RECT inval = paint.rcPaint;
        const int width = inval.right - inval.left, height = inval.bottom - inval.top;
        if (width > 0 && height > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
            HGDIOBJ oldbmp = SelectObject(mem, bmp);
            HGDIOBJ oldfont = SelectObject(mem, g_font);
            SetViewportOrgEx(mem, -inval.left, -inval.top, NULL);
            paint_rect(mem, &inval);
            SetViewportOrgEx(mem, 0, 0, NULL);
            BitBlt(hdc, inval.left, inval.top, width, height, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldfont);
            SelectObject(mem, oldbmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pnt;
        int idx;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        SetFocus(hwnd);         /* typing goes to the PC again after a combo */
        mouse_release();
        for (idx = 0; idx < BTN_COUNT; idx++) {
            if (!PtInRect(&g_btn_rc[idx], pnt)) continue;
            switch (idx) {
            case BTN_MAP: set_map_state(g_map_state == -2 ? -1 : -2); break;
            case BTN_CLEAR:
                if (g_map_state >= 0) {
                    msdossession_binding_clear(g_session, g_map_state);
                    set_hint("Cleared. Press a key or a gamepad button to bind it again.");
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                break;
            case BTN_DEFAULTS:
                msdossession_bindings_reset(g_session);
                set_hint("Every control is back to its default (no keys; the gamepad buttons as before).");
                InvalidateRect(hwnd, NULL, FALSE);
                break;
            case BTN_SHOW:
                g_show_bindings = !g_show_bindings;
                msdossession_set_controls_show_bindings(g_session, g_show_bindings);
                InvalidateRect(hwnd, NULL, FALSE);
                break;
            default:
                g_pressed_btn = idx;
                InvalidateRect(hwnd, &g_btn_rc[idx], FALSE);
                SetCapture(hwnd);
                break;
            }
            return 0;
        }
        press_at(pnt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        if (g_hold == HOLD_STICK) {
            drag_to(pnt);
        } else if (g_hold == HOLD_KEY) {
            const int item = msdossession_keyboard_skin_hit(g_built_machine, (float)((pnt.x - g_kx) / g_kunit),
                                                           (float)((pnt.y - g_ky) / g_kunit));
            /* dragged off what it holds: let go, as a button does */
            if (item < 0 || item >= g_nskin || g_skin[item].key != g_hold_key) {
                mouse_release();
                if (GetCapture() == hwnd) ReleaseCapture();
            }
        } else if (g_hold == HOLD_BUTTON) {
            if (control_at(pnt) != g_hold_ctl) {
                mouse_release();
                if (GetCapture() == hwnd) ReleaseCapture();
            }
        } else if (g_pressed_btn < 0) {
            /* idle: the hint line says what drives the button under the pointer */
            const int idx = control_at(pnt);
            const int target = idx >= 0 && (g_ctl[idx].kind == MSDOS_CTL_BUTTON || g_ctl[idx].kind == MSDOS_CTL_PILL)
                                   ? g_ctl[idx].target : -1;
            if (!g_tracking) {
                TRACKMOUSEEVENT track;
                memset(&track, 0, sizeof track);
                track.cbSize = sizeof track;
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                g_tracking = TrackMouseEvent(&track) != 0;
            }
            if (target != g_hover_target) {
                g_hover_target = target;
                InvalidateRect(hwnd, &g_hint_rc, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_target >= 0) {
            g_hover_target = -1;
            InvalidateRect(hwnd, &g_hint_rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT pnt;
        pnt.x = GET_X_LPARAM(lparam);
        pnt.y = GET_Y_LPARAM(lparam);
        mouse_release();
        if (g_pressed_btn >= 0) {
            const int btn = g_pressed_btn;
            g_pressed_btn = -1;
            InvalidateRect(hwnd, &g_btn_rc[btn], FALSE);
            if (btn == BTN_HELP && PtInRect(&g_btn_rc[btn], pnt)) msdos_help_show(hwnd, "controllers");
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    }
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lparam != hwnd) {
            mouse_release();
            g_pressed_btn = -1;
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) >= IDC_PAD0 && LOWORD(wparam) < IDC_PAD0 + PAD_ROWS && HIWORD(wparam) == CBN_SELCHANGE) {
            const int row = LOWORD(wparam) - IDC_PAD0;
            const int sel = (int)SendMessageA(g_combo[row], CB_GETCURSEL, 0, 0);
            /* 0 = automatic (connection order), 1-2 = that joystick */
            if (sel >= 0) msdossession_gamepad_assign(g_session, row, sel == 0 ? -1 : sel - 1);
            refresh_pads();
            SetFocus(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wparam == IDT_TICK) {
            tick();
        } else if (wparam == IDT_CAPTURE) {
            int button = MSDOS_PAD_BTN_NONE;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (msdossession_gamepad_capture_poll(g_session, &button)) {
                const int target = g_map_state;
                char stolen[160];
                stolen[0] = '\0';
                msdossession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);          /* stay armed */
                bound_hint(msdos_pad_button_name(button), target, stolen);
            }
        }
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wparam, lparam)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter went to the PC: no beep. Alt+Space opens the menu. */
        if (wparam != ' ') return 0;
        break;

    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            /* the pointer's hold, Map, the bound keys and every PC key: always
             * let go, whichever window gets the focus */
            set_map_state(-2);
            g_swallow_vkey = 0;
            mouse_release();
            g_pressed_btn = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            msdossession_release_all(g_session);
        }
        break;                      /* DefWindowProc gives the panel the focus */
    case WM_SHOWWINDOW:
        if (wparam) SetTimer(hwnd, IDT_TICK, 40, NULL);
        else KillTimer(hwnd, IDT_TICK);
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            SIZE dims;
            window_size(hwnd, &dims);
            limits->ptMinTrackSize.x = limits->ptMaxTrackSize.x = limits->ptMaxSize.x = dims.cx;
            limits->ptMinTrackSize.y = limits->ptMaxTrackSize.y = limits->ptMaxSize.y = dims.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lparam;
        rescale(HIWORD(wparam));
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: the position survives closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        set_map_state(-2);
        mouse_release();
        KillTimer(hwnd, IDT_TICK);
        g_panel = NULL;
        memset(g_combo, 0, sizeof g_combo);
        memset(g_combo_joys, 0, sizeof g_combo_joys);
        g_built_machine = -1;
        g_npads = -1;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want, parent_rc;
    int idx;

    if (!registered) {
        WNDCLASSEXA wcls;
        memset(&wcls, 0, sizeof wcls);
        wcls.cbSize = sizeof wcls;
        wcls.lpfnWndProc = panel_proc;      /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wcls.hInstance = inst;
        wcls.hCursor = LoadCursor(NULL, IDC_ARROW);
        wcls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wcls.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wcls) != 0;
    }
    g_dpi = msdos_window_dpi(g_parent);
    make_fonts();
    g_skin = msdossession_keyboard_skin_layout(msdossession_machine(g_session), &g_nskin, &g_skin_w, &g_skin_h);
    layout();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    GetWindowRect(g_parent, &parent_rc);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE, parent_rc.left + SCL(60),
                              parent_rc.top + SCL(60), want.right - want.left, want.bottom - want.top, g_parent,
                              NULL, inst, NULL);
    if (!g_panel) return 0;
    for (idx = 0; idx < PAD_ROWS; idx++) {
        g_combo[idx] = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0,
                                       SCL(COMBO_W), SCL(200), g_panel, (HMENU)(INT_PTR)(IDC_PAD0 + idx), inst, NULL);
        g_combo_joys[idx] = -1;
    }
    msdos_help_set_context(g_panel, "controllers", 0);
    memset(g_ptr_x, 0, sizeof g_ptr_x);
    memset(g_ptr_y, 0, sizeof g_ptr_y);
    memset(g_ptr_btn, 0, sizeof g_ptr_btn);
    g_map_state = -2;
    g_hover_target = -1;
    /* placed on another monitor than the main window: its own DPI */
    rescale(msdos_window_dpi(g_panel));
    fit_client();
    g_pad_generation = msdossession_gamepad_generation(g_session);
    refresh_pads();
    set_hint(idle_hint());
    return 1;
}

static void show_panel(void)
{
    if (msdossession_machine(g_session) != g_built_machine || msdossession_game_port_mode(g_session) != g_built_mode)
        rebuild();
    g_pad_generation = msdossession_gamepad_generation(g_session);
    refresh_pads();
    set_hint(idle_hint());
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void msdos_controllers_toggle(HWND parent, msdossession *session)
{
    g_session = session;
    if (parent) g_parent = parent;
    if (!g_panel && !create_panel()) return;
    if (IsWindowVisible(g_panel)) hide_panel();
    else show_panel();
}

int msdos_controllers_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void msdos_controllers_refresh(void)
{
    if (!g_panel) return;
    if (IsWindowVisible(g_panel)) {
        rebuild();
        refresh_pads();
    } else {
        g_built_machine = -1;       /* rebuilt when next shown */
    }
}

int msdos_controllers_pretranslate(MSG *msg)
{
    /* A combo box has the focus after a choice: its keys are the panel's
     * (they type on the PC), not the combo's. */
    if (!g_panel || !msg->hwnd || msg->hwnd == g_panel || !IsChild(g_panel, msg->hwnd)) return 0;
    switch (msg->message) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        /* a dropped-down list keeps its own keys */
        if (SendMessageA(GetAncestor(msg->hwnd, GA_PARENT) == g_panel ? msg->hwnd : GetParent(msg->hwnd),
                         CB_GETDROPPEDSTATE, 0, 0))
            return 0;
        SendMessageA(g_panel, msg->message, msg->wParam, msg->lParam);
        return 1;
    default:
        return 0;
    }
}
