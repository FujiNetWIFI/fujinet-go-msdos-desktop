/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as the PC game
 * port's joysticks, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices come and go as
 * they appear on the system; SDL posts JOYSTICK_ and GAMEPAD_ add/remove
 * pairs and both are taken, with idempotent open/close.
 *
 * A pad is an analog PC joystick: the left stick is the stick (the D-pad
 * pushes it all the way), South/East/West/North are buttons 1-4 and the
 * triggers 3 and 4. A raw joystick's first two axes and first four buttons
 * map straight across.
 *
 * Pads take joysticks A and B in connection order unless assigned
 * explicitly; a pad unplugged and plugged back in (matched by GUID and
 * name) gets its assignment back, and every connect and disconnect is
 * reported to the session for the frontend's toast.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "session_internal.h"

#define MAX_PADS 8

typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit joystick, -1 = automatic */
    int stick;                /* the joystick it drove on the last poll, or -1 */
    SDL_GUID guid;
    char name[64];
} pad_slot;

typedef struct {
    struct msdossession *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    volatile int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    /* explicit assignments of pads that went away, for when they come back */
    struct { SDL_GUID guid; char name[64]; int assign; int used; } gone[MAX_PADS];
    int gone_next;
} gamepad_state;

static gamepad_state *g_state;   /* one per process: SDL's subsystem is */

int msdos_pad_for_stick(const int *assign, int npads, int stick)
{
    int i, next = 0;
    for (i = 0; i < npads; i++)
        if (assign[i] == stick) return i;
    for (i = 0; i < npads; i++) {
        if (assign[i] >= 0) continue;   /* explicitly elsewhere */
        if (next == stick) return i;
        next++;
    }
    return -1;
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

static void slot_release_all(gamepad_state *g, pad_slot *slot)
{
    if (slot->stick >= 0)
        session_gamepad_stick(g->session, slot->stick, 0, 0, 0, 0);
    slot->stick = -1;
}

static void slot_close(gamepad_state *g, pad_slot *slot)
{
    slot_release_all(g, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_stick_locked(gamepad_state *g, int idx);

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            int h;
            memset(slot, 0, sizeof *slot);
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->stick = -1;
            slot->guid = SDL_GetJoystickGUIDForID(id);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            for (h = 0; h < MAX_PADS; h++) {
                if (g->gone[h].used && !memcmp(&g->gone[h].guid, &slot->guid, sizeof slot->guid)
                    && !strcmp(g->gone[h].name, slot->name)) {
                    slot->assign = g->gone[h].assign;
                    g->gone[h].used = 0;
                    break;
                }
            }
            g->generation++;
            fprintf(stderr, "msdos gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            {
                char msg[128];
                int stick = effective_stick_locked(g, g->npads - 1);
                if (stick >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (joystick %c)", slot->name, 'A' + stick);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                session_gamepad_event(g->session, msg);
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    int idx, i;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        char msg[128];
        fprintf(stderr, "msdos gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        if (slot->assign >= 0) {
            int k = g->gone_next++ % MAX_PADS;
            g->gone[k].guid = slot->guid;
            memcpy(g->gone[k].name, slot->name, sizeof g->gone[k].name);
            g->gone[k].assign = slot->assign;
            g->gone[k].used = 1;
        }
        slot_close(g, slot);
        session_gamepad_event(g->session, msg);
        for (i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
    }
    pthread_mutex_unlock(&g->lock);
}

/* ---- polling ------------------------------------------------------------------ */

/* The joystick a slot drives right now. Locked by the caller. */
static int effective_stick_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], i, stick;
    for (i = 0; i < MAX_PADS; i++) assign[i] = -1;
    for (i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    for (stick = 0; stick < MSDOS_JOYSTICKS; stick++)
        if (msdos_pad_for_stick(assign, g->npads, stick) == idx) return stick;
    return -1;
}

static int clamp_axis(int v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static void poll_one(gamepad_state *g, pad_slot *slot, int stick)
{
    int x = 0, y = 0;
    unsigned buttons = 0;

    if (stick != slot->stick) {
        slot_release_all(g, slot);
        slot->stick = stick;
        if (stick < 0) return;
    }
    if (slot->handle) {
        SDL_Gamepad *p = slot->handle;
        x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
        y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) x = -32768;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) x = 32767;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP)) y = -32768;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) y = 32767;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH)) buttons |= 1;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST)) buttons |= 2;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST)) buttons |= 4;
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH)) buttons |= 8;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) buttons |= 4;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) buttons |= 8;
    } else if (slot->joy) {
        SDL_Joystick *j = slot->joy;
        int b;
        if (SDL_GetNumJoystickAxes(j) > 0) x = SDL_GetJoystickAxis(j, 0);
        if (SDL_GetNumJoystickAxes(j) > 1) y = SDL_GetJoystickAxis(j, 1);
        if (SDL_GetNumJoystickHats(j) > 0) {
            Uint8 m = SDL_GetJoystickHat(j, 0);
            if (m & SDL_HAT_LEFT) x = -32768;
            if (m & SDL_HAT_RIGHT) x = 32767;
            if (m & SDL_HAT_UP) y = -32768;
            if (m & SDL_HAT_DOWN) y = 32767;
        }
        for (b = 0; b < 4 && b < SDL_GetNumJoystickButtons(j); b++)
            if (SDL_GetJoystickButton(j, b)) buttons |= 1u << b;
    }
    session_gamepad_stick(g->session, stick, clamp_axis(x), clamp_axis(y), buttons, 1);
}

static void poll_sticks(gamepad_state *g)
{
    int i;
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_stick_locked(g, i));
    pthread_mutex_unlock(&g->lock);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0, i;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }

    while (!g->stop_requested) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            default:
                break;
            }
        }
        poll_sticks(g);
        SDL_Delay(16);   /* ~60 Hz, the machine's own sampling rate */
    }

    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->npads; i++) slot_close(g, &g->pads[i]);
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(struct msdossession *s)
{
    gamepad_state *g;
    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    g->running = 1;
    g_state = g;
    s->gamepad = g;
    return 0;
}

void gamepad_stop(struct msdossession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->stop_requested = 1;
    pthread_join(g->thread, NULL);
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    if (g_state == g) g_state = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int msdossession_gamepad_count(msdossession *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int msdossession_gamepad_name(msdossession *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", slot_name(&g->pads[idx]));
    pthread_mutex_unlock(&g->lock);
    return n;
}

void msdossession_gamepad_assign(msdossession *s, int idx, int stick)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        g->pads[idx].assign = (stick >= 0 && stick < MSDOS_JOYSTICKS) ? stick : -1;
    pthread_mutex_unlock(&g->lock);
}

int msdossession_gamepad_assignment(msdossession *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int msdossession_gamepad_effective_stick(msdossession *s, int idx)
{
    gamepad_state *g = s->gamepad;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_stick_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned msdossession_gamepad_generation(msdossession *s)
{
    gamepad_state *g = s->gamepad;
    unsigned r;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}
