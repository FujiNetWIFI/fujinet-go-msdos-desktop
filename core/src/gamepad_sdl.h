/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware (gamepad_test), and what the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_GAMEPAD_SDL_H
#define MSDOS_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a hat resolves to. */
#define MSDOS_DIR_UP    1
#define MSDOS_DIR_DOWN  2
#define MSDOS_DIR_LEFT  4
#define MSDOS_DIR_RIGHT 8

/* A stick's four-way state as a mask over the target acts (bit n = act n: UP
 * DOWN LEFT RIGHT). Opposite directions cancel, so a hat pushed both ways
 * holds neither. Pure. */
unsigned msdos_acts_from_dir(int dir);

/* The left stick (x, y in -1..1, SDL's convention: +x right, +y down) as the
 * game port's axes -32768..32767, with a radial dead zone (0.12) rescaled so
 * the edge of the dead zone is 0 and full deflection is the stop. Pure. */
void msdos_axes_from_stick(float x, float y, int *axis_x, int *axis_y);

/* Which pad (index into assign[]) drives joystick `stick`: an explicit
 * assignment first, otherwise the automatic pads in connection order. Pure. */
int msdos_pad_for_stick(const int *assign, int npads, int stick);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering
 * (MSDOS_PAD_BTN_*), and back. Explicit tables, never a cast, so an SDL
 * reordering cannot desync them. */
int msdos_pad_button_from_sdl(uint8_t button);
int msdos_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif
