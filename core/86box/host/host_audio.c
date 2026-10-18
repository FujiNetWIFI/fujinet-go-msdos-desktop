/*
 * 86Box's sound backend for the embedded host: every buffer goes to
 * PcHost, which resamples and mixes them into the app's SDL output.
 * No capture, no device lists.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/sound.h>

#include "msdos_host.h"

/* host_ui.c owns the callbacks; it hands the audio one out here. */
extern void msdos_host_deliver_audio(int stream, const void *buf, int frames, int is_float, int rate);

static const int stream_of[I_MAX] = {
    [I_NORMAL] = MSDOS_AUDIO_MAIN, [I_MUSIC] = MSDOS_AUDIO_MUSIC,
    [I_WT] = MSDOS_AUDIO_WT,       [I_CD] = MSDOS_AUDIO_CD,
    [I_FDD] = MSDOS_AUDIO_FDD,     [I_HDD] = MSDOS_AUDIO_HDD,
    [I_YM2151] = MSDOS_AUDIO_MUSIC, [I_MIDI] = MSDOS_AUDIO_MUSIC,
};

void
inital(void)
{
    src_freqs[I_NORMAL] = src_freqs[I_FDD] = src_freqs[I_HDD] = sound_sample_rate;
    src_freqs[I_MIDI]   = midi_freq;
}

void
closeal(void)
{
}

void
al_set_midi(const int freq, const int buf_size)
{
    (void) buf_size;
    src_freqs[I_MIDI] = freq;
}

void
givealbuffer_common(const void *buf, const uint8_t src, const int size)
{
    if (src >= I_MAX || fast_forward || size <= 0)
        return;
    msdos_host_deliver_audio(stream_of[src], buf, size / 2, sound_is_float, (int) src_freqs[src]);
}

const char *sound_get_output_devices(void) { return NULL; }
const char *sound_get_input_devices(void) { return NULL; }
int  sound_get_device_sample_rate(const char *device_name) { (void) device_name; return 0; }
int  sound_get_device_supported_rates(const char *device_name, int *rates_out, int max_rates)
{
    (void) device_name;
    (void) rates_out;
    (void) max_rates;
    return 0;
}
void al_capture_open(void) { }
void al_capture_close(void) { }
int  al_capture_get_rate(void) { return 0; }
int  al_capture_available(void) { return 0; }
void al_capture_start(void) { }
void al_capture_stop(void) { }
void al_capture_get_data(int16_t *buf, size_t *len) { (void) buf; if (len) *len = 0; }
