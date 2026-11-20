/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as the PC game
 * port's joysticks, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices come and go as
 * they appear on the system: SDL posts JOYSTICK_ and GAMEPAD_ add/remove
 * pairs and both are taken, with idempotent open/close, so neither kind of
 * device can be missed. A device SDL has no gamepad mapping for (a plain HID
 * adapter) is opened as a raw joystick instead: its buttons and hats are
 * bindable in the raw bands, just unnamed.
 *
 * A pad is an analog PC joystick. Its left stick (a raw joystick's first two
 * axes) is the stick, proportionally, with a 0.12 radial dead zone; a raw
 * joystick's hat 0 pushes it to the stops. Every button goes through the
 * bindings table (bindings.c): by default the D-pad is the four directions,
 * South, East, West and North are buttons 1-4 and the triggers are buttons 3
 * and 4 too. The ways of holding an input -- a pad button, the hat, the analog
 * stick, the keyboard, the window -- are separate sources in the session, so
 * letting go of one never releases another.
 *
 * Pads take joysticks A and B in connection order unless assigned explicitly
 * (an explicit assignment is remembered by GUID and name: "pad_assign", see
 * pad_assign.h). With one stick on the game port (4 buttons) only A exists;
 * with none, pads are inert. Every connect and disconnect is reported to the
 * session for the frontend's toast. Pad inputs are dispatched on the thread
 * with no pad lock held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS MSDOS_MAX_PADS
#define MAX_HATS 4

#define TRIGGER_ON  0.5f
#define STICK_DEADZONE 0.12f

typedef struct {
    SDL_JoystickID joyid;
    int owner;                /* the session's source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit joystick, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int stick;                /* the joystick it drove on the last poll, or -1 */
    uint8_t sent[4];          /* hat directions held (acts UP DOWN LEFT RIGHT) */
    int axis_x, axis_y;       /* the analog axes last sent for this pad */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[MSDOS_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[MSDOS_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct msdossession *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    msdos_pad_table table;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

/* ---- pure helpers ----------------------------------------------------------- */

void msdos_axes_from_stick(float stick_x, float stick_y, int *axis_x, int *axis_y)
{
    const float dead = STICK_DEADZONE;
    const float raw = sqrtf(stick_x * stick_x + stick_y * stick_y);
    float mag = raw, scale;
    int raw_x, raw_y;

    if (mag < dead) {
        *axis_x = *axis_y = 0;
        return;
    }
    if (mag > 1.0f) mag = 1.0f;
    /* radial dead zone: the deflection past it, rescaled to 0..1 */
    scale = ((mag - dead) / (1.0f - dead)) / raw;
    raw_x = (int)lroundf(stick_x * scale * 32767.0f);
    raw_y = (int)lroundf(stick_y * scale * 32767.0f);
    *axis_x = raw_x < -32768 ? -32768 : raw_x > 32767 ? 32767 : raw_x;
    *axis_y = raw_y < -32768 ? -32768 : raw_y > 32767 ? 32767 : raw_y;
}

unsigned msdos_acts_from_dir(int dir)
{
    unsigned acts = 0;
    if ((dir & MSDOS_DIR_UP) && !(dir & MSDOS_DIR_DOWN)) acts |= 1u << MSDOS_ACT_UP;
    if ((dir & MSDOS_DIR_DOWN) && !(dir & MSDOS_DIR_UP)) acts |= 1u << MSDOS_ACT_DOWN;
    if ((dir & MSDOS_DIR_LEFT) && !(dir & MSDOS_DIR_RIGHT)) acts |= 1u << MSDOS_ACT_LEFT;
    if ((dir & MSDOS_DIR_RIGHT) && !(dir & MSDOS_DIR_LEFT)) acts |= 1u << MSDOS_ACT_RIGHT;
    return acts;
}

int msdos_pad_for_stick(const int *assign, int npads, int stick)
{
    int pos, next = 0;
    for (pos = 0; pos < npads; pos++)
        if (assign[pos] == stick) return pos;
    for (pos = 0; pos < npads; pos++) {
        if (assign[pos] >= 0) continue;   /* explicitly elsewhere */
        if (next == stick) return pos;
        next++;
    }
    return -1;
}

int msdos_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return MSDOS_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return MSDOS_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return MSDOS_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return MSDOS_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return MSDOS_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return MSDOS_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return MSDOS_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return MSDOS_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return MSDOS_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return MSDOS_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return MSDOS_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return MSDOS_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return MSDOS_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return MSDOS_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return MSDOS_PAD_BTN_DPAD_RIGHT;
    default: return MSDOS_PAD_BTN_NONE;
    }
}

int msdos_sdl_button_from_pad(int button)
{
    switch (button) {
    case MSDOS_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case MSDOS_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case MSDOS_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case MSDOS_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case MSDOS_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case MSDOS_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case MSDOS_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case MSDOS_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case MSDOS_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case MSDOS_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case MSDOS_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case MSDOS_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case MSDOS_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case MSDOS_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case MSDOS_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *state, SDL_JoystickID joyid)
{
    int pos;
    for (pos = 0; pos < state->npads; pos++)
        if (state->pads[pos].joyid == joyid) return pos;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *total = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return total ? total : "Gamepad";
}

/* Lets go of everything the pad holds (a pad that goes away, or moves to
 * another stick). Locked by the caller; the session's lock is a leaf. */
static void slot_release_all(gamepad_state *state, pad_slot *slot)
{
    memset(slot->sent, 0, sizeof slot->sent);
    slot->axis_x = slot->axis_y = 0;
    memset(slot->btn_target, 0, sizeof slot->btn_target);
    memset(slot->trigger_down, 0, sizeof slot->trigger_down);
    session_gamepad_release(state->session, slot->owner);
}

static void slot_close(gamepad_state *state, pad_slot *slot)
{
    slot_release_all(state, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_stick_locked(gamepad_state *state, int idx);

/* Gives the pad in slot idx its remembered stick. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads settings' copy in
 * state->table, never writes. */
static void restore_assignment(gamepad_state *state, int idx)
{
    pad_slot *slot = &state->pads[idx];
    int ent = msdos_pad_table_find(&state->table, slot->guidstr, slot->name), kix;
    int exact;

    if (ent < 0 || state->table.entry[ent].port >= MSDOS_JOYSTICKS) return;
    exact = !strcmp(state->table.entry[ent].guid, slot->guidstr);
    for (kix = 0; kix < state->npads; kix++) {
        pad_slot *other = &state->pads[kix];
        if (kix == idx || other->assign < 0 ||
            msdos_pad_table_find(&state->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = state->table.entry[ent].port;
    slot->by_name = !exact;
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *state, SDL_JoystickID joyid)
{
    pthread_mutex_lock(&state->lock);
    if (state->npads < MAX_PADS && slot_for_id(state, joyid) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gamepad = SDL_IsGamepad(joyid) ? SDL_OpenGamepad(joyid) : NULL;
        SDL_Joystick *joy = gamepad ? NULL : SDL_OpenJoystick(joyid);
        if (gamepad || joy) {
            pad_slot *slot = &state->pads[state->npads++];
            int hat, used = 0, kidx;
            for (kidx = 0; kidx < state->npads - 1; kidx++)
                used |= 1 << state->pads[kidx].owner;
            memset(slot, 0, sizeof *slot);
            for (kidx = 0; kidx < MAX_PADS && (used & (1 << kidx)); kidx++) {}
            slot->owner = kidx;
            slot->joyid = joyid;
            slot->handle = gamepad;
            slot->joy = joy;
            slot->assign = -1;
            slot->stick = -1;
            for (hat = 0; hat < MAX_HATS; hat++) slot->last_hat[hat] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(joyid);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(state, state->npads - 1);
            state->generation++;
            fprintf(stderr, "msdos gamepad: %s connected (%s)\n", slot->name,
                    gamepad ? "gamepad" : "raw joystick");
            {
                char msg[128];
                int stick = effective_stick_locked(state, state->npads - 1);
                if (stick >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (joystick %c)", slot->name, 'A' + stick);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                session_gamepad_event(state->session, msg);
            }
        }
    }
    pthread_mutex_unlock(&state->lock);
}

static void close_pad(gamepad_state *state, SDL_JoystickID joyid)
{
    int idx, pos;
    pthread_mutex_lock(&state->lock);
    idx = slot_for_id(state, joyid);
    if (idx >= 0) {
        pad_slot *slot = &state->pads[idx];
        char msg[128];
        fprintf(stderr, "msdos gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        slot_close(state, slot);
        session_gamepad_event(state->session, msg);
        for (pos = idx; pos < state->npads - 1; pos++) state->pads[pos] = state->pads[pos + 1];
        state->npads--;
        state->generation++;
    }
    pthread_mutex_unlock(&state->lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The joystick a slot drives right now, or -1 (more pads than sticks, an
 * explicit assignment to a stick the game port does not have, or no game
 * port). Locked by the caller. */
static int effective_stick_locked(gamepad_state *state, int idx)
{
    int assign[MAX_PADS], pos, stick;
    const int nsticks = session_joystick_count(state->session);
    for (pos = 0; pos < MAX_PADS; pos++) assign[pos] = -1;
    for (pos = 0; pos < state->npads; pos++) assign[pos] = state->pads[pos].assign;
    for (stick = 0; stick < nsticks && stick < MSDOS_JOYSTICKS; stick++)
        if (msdos_pad_for_stick(assign, state->npads, stick) == idx) return stick;
    return -1;
}

static void dispatch_button(gamepad_state *state, SDL_JoystickID which, int button, int down)
{
    int idx, stick, owner = 0, target = -1;
    if (button == MSDOS_PAD_BTN_NONE) return;

    pthread_mutex_lock(&state->lock);
    if (state->cap_active && down) {
        /* Map mode: every button PRESS is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. A release still goes through: it lets
         * go of what an earlier press asserted, so arming Map never leaves a
         * button stuck. */
        if (!state->cap_have) { state->cap_have = 1; state->cap_result = button; }
        pthread_mutex_unlock(&state->lock);
        return;
    }
    idx = slot_for_id(state, which);
    stick = idx >= 0 ? effective_stick_locked(state, idx) : -1;
    if (idx >= 0 && button >= 0 && button < 256) {
        pad_slot *slot = &state->pads[idx];
        owner = slot->owner;
        if (down) {
            if (stick >= 0) {
                target = bindings_target_for_pad(stick, button);
                if (target >= 0) slot->btn_target[button] = (int16_t)(target + 1);
            }
        } else if (slot->btn_target[button]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[button] - 1;
            slot->btn_target[button] = 0;
        }
    }
    pthread_mutex_unlock(&state->lock);
    if (target < 0) return;
    session_gamepad_press(state->session, owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *state, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&state->lock);
    idx = slot_for_id(state, which);
    if (idx < 0 || state->pads[idx].handle) { pthread_mutex_unlock(&state->lock); return; }
    prev_dir = hat_dir_from_mask(state->pads[idx].last_hat[hat]);
    state->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&state->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also steer the stick (poll_one) */
    if (prev_dir >= 0)
        dispatch_button(state, which, MSDOS_PAD_HAT_BASE + hat * MSDOS_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(state, which, MSDOS_PAD_HAT_BASE + hat * MSDOS_PAD_HAT_DIRS + dir, 1);
}

/* ---- the stick and the hat -------------------------------------------------------- */

static float axis_norm(Sint16 level)
{
    return level < 0 ? (float)level / 32768.0f : (float)level / 32767.0f;
}

static void slot_stick(const pad_slot *slot, float *stick_x, float *stick_y)
{
    if (slot->handle) {
        *stick_x = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX));
        *stick_y = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        *stick_x = SDL_GetNumJoystickAxes(slot->joy) > 0 ? axis_norm(SDL_GetJoystickAxis(slot->joy, 0)) : 0.f;
        *stick_y = SDL_GetNumJoystickAxes(slot->joy) > 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, 1)) : 0.f;
    }
}

/* The raw joystick's hat 0 as directions (a mapped gamepad's D-pad is a
 * button, bound like any other). */
static int slot_hat_dir(const pad_slot *slot)
{
    int dir = 0;
    if (!slot->handle && slot->joy && SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 mask = SDL_GetJoystickHat(slot->joy, 0);
        if (mask & SDL_HAT_UP) dir |= MSDOS_DIR_UP;
        if (mask & SDL_HAT_DOWN) dir |= MSDOS_DIR_DOWN;
        if (mask & SDL_HAT_LEFT) dir |= MSDOS_DIR_LEFT;
        if (mask & SDL_HAT_RIGHT) dir |= MSDOS_DIR_RIGHT;
    }
    return dir;
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID joyid;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(gamepad_state *state, pad_slot *slot, int stick,
                     trigger_edge *edges, int *nedges)
{
    unsigned acts;
    int act, axis_x = 0, axis_y = 0;
    float stick_x, stick_y;

    if (stick != slot->stick) {
        /* moved to another stick (or none): release what it held there */
        slot_release_all(state, slot);
        slot->stick = stick;
        if (stick < 0) return;
    }

    slot_stick(slot, &stick_x, &stick_y);
    msdos_axes_from_stick(stick_x, stick_y, &axis_x, &axis_y);
    if (axis_x != slot->axis_x || axis_y != slot->axis_y) {
        slot->axis_x = axis_x;
        slot->axis_y = axis_y;
        session_gamepad_analog(state->session, slot->owner, stick, axis_x, axis_y);
    }

    /* opposite hat directions together cancel: nothing */
    acts = msdos_acts_from_dir(slot_hat_dir(slot));
    for (act = 0; act < 4; act++) {
        const uint8_t want = (uint8_t)((acts >> act) & 1u);
        if (want != slot->sent[act]) {
            slot->sent[act] = want;
            session_gamepad_dir(state->session, slot->owner, stick, act, want);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int trig;
        for (trig = 0; trig < 2; trig++) {
            float level = axis_norm(SDL_GetGamepadAxis(slot->handle,
                trig ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = level >= TRIGGER_ON;
            if (down != slot->trigger_down[trig]) {
                slot->trigger_down[trig] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].joyid = slot->joyid;
                    edges[*nedges].button = trig ? MSDOS_PAD_BTN_RIGHT_TRIGGER : MSDOS_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *state)
{
    int pos, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&state->lock);
    for (pos = 0; pos < state->npads; pos++)
        poll_one(state, &state->pads[pos], effective_stick_locked(state, pos), edges, &nedges);
    pthread_mutex_unlock(&state->lock);
    for (pos = 0; pos < nedges; pos++)
        dispatch_button(state, edges[pos].joyid, edges[pos].button, edges[pos].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *state = arg;
    int count = 0, pos;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (pos = 0; pos < count; pos++) open_pad(state, ids[pos]);
        SDL_free(ids);
    }

    while (!atomic_load(&state->stop_requested)) {
        SDL_Event event;
        int npads;

        pthread_mutex_lock(&state->lock);
        npads = state->npads;
        pthread_mutex_unlock(&state->lock);
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(state, event.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(state, event.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(state, event.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(state, event.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(state, event.gbutton.which,
                                msdos_pad_button_from_sdl(event.gbutton.button),
                                event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, idx;
                pthread_mutex_lock(&state->lock);
                idx = slot_for_id(state, event.jbutton.which);
                gamepad = idx >= 0 && state->pads[idx].handle != NULL;
                pthread_mutex_unlock(&state->lock);
                if (gamepad || event.jbutton.button >= 64) break;
                dispatch_button(state, event.jbutton.which,
                                MSDOS_PAD_BTN_RAW_BASE + event.jbutton.button,
                                event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(state, event.jhat.which, event.jhat.hat, event.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(state);
        /* ~60 Hz while a pad is attached (the machine's own sampling rate).
         * With none there are only hotplug events to notice, which can wait
         * half a second. (Not SDL_WaitEventTimeout: SDL polls inside it at
         * about 1 kHz.) */
        SDL_Delay(npads > 0 ? 16 : 500);
    }

    pthread_mutex_lock(&state->lock);
    for (pos = 0; pos < state->npads; pos++) slot_close(state, &state->pads[pos]);
    state->npads = 0;
    pthread_mutex_unlock(&state->lock);
    return NULL;
}

int gamepad_start(struct msdossession *sess)
{
    gamepad_state *state;
    if (sess->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(sess, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    state = calloc(1, sizeof *state);
    if (!state) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    state->session = sess;
    /* the remembered assignments, once: garbage or no key is an empty table */
    msdos_pad_table_decode(&state->table, msdossession_get_str(sess, "pad_assign", ""), MSDOS_JOYSTICKS);
    pthread_mutex_init(&state->lock, NULL);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&state->thread, &attr, thread_main, state) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(sess, "gamepad thread could not start");
        pthread_mutex_destroy(&state->lock);
        free(state);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    state->running = 1;
    g_state = state;
    sess->gamepad = state;
    return 0;
}

void gamepad_stop(struct msdossession *sess)
{
    gamepad_state *state = sess->gamepad;
    if (!state) return;
    atomic_store(&state->stop_requested, 1);
    pthread_join(state->thread, NULL);
    pthread_mutex_destroy(&state->lock);
    sess->gamepad = NULL;
    if (g_state == state) g_state = NULL;
    free(state);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int msdossession_gamepad_count(msdossession *sess)
{
    gamepad_state *state = sess->gamepad;
    int total;
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    total = state->npads;
    pthread_mutex_unlock(&state->lock);
    return total;
}

int msdossession_gamepad_name(msdossession *sess, int idx, char *dst, int dstsz)
{
    gamepad_state *state = sess->gamepad;
    int total = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    if (idx >= 0 && idx < state->npads)
        total = snprintf(dst, (size_t)dstsz, "%s", slot_name(&state->pads[idx]));
    pthread_mutex_unlock(&state->lock);
    return total;
}

void msdossession_gamepad_assign(msdossession *sess, int idx, int port)
{
    /* Joystick B is not there with one stick on the game port (and nothing is
     * with none): such a choice is Automatic. */
    if (port >= msdossession_joystick_count(sess)) port = -1;
    gamepad_state *state = sess->gamepad;
    char text[MSDOS_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    if (idx >= 0 && idx < state->npads) {
        pad_slot *slot = &state->pads[idx];
        if (port >= 0 && port < MSDOS_JOYSTICKS) {
            slot->assign = port;
            slot->by_name = 0;
            changed = msdos_pad_table_set(&state->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int kix, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (kix = 0; kix < state->npads; kix++)
                if (kix != idx && state->pads[kix].assign >= 0) {
                    kguid[nkeep] = state->pads[kix].guidstr;
                    kname[nkeep++] = state->pads[kix].name;
                }
            changed = msdos_pad_table_remove(&state->table, slot->guidstr, slot->name,
                                           nkeep, kguid, kname);
        }
        if (changed && msdos_pad_table_encode(&state->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&state->lock);
    /* the settings file is written here, on the caller's thread, with the
     * pad lock let go: the pad thread must never wait on a file write */
    if (changed)
        msdossession_set_str(sess, "pad_assign", text);
}

int msdossession_gamepad_assignment(msdossession *sess, int idx)
{
    gamepad_state *state = sess->gamepad;
    int result = -1;
    if (!state) return -1;
    pthread_mutex_lock(&state->lock);
    if (idx >= 0 && idx < state->npads) result = state->pads[idx].assign;
    pthread_mutex_unlock(&state->lock);
    return result;
}

int msdossession_gamepad_effective_stick(msdossession *sess, int idx)
{
    gamepad_state *state = sess->gamepad;
    int result = -1;
    if (!state) return -1;
    pthread_mutex_lock(&state->lock);
    if (idx >= 0 && idx < state->npads) result = effective_stick_locked(state, idx);
    pthread_mutex_unlock(&state->lock);
    return result;
}

unsigned msdossession_gamepad_generation(msdossession *sess)
{
    gamepad_state *state = sess->gamepad;
    unsigned result;
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    result = state->generation;
    pthread_mutex_unlock(&state->lock);
    return result;
}

void msdossession_gamepad_capture_begin(msdossession *sess)
{
    gamepad_state *state = sess->gamepad;
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->cap_active = 1; state->cap_have = 0; state->cap_result = MSDOS_PAD_BTN_NONE;
    pthread_mutex_unlock(&state->lock);
}

void msdossession_gamepad_capture_cancel(msdossession *sess)
{
    gamepad_state *state = sess->gamepad;
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->cap_active = 0; state->cap_have = 0;
    pthread_mutex_unlock(&state->lock);
}

int msdossession_gamepad_capture_poll(msdossession *sess, int *button)
{
    gamepad_state *state = sess->gamepad;
    int got = 0;
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    if (state->cap_active && state->cap_have) {
        if (button) *button = state->cap_result;
        state->cap_active = 0; state->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&state->lock);
    return got;
}
