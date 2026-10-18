/*
 * The game port's joysticks, fed by the app's gamepad layer
 * (msdos_host_joystick_set) instead of an OS joystick API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/gameport.h>

#include "msdos_host.h"

plat_joystick_state_t plat_joystick_state[MAX_PLAT_JOYSTICKS];
joystick_state_t      joystick_state[GAMEPORT_MAX][MAX_JOYSTICKS];

typedef struct {
    volatile int      present;
    volatile int      axes[4];
    volatile unsigned buttons;
    volatile int      pov;
} host_stick;

static host_stick sticks[MSDOS_HOST_STICKS];

void
msdos_host_joystick_set(int stick, const int axes[4], unsigned buttons, int pov)
{
    if (stick < 0 || stick >= MSDOS_HOST_STICKS)
        return;
    for (int i = 0; i < 4; i++)
        sticks[stick].axes[i] = axes ? axes[i] : 0;
    sticks[stick].buttons = buttons;
    sticks[stick].pov     = pov;
    sticks[stick].present = 1;
}

void
msdos_host_joystick_clear(void)
{
    memset((void *) sticks, 0, sizeof(sticks));
}

void
joystick_init(void)
{
}

void
joystick_close(void)
{
}

void
joystick_process(uint8_t gp)
{
    if (gp >= GAMEPORT_MAX || !joystick_type[gp])
        return;

    int max = joystick_get_max_joysticks(joystick_type[gp]);
    for (int js = 0; js < max && js < MAX_JOYSTICKS; js++) {
        joystick_state_t *st   = &joystick_state[gp][js];
        const host_stick *h    = js < MSDOS_HOST_STICKS ? &sticks[js] : NULL;
        int               have = h && h->present;

        st->plat_joystick_nr = have ? js + 1 : 0;
        for (int a = 0; a < joystick_get_axis_count(joystick_type[gp]) && a < MAX_JOY_AXES; a++)
            st->axis[a] = (have && a < 4) ? h->axes[a] : 0;
        for (int b = 0; b < joystick_get_button_count(joystick_type[gp]) && b < MAX_JOY_BUTTONS; b++)
            st->button[b] = have ? !!(h->buttons & (1u << b)) : 0;
        for (int p = 0; p < joystick_get_pov_count(joystick_type[gp]) && p < MAX_JOY_POVS; p++)
            st->pov[p] = (have && p == 0) ? h->pov : -1;
    }
}
