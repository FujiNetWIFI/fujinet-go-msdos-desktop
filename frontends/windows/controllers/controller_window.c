/*
 * The Win32 Controllers panel: the machine's own keyboard on screen (the XT's
 * 83 keys or the Enhanced 101, the keys a desktop keeps for itself --
 * SysRq, Pause, Scroll Lock -- included), the lock lights, the game port's
 * two joysticks with their four buttons, and the gamepads that drive them.
 * The GNOME panel's behaviour, in GDI.
 *
 * Keys and buttons are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the window
 * rather than by BN_CLICKED: a key is HELD while the mouse button is down,
 * exactly like the real one. The mouse is captured on press; letting go
 * anywhere, dragging off the key or losing the capture releases it, so the
 * PC is never left holding one. A joystick is dragged inside its square and
 * springs back to the middle when let go.
 *
 * What the PC sees held -- host keyboard, gamepads and this panel together --
 * is filled with the accent (with the badge's ink on it), so the panel
 * doubles as an input tester for a new gamepad.
 *
 * Keys typed here go to the main window's handler (msdos_main_key): the
 * PC's keys and the app's Ctrl+Shift shortcuts, Ctrl+Shift+K hiding it.
 *
 * FIXED SIZE: laid out for the larger keyboard and four gamepad rows
 * whatever is fitted, so neither the machine nor a gamepad resizes it (a
 * tiling compositor floats a window whose size cannot change). Hidden at
 * start and hidden, not destroyed, when closed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controller_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>
#include <commctrl.h>

#include <string.h>

#include "../app.h"

#define PANEL_CLASS   "FujiNetGoMSDOSControllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; S() scales them to the window's. */
#define MARGIN     12
#define UNIT       34       /* a key's width in pixels */
#define KEY_GAP     3       /* between key caps */
#define KBD_COLS   22.5f    /* the Enhanced keyboard's size in key units: */
#define KBD_ROWS    6.5f    /* the XT's (22 x 5) fits inside it */
#define LIGHTS_H   20
#define STICK      120      /* a joystick's square */
#define STICK_GAP  32
#define TITLE_H    22
#define FIRE_W     28
#define FIRE_H     24
#define NOTE_H     20
#define HEAD_H     24
#define ROW_H      30       /* one gamepad's row */
#define PAD_ROWS    4       /* gamepad rows the window has room for */
#define COMBO_W   150
#define KNOB        10

#define MAX_KEYS  128

#define IDT_TICK   1
#define IDC_PAD0   100      /* + row: its assignment combo */

static HWND g_panel, g_parent;
static msdossession *g_session;
static int g_dpi = 96;

static HWND g_combo[PAD_ROWS];
static int g_npads = -1;
static unsigned g_pad_generation;

/* the keyboard */
static const msdos_kbd_key *g_keys;
static int g_nkeys;
static RECT g_key_rc[MAX_KEYS];
static int g_key_lit[MAX_KEYS];
static int g_built_machine = -1;
static unsigned g_lights = ~0u;

/* the joysticks, drawn and dragged here */
typedef struct {
    RECT area, fire[4], title;
    int x, y;               /* -32768..32767 as shown */
    unsigned held;          /* the buttons this panel holds */
    unsigned lit;           /* the buttons shown lit */
    int dragging;
} stick_ui;
static stick_ui g_stick[MSDOS_JOYSTICKS];
static int g_fitted = -1;

/* what the captured mouse holds: a key, a stick, or a stick's button */
enum { HOLD_NONE, HOLD_KEY, HOLD_STICK, HOLD_FIRE };
static int g_hold = HOLD_NONE, g_hold_index, g_hold_button;

static int g_total_w, g_total_h;
static RECT g_kbd_rc, g_lights_rc, g_note_rc, g_pad_head_rc, g_pad_row_rc[PAD_ROWS];

static HBRUSH g_accent_brush;
static HFONT g_font, g_bold, g_key_font, g_key_small, g_sym_font;
static int g_key_h, g_key_small_h;

static int S(int v)
{
    return msdos_scale(v, g_dpi);
}

static int px(float units)
{
    return (int)(units * (float)S(UNIT) + 0.5f);
}

/* ---- fonts ------------------------------------------------------------------- */

static void free_fonts(void)
{
    HFONT *f[] = { &g_font, &g_bold, &g_key_font, &g_key_small, &g_sym_font };
    size_t i;
    for (i = 0; i < sizeof f / sizeof f[0]; i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static int font_has(HDC dc, HFONT f, const wchar_t *w, int n)
{
    WORD idx[32];
    HGDIOBJ old;
    int i, ok = 1;
    if (!f || n <= 0 || n > 32) return 0;
    old = SelectObject(dc, f);
    if (GetGlyphIndicesW(dc, w, n, idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) ok = 0;
    for (i = 0; ok && i < n; i++)
        if (idx[i] == 0xffff) ok = 0;
    SelectObject(dc, old);
    return ok;
}

static int font_height(HDC dc, HFONT f)
{
    TEXTMETRICA tm;
    HGDIOBJ old = SelectObject(dc, f);
    int h = GetTextMetricsA(dc, &tm) ? tm.tmHeight : 12;
    SelectObject(dc, old);
    return h;
}

/* The message font, bold, and the key caps' (8pt bold, as GNOME's). The
 * arrows on the cursor keys are symbols the UI font may lack: the first
 * symbol face that has them draws them. */
static void make_fonts(void)
{
    static const char *const symbol_faces[] = { "Segoe UI Symbol", "Segoe UI", "DejaVu Sans", "Arial" };
    HDC dc;
    size_t i;

    free_fonts();
    g_font = msdos_make_font(g_dpi, 0, FW_NORMAL, 0, NULL);
    g_bold = msdos_make_font(g_dpi, 0, FW_BOLD, 0, NULL);
    g_key_font = msdos_make_font(g_dpi, 11, FW_BOLD, 0, NULL);
    g_key_small = msdos_make_font(g_dpi, 9, FW_NORMAL, 0, NULL);

    dc = GetDC(NULL);
    for (i = 0; i < sizeof symbol_faces / sizeof symbol_faces[0] && !g_sym_font; i++) {
        NONCLIENTMETRICSA ncm;
        LOGFONTA lf;
        HFONT f;
        memset(&ncm, 0, sizeof ncm);
        ncm.cbSize = sizeof ncm;
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) lf = ncm.lfMessageFont;
        else GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfHeight = -S(13);
        lf.lfWidth = 0;
        lf.lfWeight = FW_BOLD;
        snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", symbol_faces[i]);
        f = CreateFontIndirectA(&lf);
        if (font_has(dc, f, L"\x2190\x2191\x2192\x2193", 4)) g_sym_font = f;
        else if (f) DeleteObject(f);
    }
    g_key_h = font_height(dc, g_key_font);
    g_key_small_h = font_height(dc, g_key_small);
    ReleaseDC(NULL, dc);
}

/* ---- text ------------------------------------------------------------------- */

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC dc, const char *utf8, RECT *rc, UINT fmt)
{
    wchar_t w[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)(sizeof w / sizeof w[0])) <= 0) return 0;
    return DrawTextW(dc, w, -1, rc, fmt | DT_NOPREFIX);
}

static void text_line(HDC dc, const char *utf8, const RECT *rc, UINT fmt)
{
    RECT r = *rc;
    text_out(dc, utf8, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Centred both ways, wrapped ("Scroll Lock" on a two-unit key). */
static void text_block(HDC dc, const char *utf8, const RECT *rc)
{
    RECT m = *rc;
    int h;
    InflateRect(&m, -S(2), 0);
    h = text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    m.left = rc->left + S(2);
    m.right = rc->right - S(2);
    m.top = rc->top + ((rc->bottom - rc->top) - h) / 2;
    if (m.top < rc->top) m.top = rc->top;
    m.bottom = rc->bottom;
    text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK);
}

/* A legend in the key font, or the symbol font when the key font lacks it. */
static void select_legend_font(HDC dc, HFONT base, const char *utf8)
{
    SelectObject(dc, base);
    if (g_sym_font && utf8 && (unsigned char)utf8[0] >= 0x80) {
        wchar_t w[16];
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 16) - 1;
        if (n > 0 && !font_has(dc, base, w, n)) SelectObject(dc, g_sym_font);
    }
}

/* ---- layout ------------------------------------------------------------------- */

static int stick_block_w(void)
{
    const int buttons = 4 * S(FIRE_W) + 3 * S(4);
    return buttons > S(STICK) ? buttons : S(STICK);
}

static int sticks_h(void)
{
    return S(TITLE_H) + S(6) + S(STICK) + S(6) + S(FIRE_H);
}

/* The keyboard's box for this layout: the Enhanced keyboard's at least. */
static void keyboard_extent(float *ew, float *eh)
{
    int i;
    *ew = KBD_COLS;
    *eh = KBD_ROWS;
    for (i = 0; i < g_nkeys; i++) {
        if (g_keys[i].x + g_keys[i].w > *ew) *ew = g_keys[i].x + g_keys[i].w;
        if (g_keys[i].y + g_keys[i].h > *eh) *eh = g_keys[i].y + g_keys[i].h;
    }
}

/* Everything's place: the window is this size for every machine. */
static void layout(void)
{
    const int left = S(MARGIN);
    const int sw = MSDOS_JOYSTICKS * stick_block_w() + (MSDOS_JOYSTICKS - 1) * S(STICK_GAP);
    float ew, eh;
    int kbd_w, kbd_h, content_w, y, i, x0;

    keyboard_extent(&ew, &eh);
    kbd_w = px(ew);
    kbd_h = px(eh);
    content_w = kbd_w > sw ? kbd_w : sw;
    if (content_w < S(480)) content_w = S(480);
    g_total_w = 2 * left + content_w;

    y = S(MARGIN);
    SetRect(&g_kbd_rc, left + (content_w - kbd_w) / 2, y, left + (content_w - kbd_w) / 2 + kbd_w, y + kbd_h);
    y = g_kbd_rc.bottom + S(8);
    SetRect(&g_lights_rc, left, y, left + content_w, y + S(LIGHTS_H));
    y += S(LIGHTS_H) + S(MARGIN);

    x0 = left + (content_w - sw) / 2;
    for (i = 0; i < MSDOS_JOYSTICKS; i++) {
        stick_ui *s = &g_stick[i];
        const int bx = x0 + i * (stick_block_w() + S(STICK_GAP)), bw = stick_block_w();
        const int fw = 4 * S(FIRE_W) + 3 * S(4);
        int b;
        SetRect(&s->title, bx, y, bx + bw, y + S(TITLE_H));
        SetRect(&s->area, bx + (bw - S(STICK)) / 2, y + S(TITLE_H) + S(6),
                bx + (bw - S(STICK)) / 2 + S(STICK), y + S(TITLE_H) + S(6) + S(STICK));
        for (b = 0; b < 4; b++) {
            const int fx = bx + (bw - fw) / 2 + b * (S(FIRE_W) + S(4));
            SetRect(&s->fire[b], fx, s->area.bottom + S(6), fx + S(FIRE_W), s->area.bottom + S(6) + S(FIRE_H));
        }
    }
    y += sticks_h() + S(4);
    SetRect(&g_note_rc, left, y, left + content_w, y + S(NOTE_H));
    y += S(NOTE_H) + S(8);

    SetRect(&g_pad_head_rc, left, y, left + content_w, y + S(HEAD_H));
    y += S(HEAD_H) + S(2);
    for (i = 0; i < PAD_ROWS; i++) {
        SetRect(&g_pad_row_rc[i], left, y, left + content_w, y + S(ROW_H));
        if (g_combo[i])
            MoveWindow(g_combo[i], left + content_w - S(COMBO_W), y + (S(ROW_H) - S(24)) / 2, S(COMBO_W),
                       S(200), FALSE);
        y += S(ROW_H);
    }
    g_total_h = y + S(MARGIN);
}

/* Each key's cap. */
static void place_keys(void)
{
    const int gap = S(KEY_GAP);
    float ew = 0, eh = 0;
    int i, x0, y0;
    for (i = 0; i < g_nkeys; i++) {
        if (g_keys[i].x + g_keys[i].w > ew) ew = g_keys[i].x + g_keys[i].w;
        if (g_keys[i].y + g_keys[i].h > eh) eh = g_keys[i].y + g_keys[i].h;
    }
    x0 = g_kbd_rc.left + ((g_kbd_rc.right - g_kbd_rc.left) - px(ew)) / 2;
    y0 = g_kbd_rc.top + ((g_kbd_rc.bottom - g_kbd_rc.top) - px(eh)) / 2;
    for (i = 0; i < g_nkeys; i++) {
        const msdos_kbd_key *k = &g_keys[i];
        RECT *r = &g_key_rc[i];
        r->left = x0 + px(k->x) + gap / 2;
        r->top = y0 + px(k->y) + gap / 2;
        r->right = x0 + px(k->x + k->w) - (gap - gap / 2);
        r->bottom = y0 + px(k->y + k->h) - (gap - gap / 2);
    }
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wr, cr;
    GetWindowRect(g_panel, &wr);
    GetClientRect(g_panel, &cr);
    if (cr.right == g_total_w && cr.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wr.right - wr.left) - cr.right + g_total_w,
                 (wr.bottom - wr.top) - cr.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *sz)
{
    RECT wr, cr;
    if (hwnd && GetWindowRect(hwnd, &wr) && GetClientRect(hwnd, &cr) && cr.right > 0) {
        sz->cx = (wr.right - wr.left) - cr.right + g_total_w;
        sz->cy = (wr.bottom - wr.top) - cr.bottom + g_total_h;
    } else {
        SetRect(&wr, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wr, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        sz->cx = wr.right - wr.left;
        sz->cy = wr.bottom - wr.top;
    }
}

/* ---- pressing ---------------------------------------------------------------- */

static void send_stick(int i)
{
    stick_ui *s = &g_stick[i];
    msdossession_joystick(g_session, i, s->x, s->y, s->held);
}

/* Lets go of what the mouse holds in this panel (only that: the main
 * window's keys are its own). */
static void mouse_release(void)
{
    const int what = g_hold, i = g_hold_index;
    g_hold = HOLD_NONE;
    if (what == HOLD_KEY) {
        msdossession_keyboard_press(g_session, i, 0);
        if (g_panel && i < g_nkeys) InvalidateRect(g_panel, &g_key_rc[i], FALSE);
    } else if (what == HOLD_STICK) {
        stick_ui *s = &g_stick[i];
        s->dragging = 0;
        s->x = s->y = 0;          /* a joystick springs back to the middle */
        send_stick(i);
        if (g_panel) InvalidateRect(g_panel, &s->area, FALSE);
    } else if (what == HOLD_FIRE) {
        stick_ui *s = &g_stick[i];
        s->held &= ~(1u << g_hold_button);
        send_stick(i);
        if (g_panel) InvalidateRect(g_panel, &s->fire[g_hold_button], FALSE);
    }
}

static int clamp_axis(double v)
{
    return v < -32768.0 ? -32768 : v > 32767.0 ? 32767 : (int)v;
}

static void drag_to(int i, int x, int y)
{
    stick_ui *s = &g_stick[i];
    const double w = s->area.right - s->area.left, h = s->area.bottom - s->area.top;
    s->x = clamp_axis((x - s->area.left - w / 2.0) / (w / 2.0) * 32767.0);
    s->y = clamp_axis((y - s->area.top - h / 2.0) / (h / 2.0) * 32767.0);
    send_stick(i);
    InvalidateRect(g_panel, &s->area, FALSE);
}

/* ---- painting -------------------------------------------------------------- */

static COLORREF mix(COLORREF a, COLORREF b, int pct_a)
{
    return RGB((GetRValue(a) * pct_a + GetRValue(b) * (100 - pct_a)) / 100,
               (GetGValue(a) * pct_a + GetGValue(b) * (100 - pct_a)) / 100,
               (GetBValue(a) * pct_a + GetBValue(b) * (100 - pct_a)) / 100);
}

static void draw_face(HDC dc, const RECT *rc, int lit, int inactive)
{
    RECT r = *rc;
    if (lit) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, MSDOS_INK_RGB);
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(dc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

/* A key cap's legend: the shifted one small above the key's own. */
static void draw_key(HDC dc, int i)
{
    const msdos_kbd_key *k = &g_keys[i];
    const RECT *r = &g_key_rc[i];
    RECT t;
    draw_face(dc, r, g_key_lit[i], 0);
    if (k->shifted) {
        const int top = r->top + ((r->bottom - r->top) - g_key_small_h - g_key_h) / 2;
        SetRect(&t, r->left + 1, top, r->right - 1, top + g_key_small_h);
        select_legend_font(dc, g_key_small, k->shifted);
        text_line(dc, k->shifted, &t, DT_CENTER | DT_END_ELLIPSIS);
        SetRect(&t, r->left + 1, t.bottom, r->right - 1, t.bottom + g_key_h);
        select_legend_font(dc, g_key_font, k->label);
        text_line(dc, k->label, &t, DT_CENTER | DT_END_ELLIPSIS);
        return;
    }
    select_legend_font(dc, g_key_font, k->label);
    text_block(dc, k->label, r);
}

static void draw_stick(HDC dc, int i)
{
    const stick_ui *s = &g_stick[i];
    const RECT *a = &s->area;
    const COLORREF fg = GetSysColor(COLOR_BTNTEXT), bg = GetSysColor(COLOR_BTNFACE);
    const int fitted = g_fitted > 0;
    const int w = a->right - a->left, h = a->bottom - a->top;
    const int cx = a->left + w / 2 + (int)((double)s->x / 32768.0 * (w / 2.0 - S(12)));
    const int cy = a->top + h / 2 + (int)((double)s->y / 32768.0 * (h / 2.0 - S(12)));
    HBRUSH fill = CreateSolidBrush(mix(fg, bg, 15));
    HPEN cross = CreatePen(PS_SOLID, 1, mix(fg, bg, 40)), ring = CreatePen(PS_SOLID, S(1), mix(fg, bg, 80));
    HGDIOBJ oldp, oldb;
    char title[16];
    RECT t = s->title;
    int b;

    snprintf(title, sizeof title, "Joystick %c", 'A' + i);
    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(fitted ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
    text_line(dc, title, &t, DT_CENTER);

    FillRect(dc, a, fill);
    oldp = SelectObject(dc, cross);
    MoveToEx(dc, a->left + w / 2, a->top, NULL);
    LineTo(dc, a->left + w / 2, a->bottom);
    MoveToEx(dc, a->left, a->top + h / 2, NULL);
    LineTo(dc, a->right, a->top + h / 2);
    SelectObject(dc, ring);
    oldb = SelectObject(dc, fitted ? (HGDIOBJ)g_accent_brush : GetStockObject(LTGRAY_BRUSH));
    Ellipse(dc, cx - S(KNOB), cy - S(KNOB), cx + S(KNOB) + 1, cy + S(KNOB) + 1);
    SelectObject(dc, oldb);
    SelectObject(dc, oldp);
    DeleteObject(fill);
    DeleteObject(cross);
    DeleteObject(ring);

    SelectObject(dc, g_font);
    for (b = 0; b < 4; b++) {
        char label[4];
        snprintf(label, sizeof label, "%d", b + 1);
        draw_face(dc, &s->fire[b], fitted && (s->lit & (1u << b)), !fitted);
        text_line(dc, label, &s->fire[b], DT_CENTER);
    }
}

static void paint_panel(HDC dc)
{
    static const char *const lock_names[3] = { "Num Lock", "Caps Lock", "Scroll Lock" };
    RECT client, r;
    char text[160];
    int i, x;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);

    for (i = 0; i < g_nkeys; i++)
        draw_key(dc, i);

    /* the lock lights: a filled dot each, accent when on */
    SelectObject(dc, g_font);
    x = g_lights_rc.left + ((g_lights_rc.right - g_lights_rc.left) - 3 * S(120)) / 2;
    for (i = 0; i < 3; i++) {
        const int on = g_lights != ~0u && (g_lights & (1u << i));
        const int d = S(10), cy = (g_lights_rc.top + g_lights_rc.bottom) / 2;
        HGDIOBJ oldb = SelectObject(dc, on ? (HGDIOBJ)g_accent_brush : GetSysColorBrush(COLOR_BTNFACE));
        HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_GRAYTEXT));
        HGDIOBJ oldp = SelectObject(dc, pen);
        Ellipse(dc, x, cy - d / 2, x + d, cy - d / 2 + d);
        SelectObject(dc, oldp);
        SelectObject(dc, oldb);
        DeleteObject(pen);
        SetRect(&r, x + d + S(6), g_lights_rc.top, x + S(120), g_lights_rc.bottom);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_line(dc, lock_names[i], &r, DT_LEFT);
        x += S(120);
    }

    for (i = 0; i < MSDOS_JOYSTICKS; i++)
        draw_stick(dc, i);
    if (g_fitted == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_line(dc, "No joystick on the game port (Preferences \xe2\x96\xb8 Ports)", &g_note_rc, DT_CENTER);
    }

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    text_line(dc, "Gamepads", &g_pad_head_rc, DT_LEFT);
    SelectObject(dc, g_font);
    if (g_npads <= 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_line(dc, "No gamepads. One plugged in drives Joystick A, the next B.", &g_pad_row_rc[0], DT_LEFT);
    } else {
        if (g_npads > PAD_ROWS) {
            snprintf(text, sizeof text, "%d more not shown", g_npads - PAD_ROWS);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            text_line(dc, text, &g_pad_head_rc, DT_RIGHT);
        }
        for (i = 0; i < g_npads && i < PAD_ROWS; i++) {
            const int eff = msdossession_gamepad_effective_stick(g_session, i);
            char name[128];
            RECT nr = g_pad_row_rc[i], er;
            nr.right -= S(COMBO_W) + S(150);
            name[0] = '\0';
            msdossession_gamepad_name(g_session, i, name, sizeof name);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            text_line(dc, name, &nr, DT_LEFT | DT_END_ELLIPSIS);
            if (eff >= 0) snprintf(text, sizeof text, "drives Joystick %c", 'A' + eff);
            else snprintf(text, sizeof text, "drives nothing");
            SetRect(&er, nr.right + S(8), nr.top, g_pad_row_rc[i].right - S(COMBO_W) - S(10), nr.bottom);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            text_line(dc, text, &er, DT_RIGHT);
        }
    }
}

/* ---- refreshing --------------------------------------------------------------- */

/* The gamepads' rows: a combo each for the ones there are. */
static void refresh_pads(void)
{
    int i;
    g_npads = msdossession_gamepad_count(g_session);
    for (i = 0; i < PAD_ROWS; i++) {
        if (!g_combo[i]) continue;
        if (i < g_npads) {
            const int a = msdossession_gamepad_assignment(g_session, i);
            SendMessageA(g_combo[i], CB_SETCURSEL, (WPARAM)(a < 0 ? 0 : a + 1), 0);
            ShowWindow(g_combo[i], SW_SHOW);
        } else {
            ShowWindow(g_combo[i], SW_HIDE);
        }
    }
    if (g_panel) {
        RECT r;
        UnionRect(&r, &g_pad_head_rc, &g_pad_row_rc[PAD_ROWS - 1]);
        InvalidateRect(g_panel, &r, FALSE);
    }
}

/* The keyboard for this machine, and whether a joystick is fitted. */
static void rebuild(void)
{
    mouse_release();
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_nkeys = msdossession_keyboard_layout(g_session, &g_keys);
    if (g_nkeys > MAX_KEYS) g_nkeys = MAX_KEYS;
    if (g_nkeys < 0 || !g_keys) g_nkeys = 0;
    memset(g_key_lit, 0, sizeof g_key_lit);
    g_built_machine = msdossession_machine(g_session);
    g_fitted = msdossession_joystick_fitted(g_session) != 0;
    layout();
    place_keys();
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
    int i;

    if (msdossession_machine(g_session) != g_built_machine
        || (msdossession_joystick_fitted(g_session) != 0) != g_fitted) {
        rebuild();
        return;
    }
    for (i = 0; i < g_nkeys; i++) {
        const int on = msdossession_keyboard_held(g_session, i) != 0;
        if (on != g_key_lit[i]) {
            g_key_lit[i] = on;
            InvalidateRect(g_panel, &g_key_rc[i], FALSE);
        }
    }
    if (lights != g_lights) {
        g_lights = lights;
        InvalidateRect(g_panel, &g_lights_rc, FALSE);
    }
    for (i = 0; i < MSDOS_JOYSTICKS; i++) {
        stick_ui *s = &g_stick[i];
        int x = 0, y = 0, b;
        unsigned buttons = 0;
        msdossession_joystick_state(g_session, i, &x, &y, &buttons);
        if (!s->dragging && (x != s->x || y != s->y)) {
            s->x = x;
            s->y = y;
            InvalidateRect(g_panel, &s->area, FALSE);
        }
        buttons |= s->held;
        if (buttons != s->lit) {
            for (b = 0; b < 4; b++)
                if ((buttons ^ s->lit) & (1u << b)) InvalidateRect(g_panel, &s->fire[b], FALSE);
            s->lit = buttons;
        }
    }
    gen = msdossession_gamepad_generation(g_session);
    if (gen != g_pad_generation || msdossession_gamepad_count(g_session) != g_npads) {
        g_pad_generation = gen;
        refresh_pads();
    }
}

/* ---- the window ----------------------------------------------------------- */

static void hide_panel(void)
{
    mouse_release();
    if (GetCapture() == g_panel) ReleaseCapture();
    ShowWindow(g_panel, SW_HIDE);
}

static void rescale(int dpi)
{
    int i;
    g_dpi = dpi;
    make_fonts();
    for (i = 0; i < PAD_ROWS; i++)
        if (g_combo[i]) SendMessageA(g_combo[i], WM_SETFONT, (WPARAM)g_font, FALSE);
    rebuild();
}

static void press_at(POINT p)
{
    int i, b;
    for (i = 0; i < g_nkeys; i++)
        if (PtInRect(&g_key_rc[i], p)) {
            g_hold = HOLD_KEY;
            g_hold_index = i;
            msdossession_keyboard_press(g_session, i, 1);
            g_key_lit[i] = 1;
            InvalidateRect(g_panel, &g_key_rc[i], FALSE);
            SetCapture(g_panel);
            return;
        }
    if (g_fitted <= 0) return;
    for (i = 0; i < MSDOS_JOYSTICKS; i++) {
        stick_ui *s = &g_stick[i];
        if (PtInRect(&s->area, p)) {
            g_hold = HOLD_STICK;
            g_hold_index = i;
            s->dragging = 1;
            drag_to(i, p.x, p.y);
            SetCapture(g_panel);
            return;
        }
        for (b = 0; b < 4; b++)
            if (PtInRect(&s->fire[b], p)) {
                g_hold = HOLD_FIRE;
                g_hold_index = i;
                g_hold_button = b;
                s->held |= 1u << b;
                send_stick(i);
                InvalidateRect(g_panel, &s->fire[b], FALSE);
                SetCapture(g_panel);
                return;
            }
    }
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* off-screen first: the held highlights repaint at 20 Hz */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT p;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        SetFocus(hwnd);         /* typing goes to the PC again after a combo */
        mouse_release();
        press_at(p);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_hold != HOLD_NONE) {
            POINT p;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            if (g_hold == HOLD_STICK) {
                drag_to(g_hold_index, p.x, p.y);
            } else {
                /* dragged off what it holds: let go, as a button does */
                const RECT *r = g_hold == HOLD_KEY ? &g_key_rc[g_hold_index]
                                                   : &g_stick[g_hold_index].fire[g_hold_button];
                if (!PtInRect(r, p)) {
                    mouse_release();
                    if (GetCapture() == hwnd) ReleaseCapture();
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        mouse_release();
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lp != hwnd) mouse_release();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) >= IDC_PAD0 && LOWORD(wp) < IDC_PAD0 + PAD_ROWS && HIWORD(wp) == CBN_SELCHANGE) {
            const int row = LOWORD(wp) - IDC_PAD0;
            const int sel = (int)SendMessageA(g_combo[row], CB_GETCURSEL, 0, 0);
            /* 0 = automatic (connection order), 1-2 = that joystick */
            if (sel >= 0) msdossession_gamepad_assign(g_session, row, sel == 0 ? -1 : sel - 1);
            refresh_pads();
            SetFocus(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == IDT_TICK) tick();
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        /* the main window's keys: the PC's, and the app's shortcuts */
        if (msdos_main_key(msg, wp, lp)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter went to the PC: no beep. Alt+Space opens the menu. */
        if (wp != ' ') return 0;
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND other = (HWND)lp;
            mouse_release();
            if (GetCapture() == hwnd) ReleaseCapture();
            /* Leaving the app: nothing typed here stays held (within it,
             * the window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                msdossession_release_keys(g_session);
        }
        break;                      /* DefWindowProc gives the panel the focus */
    case WM_SHOWWINDOW:
        if (wp) SetTimer(hwnd, IDT_TICK, 50, NULL);
        else KillTimer(hwnd, IDT_TICK);
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            SIZE sz;
            window_size(hwnd, &sz);
            mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = mm->ptMaxSize.x = sz.cx;
            mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = mm->ptMaxSize.y = sz.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        rescale(HIWORD(wp));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: the position survives closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release();
        KillTimer(hwnd, IDT_TICK);
        g_panel = NULL;
        memset(g_combo, 0, sizeof g_combo);
        g_built_machine = -1;
        g_npads = -1;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int create_panel(void)
{
    static int registered;
    static const char *const choices[3] = { "Automatic", "Joystick A", "Joystick B" };
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want, pr;
    int i, c;

    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = panel_proc;    /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wc) != 0;
    }
    if (!g_accent_brush) g_accent_brush = CreateSolidBrush(msdos_accent_rgb());
    g_dpi = msdos_window_dpi(g_parent);
    make_fonts();
    g_nkeys = msdossession_keyboard_layout(g_session, &g_keys);
    if (g_nkeys > MAX_KEYS) g_nkeys = MAX_KEYS;
    layout();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it
     * is exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    GetWindowRect(g_parent, &pr);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE, pr.left + S(60),
                              pr.top + S(60), want.right - want.left, want.bottom - want.top, g_parent, NULL,
                              inst, NULL);
    if (!g_panel) return 0;
    for (i = 0; i < PAD_ROWS; i++) {
        g_combo[i] = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0,
                                     S(COMBO_W), S(200), g_panel, (HMENU)(INT_PTR)(IDC_PAD0 + i), inst, NULL);
        for (c = 0; c < 3; c++) SendMessageA(g_combo[i], CB_ADDSTRING, 0, (LPARAM)choices[c]);
    }
    /* placed on another monitor than the main window: its own DPI */
    rescale(msdos_window_dpi(g_panel));
    g_pad_generation = msdossession_gamepad_generation(g_session);
    refresh_pads();
    return 1;
}

static void show_panel(void)
{
    if (msdossession_machine(g_session) != g_built_machine) rebuild();
    g_pad_generation = msdossession_gamepad_generation(g_session);
    refresh_pads();
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
