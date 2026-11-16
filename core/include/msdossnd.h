/*
 * msdossnd -- the debugger's Sound tab: what the PC speaker and the FM chip are doing.
 *
 * What is decoded and what is only listed:
 *   decoded      PC speaker (PIT channel 2 reload, port 61h gate and enable bits),
 *                AdLib / Sound Blaster OPL2 and OPL3 register file (ports 388h-38Bh and
 *                the Sound Blaster's 220h-223h and 228h/229h): per channel frequency,
 *                block, key-on, instrument; rhythm mode; OPL3 mode.
 *   decoded      Sound Blaster DSP commands seen at 22Ch (time constant, sample rate,
 *                speaker on/off, last command) and the mixer index/data at 224h/225h.
 *                Only the default base 220h is watched.
 *   listed only  every configured sound card by name, and the PCjr's SN76496.
 *
 * The OPL chip's own registers are not reachable in 86Box, so they are a shadow built from
 * the writes the debugger sees (msdos_snd_io_observe): it knows nothing written before the
 * debugger attached or since the last msdos_snd_reset(). The PC speaker is read straight
 * from 86Box's PIT and PPI when a machine runs, and from the shadow otherwise.
 *
 * Threads: msdos_snd_io_observe, msdos_snd_get and msdos_snd_text run on the emulation
 * thread (the engine calls them through its host job, like msdosdebug_video_snap). The
 * results are plain copies: the UI thread never touches 86Box.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MSDOSSND_H
#define MSDOSSND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSDOS_SND_OPL_CHANNELS 18   /* 9 for OPL2, 18 for OPL3 */
#define MSDOS_SND_CARDS 5           /* 86Box's four sound card slots and the machine's own */

typedef struct msdos_snd_operator {
    uint8_t tremolo, vibrato, sustain, ksr;   /* reg 20h bits 7..4 */
    uint8_t multiple;                         /* reg 20h bits 3..0 */
    uint8_t key_scale;                        /* reg 40h bits 7..6 */
    uint8_t total_level;                      /* reg 40h bits 5..0 (0 loudest) */
    uint8_t attack, decay;                    /* reg 60h */
    uint8_t sustain_level, release;           /* reg 80h */
    uint8_t wave;                             /* reg E0h */
} msdos_snd_operator;

typedef struct msdos_snd_channel {
    int      keyon;
    int      fnum;                            /* 10 bits */
    int      block;                           /* 0..7 */
    double   hz;                              /* fnum * 49716 / 2^(20-block) */
    int      note;                            /* nearest MIDI note, -1 when silent or out of range */
    int      feedback;                        /* reg C0h bits 3..1 */
    int      additive;                        /* reg C0h bit 0: 1 = both operators sound */
    int      pan;                             /* OPL3: bit 0 = left, bit 1 = right; OPL2 = 3 */
    int      four_op;                         /* OPL3: this channel is the lead of a 4-op pair */
    msdos_snd_operator mod, car;
} msdos_snd_channel;

typedef struct msdos_snd_card {
    char name[48];
    int  decoded;                             /* 1 = the Sound tab decodes it, 0 = listed only */
} msdos_snd_card;

typedef struct msdos_snd_state {
    /* ---- PC speaker (always decoded) */
    int      speaker_direct;                  /* 1 = read from 86Box, 0 = from the shadow */
    uint8_t  port61;                          /* bit 0 timer 2 gate, bit 1 speaker data */
    int      pit_mode;                        /* counter 2 mode 0..5 */
    uint32_t pit_reload;                      /* counter 2 reload, 0 means 65536 and is reported as 65536 */
    double   speaker_hz;                      /* 1193182 / reload, 0 when not sounding */
    int      speaker_on;                      /* gate and enable set, rate generator or square wave */
    int      speaker_dac;                     /* bit 1 toggled by hand (samples, clicks) */
    uint32_t port61_writes, pit2_writes;      /* counted since attach */

    /* ---- FM chip */
    int      opl_seen;                        /* the shadow has had a write */
    int      opl3;                            /* OPL3 mode (reg 105h bit 0) */
    int      opl_channels;                    /* 9 or 18 */
    uint32_t opl_writes;
    uint8_t  opl_reg[512];                    /* the register file, 0..FFh and 100h..1FFh */
    uint8_t  rhythm;                          /* reg BDh: bit 5 rhythm on, bits 4..0 drums */
    uint8_t  tremolo_depth, vibrato_depth;    /* reg BDh bits 7, 6 */
    uint8_t  four_op_mask;                    /* reg 104h */
    msdos_snd_channel channel[MSDOS_SND_OPL_CHANNELS];

    /* ---- Sound Blaster DSP and mixer (only what is written at the default base) */
    int      sb_seen;
    uint8_t  sb_last_command;
    int      sb_time_constant;                /* -1 unknown */
    int      sb_sample_rate;                  /* Hz, 0 unknown */
    int      sb_speaker_on;                   /* -1 unknown */
    uint32_t sb_commands;
    uint8_t  sb_mixer[256];
    uint8_t  sb_mixer_index;

    /* ---- the cards, from the session's configuration */
    int            card_count;
    msdos_snd_card card[MSDOS_SND_CARDS];
} msdos_snd_state;

/* Forget everything observed (a machine reset or a new machine). */
void msdos_snd_reset(void);

/*
 * Ports the observer needs: calls watch(port) for every port. The engine passes a function
 * that sets its I/O watch bit for the port and keeps the bit when it rebuilds the breakpoint
 * watch bitmaps.
 */
void msdos_snd_watch_ports(void (*watch)(uint16_t port));

/* One I/O access the engine saw on a watched port. width is 1, 2 or 4; val the data. */
void msdos_snd_io_observe(uint16_t port, int write, int width, uint32_t val);

/* A copy of the decoded state. Returns 1 always (the speaker is always there); 0 for out == NULL. */
int msdos_snd_get(msdos_snd_state *out);

/* The `sound` console command's text and the tab's text. Returns the length written (< dstsz). */
int msdos_snd_text(char *dst, int dstsz);

/* Pure helpers, tested on their own. */
double msdos_snd_opl_hz(int fnum, int block);             /* OPL clock 49716 Hz */
int    msdos_snd_hz_to_note(double hz);                   /* MIDI note number, -1 out of range */
const char *msdos_snd_note_name(int note, char *buf, size_t bufsz);   /* "A4"; "--" for -1 */

#ifdef __cplusplus
}
#endif

#endif
