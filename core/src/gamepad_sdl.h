/*
 * gamepad_sdl -- the pure helper its test drives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOS_GAMEPAD_SDL_H
#define MSDOS_GAMEPAD_SDL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Which pad (index into assign[]) drives joystick `stick`: an explicit
 * assignment first, otherwise the automatic pads in connection order. */
int msdos_pad_for_stick(const int *assign, int npads, int stick);

#ifdef __cplusplus
}
#endif

#endif
