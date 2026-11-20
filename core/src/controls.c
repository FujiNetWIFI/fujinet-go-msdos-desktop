/*
 * controls -- the Controllers window's layout and hit tests, for every
 * toolkit.
 *
 * Two tables, both static and in paint order, in layout units (1 = one key
 * pitch) from the top left; the toolkits draw the art from them and ask the
 * hit tests what a click landed on, so the shapes cannot drift apart between
 * GTK, Qt, AppKit and Win32:
 *
 *  - msdossession_keyboard_skin_layout: the three keyboards (Model F XT for the
 *    5150 and 5160, the PCjr Freeboard, the Model M for the 286 and later): the
 *    keys of msdossession_keyboard_layout placed in their case, with each key's
 *    cap colour, legends, and the case's own parts. Colours and proportions are
 *    DESIGN-msdos.md's, read off the photographs.
 *  - msdossession_controls_layout: two Kraft joysticks side by side, drawn from
 *    above (base, plate, round well, the knob steered by a drag, button 1 on the
 *    knob, button 2 on the side pad, two trim sliders, the KRAFT label, and
 *    buttons 3 and 4 as pills under the base). Which parts are live follows the
 *    game port: see msdossession.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "msdossession.h"

/* ---- the Kraft sticks ---------------------------------------------------------- */

#define CAPTION_H   0.9f
#define STICK_W     10.0f
#define STICK_PITCH 11.0f
#define MAX_CONTROLS 40

typedef struct {
    msdos_control ctl[MAX_CONTROLS];
    int count;
} stick_view;

static stick_view g_view[3];
static int g_view_built[3];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void add_control(stick_view *view, int kind, const char *label, const char *legend,
                        int stick, int target, int enabled,
                        float xpos, float ypos, float width, float height,
                        float cenx, float ceny, float reach)
{
    msdos_control *ctl;
    if (view->count >= MAX_CONTROLS) return;   /* controls_test counts the parts */
    ctl = &view->ctl[view->count++];
    ctl->kind = kind;
    ctl->label = label;
    ctl->legend = legend;
    ctl->stick = stick;
    ctl->target = target;
    ctl->enabled = enabled;
    ctl->x = xpos; ctl->y = ypos; ctl->w = width; ctl->h = height;
    ctl->cenx = cenx; ctl->ceny = ceny; ctl->reach = reach;
}

static void add_stick(stick_view *view, int mode, int stick)
{
    static const char *const k_caption[2] = { "Joystick A", "Joystick B" };
    static const char *const k_button[4] = { "Button 1", "Button 2", "Button 3", "Button 4" };
    static const char *const k_number[4] = { "1", "2", "3", "4" };
    const float orgx = (float)stick * STICK_PITCH, orgy = CAPTION_H;
    const int live = stick == 0 ? mode != MSDOS_PORT_NONE : mode == MSDOS_PORT_TWO;
    /* the second pair of buttons exists only on the 4-button port, on its one stick */
    const int four = stick == 0 && mode == MSDOS_PORT_ONE_4BTN;
    const float knob_x = orgx + 4.6f, knob_y = orgy + 3.8f;

    add_control(view, MSDOS_CTL_CAPTION, k_caption[stick], NULL, stick, -1, live,
                orgx, 0.0f, STICK_W, CAPTION_H, 0, 0, 0);
    add_control(view, MSDOS_CTL_BODY, "Base", NULL, stick, -1, live,
                orgx, orgy, STICK_W, 8.6f, 0, 0, 0);
    add_control(view, MSDOS_CTL_BODY, "Plate", NULL, stick, -1, live,
                orgx + 1.2f, orgy + 0.6f, 6.8f, 6.4f, 0, 0, 0);
    add_control(view, MSDOS_CTL_BODY, "Well", NULL, stick, -1, live,
                orgx + 2.0f, orgy + 1.2f, 5.2f, 5.2f, 0, 0, 0);
    /* Y trim at the left (vertical), X trim under the plate: display only */
    add_control(view, MSDOS_CTL_TRIM, "Trim Y", NULL, stick, -1, live,
                orgx + 0.35f, orgy + 1.4f, 0.5f, 4.0f, 0, 0, 0);
    add_control(view, MSDOS_CTL_TRIM, "Trim X", NULL, stick, -1, live,
                orgx + 2.2f, orgy + 7.35f, 4.8f, 0.5f, 0, 0, 0);
    /* button 2: the pad on the right of the body */
    add_control(view, MSDOS_CTL_BUTTON, k_button[1], k_number[1], stick,
                MSDOS_TARGET(stick, MSDOS_ACT_B2), live,
                orgx + 8.4f, orgy + 1.2f, 1.2f, 2.6f, 0, 0, 0);
    add_control(view, MSDOS_CTL_BODY, "Label", NULL, stick, -1, live,
                orgx + 3.0f, orgy + 7.95f, 4.0f, 0.55f, 0, 0, 0);
    add_control(view, MSDOS_CTL_CAPTION, "KRAFT", NULL, stick, -1, live,
                orgx + 3.0f, orgy + 7.95f, 4.0f, 0.55f, 0, 0, 0);
    /* the steering area (the plate) with the knob at rest in the well */
    add_control(view, MSDOS_CTL_STICK, "Stick", NULL, stick, -1, live,
                orgx + 1.2f, orgy + 0.6f, 6.8f, 6.4f, knob_x, knob_y, 1.9f);
    /* button 1 sits on the knob: round, centre and radius in cx, cy, reach */
    add_control(view, MSDOS_CTL_BUTTON, k_button[0], k_number[0], stick,
                MSDOS_TARGET(stick, MSDOS_ACT_B1), live,
                knob_x - 0.75f, knob_y - 0.75f, 1.5f, 1.5f, knob_x, knob_y, 0.75f);
    add_control(view, MSDOS_CTL_PILL, k_button[2], k_number[2], stick,
                MSDOS_TARGET(stick, MSDOS_ACT_B3), four,
                orgx + 2.3f, orgy + 9.0f, 2.2f, 0.8f, 0, 0, 0);
    add_control(view, MSDOS_CTL_PILL, k_button[3], k_number[3], stick,
                MSDOS_TARGET(stick, MSDOS_ACT_B4), four,
                orgx + 5.5f, orgy + 9.0f, 2.2f, 0.8f, 0, 0, 0);
}

static int view_mode(int mode)
{
    return mode >= MSDOS_PORT_NONE && mode <= MSDOS_PORT_ONE_4BTN ? mode : MSDOS_PORT_TWO;
}

const msdos_control *msdossession_controls_layout(int port_mode, int *count, float *width,
                                                  float *height)
{
    const int mode = view_mode(port_mode);
    stick_view *view;

    pthread_mutex_lock(&g_lock);
    if (!g_view_built[mode]) {
        memset(&g_view[mode], 0, sizeof g_view[mode]);
        add_stick(&g_view[mode], mode, 0);
        add_stick(&g_view[mode], mode, 1);
        g_view_built[mode] = 1;
    }
    view = &g_view[mode];
    pthread_mutex_unlock(&g_lock);
    if (count) *count = view->count;
    if (width) *width = MSDOS_CONTROLS_W;
    if (height) *height = MSDOS_CONTROLS_H;
    return view->ctl;
}

int msdossession_controls_hit(int port_mode, float xpos, float ypos)
{
    int count, idx;
    const msdos_control *ctl = msdossession_controls_layout(port_mode, &count, NULL, NULL);

    /* the topmost: later entries are drawn over earlier ones */
    for (idx = count - 1; idx >= 0; idx--) {
        float grow = 0.0f;
        if (!ctl[idx].enabled) continue;
        if (ctl[idx].kind != MSDOS_CTL_BUTTON && ctl[idx].kind != MSDOS_CTL_PILL
            && ctl[idx].kind != MSDOS_CTL_STICK)
            continue;
        if (ctl[idx].kind == MSDOS_CTL_PILL) grow = 0.25f;    /* a small target */
        if (xpos < ctl[idx].x - grow || xpos >= ctl[idx].x + ctl[idx].w + grow
            || ypos < ctl[idx].y - grow || ypos >= ctl[idx].y + ctl[idx].h + grow)
            continue;
        if (ctl[idx].kind == MSDOS_CTL_BUTTON && ctl[idx].reach > 0.0f) {
            const float delx = xpos - ctl[idx].cenx, dely = ypos - ctl[idx].ceny;
            if (delx * delx + dely * dely > ctl[idx].reach * ctl[idx].reach) continue;
        }
        return idx;
    }
    return -1;
}

void msdossession_stick_from_vector(float delx, float dely, float reach, int *x, int *y)
{
    float fracx = 0.0f, fracy = 0.0f;
    if (reach > 0.0f) {
        fracx = delx / reach;
        fracy = dely / reach;
    }
    if (fracx < -1.0f) fracx = -1.0f; else if (fracx > 1.0f) fracx = 1.0f;
    if (fracy < -1.0f) fracy = -1.0f; else if (fracy > 1.0f) fracy = 1.0f;
    if (x) *x = (int)lroundf(fracx < 0.0f ? fracx * 32768.0f : fracx * 32767.0f);
    if (y) *y = (int)lroundf(fracy < 0.0f ? fracy * 32768.0f : fracy * 32767.0f);
}

/* ---- the keyboard skins --------------------------------------------------------- */

#define MAX_SKIN 160

enum { SKIN_XT = 0, SKIN_PCJR, SKIN_MODEL_M, SKIN_COUNT };

typedef struct {
    msdos_skin_item item[MAX_SKIN];
    int count;
    float w, h;
} skin;

static skin g_skin[SKIN_COUNT];
static int g_skin_built[SKIN_COUNT];

static int skin_for_machine(int machine)
{
    if (machine == MSDOS_M_5150 || machine == MSDOS_M_5160) return SKIN_XT;
    if (machine == MSDOS_M_PCJR) return SKIN_PCJR;
    if (machine >= 0 && machine < MSDOS_M_COUNT) return SKIN_MODEL_M;
    return -1;
}

static void skin_add(skin *board, int kind, const char *label, const char *shifted,
                     const char *fn_legend, int key, int cap_class, int shape,
                     uint32_t fill, uint32_t ink, uint32_t alt,
                     float xpos, float ypos, float width, float height)
{
    msdos_skin_item *item;
    if (board->count >= MAX_SKIN) return;      /* controls_test counts the parts */
    item = &board->item[board->count++];
    item->kind = kind;
    item->label = label;
    item->shifted = shifted;
    item->fn_legend = fn_legend;
    item->key = key;
    item->cap_class = cap_class;
    item->shape = shape;
    item->fill_rgb = fill;
    item->ink_rgb = ink;
    item->alt_rgb = alt;
    item->x = xpos; item->y = ypos; item->w = width; item->h = height;
}

static void skin_body(skin *board, const char *label, const char *text, uint32_t fill,
                      uint32_t ink, uint32_t alt, float xpos, float ypos, float width, float height)
{
    skin_add(board, MSDOS_SKIN_BODY, label, text, NULL, -1, MSDOS_CAP_LIGHT, MSDOS_SHAPE_RECT,
             fill, ink, alt, xpos, ypos, width, height);
}

/* The Model F XT's darker caps (grey-green): Esc, Tab, Ctrl, the Shifts, Alt,
 * the locks, Backspace, F1-F10, PrtSc and the keypad's - and +. */
static int xt_is_modifier(uint32_t hid)
{
    if (hid >= 0x3A && hid <= 0x43) return 1;           /* F1-F10 */
    switch (hid) {
    case 0x29: case 0x2B: case 0x2A: case 0x39: case 0x53: case 0x47:
    case 0x55: case 0x56: case 0x57:
    case 0xE0: case 0xE1: case 0xE2: case 0xE5:
        return 1;
    default:
        return 0;
    }
}

/* The Model M's darker caps: Esc, F1-F12, PrtSc/Scroll/Pause, Tab, Caps,
 * Enter, Backspace, Shift, Ctrl, Alt, the navigation block and arrows, and the
 * keypad's operators (Num, /, *, -, +, Enter). Its digits and . are light. */
static int model_m_is_modifier(uint32_t hid)
{
    if (hid >= 0x3A && hid <= 0x48) return 1;           /* F1-F12, PrtSc, Scroll, Pause */
    if (hid >= 0x49 && hid <= 0x52) return 1;           /* Ins ... arrows */
    if (hid >= 0x53 && hid <= 0x58) return 1;           /* Num, / * - + and keypad Enter */
    switch (hid) {
    case 0x29: case 0x2B: case 0x39: case 0x28: case 0x2A:
    case 0xE0: case 0xE1: case 0xE2: case 0xE4: case 0xE5: case 0xE6:
        return 1;
    default:
        return 0;
    }
}

/* The PCjr's green legends: Fn + a digit is F1..F10, Fn + an arrow moves the
 * cursor a page or to the ends. */
static const char *pcjr_fn_legend(uint32_t hid)
{
    static const char *const fkeys[10] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10" };
    if (hid >= 0x1E && hid <= 0x26) return fkeys[hid - 0x1E];
    if (hid == 0x27) return fkeys[9];
    if (hid == 0x52) return "Home";
    if (hid == 0x50) return "PgUp";
    if (hid == 0x4F) return "PgDn";
    if (hid == 0x51) return "End";
    return NULL;
}

static void build_skin(skin *board, int which, int machine)
{
    const msdos_kbd_key *keys;
    const int count = msdossession_keyboard_layout_for_machine(machine, &keys);
    float maxx = 0.0f, maxy = 0.0f, offx, offy, casew, caseh;
    int idx;

    memset(board, 0, sizeof *board);
    for (idx = 0; idx < count; idx++) {
        if (keys[idx].x + keys[idx].w > maxx) maxx = keys[idx].x + keys[idx].w;
        if (keys[idx].y + keys[idx].h > maxy) maxy = keys[idx].y + keys[idx].h;
    }

    if (which == SKIN_XT) {
        /* a raised rear slab with the IBM badge, over a shallow well of keys */
        offx = 0.6f;
        offy = 2.0f;
        casew = offx + maxx + 0.6f;
        caseh = offy + maxy + 0.6f;
        skin_body(board, "Case", NULL, MSDOS_XT_CASE_RGB, 0, 0, 0.0f, 0.0f, casew, caseh);
        skin_body(board, "Slab", NULL, MSDOS_XT_SLAB_RGB, 0, 0, 0.3f, 0.3f, casew - 0.6f, 1.3f);
        skin_body(board, "Badge", "IBM Personal Computer", MSDOS_XT_BADGE_RGB, MSDOS_XT_INK_RGB, 0,
                  0.6f, 0.55f, 5.6f, 0.8f);
    } else if (which == SKIN_PCJR) {
        offx = 0.7f;
        offy = 0.7f;
        casew = offx + maxx + 0.7f;
        caseh = offy + maxy + 0.7f;
        skin_body(board, "Case", NULL, MSDOS_PCJR_CASE_RGB, 0, 0, 0.0f, 0.0f, casew, caseh);
        skin_body(board, "Plate", NULL, MSDOS_PCJR_PLATE_RGB, 0, 0, 0.3f, 0.3f, casew - 0.6f, caseh - 0.6f);
        skin_body(board, "Logo", "IBM", MSDOS_PCJR_PLATE_RGB, MSDOS_PCJR_LOGO_RGB, 0,
                  offx + 0.2f, offy + 4.05f, 1.6f, 0.8f);
    } else {
        /* the Model M: light case, the IBM badge at the top left, the three
         * lock lights above the keypad, the keys in a darker well */
        offx = 0.7f;
        offy = 1.5f;
        casew = offx + maxx + 0.7f;
        caseh = offy + maxy + 0.7f;
        skin_body(board, "Case", NULL, MSDOS_MM_CASE_RGB, 0, 0, 0.0f, 0.0f, casew, caseh);
        skin_body(board, "Badge", "IBM", MSDOS_MM_BADGE_RGB, 0xF2F2EC, 0, offx, 0.4f, 2.4f, 0.7f);
        skin_body(board, "Well", NULL, MSDOS_MM_WELL_RGB, 0, 0, offx - 0.3f, offy - 0.3f,
                  maxx + 0.6f, maxy + 0.6f);
    }

    for (idx = 0; idx < count; idx++) {
        const uint32_t hid = keys[idx].hid;
        int cls, shape = MSDOS_SHAPE_RECT;
        uint32_t fill, ink;
        const char *fn_legend = NULL;
        uint32_t alt = 0;
        float keyx = offx + keys[idx].x, keyy = offy + keys[idx].y, keyw = keys[idx].w, keyh = keys[idx].h;

        if (which == SKIN_XT) {
            cls = xt_is_modifier(hid) ? MSDOS_CAP_MODIFIER : MSDOS_CAP_LIGHT;
            fill = cls == MSDOS_CAP_MODIFIER ? MSDOS_XT_MODIFIER_RGB : MSDOS_XT_LIGHT_RGB;
            ink = MSDOS_XT_INK_RGB;
        } else if (which == SKIN_PCJR) {
            cls = MSDOS_CAP_CHICLET;
            fill = MSDOS_PCJR_KEY_RGB;
            ink = MSDOS_PCJR_INK_RGB;
            fn_legend = pcjr_fn_legend(hid);
            alt = MSDOS_PCJR_FN_RGB;
            if (hid == 0x28) {
                /* the L-shaped Enter: this is its upper part (the layout's
                 * rectangle); the lower part follows */
                shape = MSDOS_SHAPE_JOIN_BELOW;
                keyh = 1.0f;
            }
        } else {
            cls = model_m_is_modifier(hid) ? MSDOS_CAP_MODIFIER : MSDOS_CAP_LIGHT;
            fill = cls == MSDOS_CAP_MODIFIER ? MSDOS_MM_MODIFIER_RGB : MSDOS_MM_LIGHT_RGB;
            ink = MSDOS_MM_INK_RGB;
        }
        skin_add(board, MSDOS_SKIN_KEYCAP, keys[idx].label, keys[idx].shifted, fn_legend, idx, cls, shape,
                 fill, ink, alt, keyx, keyy, keyw, keyh);
        if (which == SKIN_PCJR && hid == 0x28)
            skin_add(board, MSDOS_SKIN_KEYCAP, keys[idx].label, NULL, NULL, idx, cls,
                     MSDOS_SHAPE_JOIN_ABOVE, fill, ink, 0, offx + 12.8f, offy + 2.0f, 1.6f, 1.0f);
    }

    if (which == SKIN_PCJR) {
        /* the coloured brackets round Fn (green) and Alt (blue) */
        skin_body(board, "FnBracket", NULL, 0, 0, MSDOS_PCJR_FN_RGB,
                  offx + 14.85f, offy - 0.15f, 1.15f, 1.3f);
        skin_body(board, "AltBracket", NULL, 0, 0, MSDOS_PCJR_ALT_RGB,
                  offx + 2.37f, offy + 3.85f, 1.35f, 1.3f);
    } else if (which == SKIN_MODEL_M) {
        static const char *const k_led[3] = { "Num", "Caps", "Scroll" };
        int led;
        for (led = 0; led < 3; led++)
            skin_add(board, MSDOS_SKIN_LED, k_led[led], NULL, NULL, led, MSDOS_CAP_LIGHT,
                     MSDOS_SHAPE_RECT, MSDOS_MM_LED_OFF_RGB, MSDOS_MM_INK_RGB, MSDOS_MM_LED_ON_RGB,
                     offx + 18.8f + (float)led * 1.2f, 0.55f, 0.8f, 0.3f);
    }
    board->w = casew;
    board->h = caseh;
}

const msdos_skin_item *msdossession_keyboard_skin_layout(int machine, int *count, float *width,
                                                         float *height)
{
    const int which = skin_for_machine(machine);
    skin *board;
    if (which < 0) {
        if (count) *count = 0;
        if (width) *width = 0.0f;
        if (height) *height = 0.0f;
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_skin_built[which]) {
        build_skin(&g_skin[which], which, machine);
        g_skin_built[which] = 1;
    }
    board = &g_skin[which];
    pthread_mutex_unlock(&g_lock);
    if (count) *count = board->count;
    if (width) *width = board->w;
    if (height) *height = board->h;
    return board->item;
}

int msdossession_keyboard_skin_hit(int machine, float xpos, float ypos)
{
    int count, idx;
    const msdos_skin_item *item = msdossession_keyboard_skin_layout(machine, &count, NULL, NULL);
    for (idx = count - 1; idx >= 0; idx--)
        if (item[idx].kind == MSDOS_SKIN_KEYCAP && xpos >= item[idx].x && xpos < item[idx].x + item[idx].w
            && ypos >= item[idx].y && ypos < item[idx].y + item[idx].h)
            return idx;
    return -1;
}
