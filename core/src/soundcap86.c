/*
 * The Sound tab's capture and decoder: the PC speaker, the FM chip (OPL2/OPL3) and the Sound
 * Blaster DSP, as msdossnd.h describes.
 *
 * The OPL chip's registers are write-only in 86Box, so they are a shadow built from the I/O
 * the debugger's hook sees. The PC speaker is read from 86Box's PIT and PPI when a machine
 * runs. Emulation thread only; nothing here runs a device's read handler.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "debug86.h"
#include "msdossnd.h"

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/pit.h>
#include <86box/ppi.h>
#include <86box/sound.h>

/* ---- the shadow ------------------------------------------------------------------------------ */

enum { PIT_RW_NONE = 0, PIT_RW_LOW = 1, PIT_RW_HIGH = 2, PIT_RW_WORD = 3 };

static struct {
    msdos_snd_state snap;
    uint32_t        opl_addr;          /* 9 bits: the bank and the register */
    int             pit_rw;
    int             pit_high_next;     /* the next byte is the high one */
    uint32_t        pit_partial;
    int             sb_params_left;
    uint8_t         sb_pending_command;
    uint32_t        sb_param_value;
    int             sb_rate_hi_next;
} shadow;

static const uint8_t k_op_offset[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

void
msdos_snd_reset(void)
{
    memset(&shadow, 0, sizeof shadow);
    shadow.snap.sb_time_constant = -1;
    shadow.snap.sb_speaker_on = -1;
    shadow.snap.pit_mode = 3;
    shadow.snap.pit_reload = 65536;
    shadow.snap.opl_channels = 9;
}

static int
shadow_ready(void)
{
    /* a zeroed shadow (first use before any reset) has opl_channels 0 */
    if (shadow.snap.opl_channels == 0)
        msdos_snd_reset();
    return 1;
}

void
msdos_snd_watch_ports(void (*watch)(uint16_t port))
{
    if (!watch)
        return;
    watch(0x61);
    for (uint16_t port = 0x40; port <= 0x43; port++)
        watch(port);
    for (uint16_t port = 0x388; port <= 0x38B; port++)
        watch(port);
    for (uint16_t port = 0x220; port <= 0x22B; port++)
        watch(port);
}

/* ---- pure helpers ---------------------------------------------------------------------------- */

double
msdos_snd_opl_hz(int fnum, int block)
{
    if (fnum < 0 || block < 0 || block > 7)
        return 0.0;
    return (double) fnum * 49716.0 / (double) (1u << (20 - block));
}

int
msdos_snd_hz_to_note(double hz)
{
    if (hz < 8.0)
        return -1;
    int note = (int) floor(69.0 + 12.0 * log2(hz / 440.0) + 0.5);
    return (note < 0 || note > 127) ? -1 : note;
}

const char *
msdos_snd_note_name(int note, char *buf, size_t bufsz)
{
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F",
                                           "F#", "G", "G#", "A", "A#", "B" };
    if (note < 0 || note > 127)
        snprintf(buf, bufsz, "--");
    else
        snprintf(buf, bufsz, "%s%d", names[note % 12], note / 12 - 1);
    return buf;
}

/* ---- observing ------------------------------------------------------------------------------- */

static void
pit_observe(uint16_t port, uint32_t val)
{
    if (port == 0x43) {
        if ((val >> 6) != 2)
            return;
        int rw_bits = (int) ((val >> 4) & 3);
        if (rw_bits == 0)
            return;                       /* a latch command */
        shadow.pit_rw = rw_bits;
        shadow.pit_high_next = (rw_bits == PIT_RW_HIGH);
        shadow.pit_partial = 0;
        int mode = (int) ((val >> 1) & 7);
        shadow.snap.pit_mode = mode >= 6 ? mode - 4 : mode;
    } else if (port == 0x42) {
        uint32_t byte = val & 0xFF;
        if (shadow.pit_rw == PIT_RW_NONE)
            return;
        if (shadow.pit_rw == PIT_RW_WORD && !shadow.pit_high_next) {
            shadow.pit_partial = byte;
            shadow.pit_high_next = 1;
            return;
        }
        uint32_t reload = shadow.pit_rw == PIT_RW_HIGH ? byte << 8
                          : shadow.pit_rw == PIT_RW_LOW ? byte
                                                   : shadow.pit_partial | (byte << 8);
        shadow.pit_high_next = (shadow.pit_rw == PIT_RW_HIGH);
        shadow.snap.pit_reload = reload ? reload : 65536;
        shadow.snap.pit2_writes++;
    }
}

static void
opl_write(uint32_t reg, uint8_t val)
{
    shadow.snap.opl_reg[reg & 0x1FF] = val;
    shadow.snap.opl_seen = 1;
    shadow.snap.opl_writes++;
}

static void
opl_observe(unsigned which, uint32_t val)
{
    uint8_t byte = (uint8_t) val;
    switch (which) {
        case 0:
            shadow.opl_addr = byte;
            break;
        case 2:
            shadow.opl_addr = (shadow.snap.opl_reg[0x105] & 1 || byte == 5) ? (0x100u | byte) : byte;
            break;
        default:
            opl_write(shadow.opl_addr, byte);
            break;
    }
}

static int
sb_param_count(uint8_t command)
{
    switch (command) {
        case 0x04: case 0x10: case 0x38: case 0x40: case 0xE0: case 0xE2: case 0xE4:
            return 1;
        case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48:
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x7D: case 0x7F: case 0x80:
            return 2;
        default:
            return (command >= 0xB0 && command <= 0xCF) ? 3 : 0;
    }
}

static void
sb_finish_command(void)
{
    if (shadow.sb_pending_command == 0x40) {
        shadow.snap.sb_time_constant = (int) (shadow.sb_param_value & 0xFF);
        shadow.snap.sb_sample_rate = (int) (1000000u / (256u - (uint32_t) shadow.snap.sb_time_constant));
    } else if (shadow.sb_pending_command == 0x41 || shadow.sb_pending_command == 0x42) {
        shadow.snap.sb_sample_rate = (int) (shadow.sb_param_value & 0xFFFF);   /* high byte first */
        shadow.snap.sb_time_constant = -1;
    }
}

static void
sb_observe(unsigned offset, uint32_t val)
{
    uint8_t byte = (uint8_t) val;
    shadow.snap.sb_seen = 1;
    switch (offset) {
        case 0x6:                         /* reset */
            shadow.snap.sb_time_constant = -1;
            shadow.snap.sb_sample_rate = 0;
            shadow.snap.sb_speaker_on = -1;
            shadow.sb_params_left = 0;
            break;
        case 0xC:                         /* command or parameter */
            if (shadow.sb_params_left > 0) {
                shadow.sb_param_value = (shadow.sb_param_value << 8) | byte;
                if (--shadow.sb_params_left == 0)
                    sb_finish_command();
                break;
            }
            shadow.snap.sb_last_command = byte;
            shadow.snap.sb_commands++;
            if (byte == 0xD1)
                shadow.snap.sb_speaker_on = 1;
            else if (byte == 0xD3)
                shadow.snap.sb_speaker_on = 0;
            shadow.sb_pending_command = byte;
            shadow.sb_param_value = 0;
            shadow.sb_params_left = sb_param_count(byte);
            break;
        case 0x4:
            shadow.snap.sb_mixer_index = byte;
            break;
        case 0x5:
            shadow.snap.sb_mixer[shadow.snap.sb_mixer_index] = byte;
            break;
        default:
            break;
    }
}

void
msdos_snd_io_observe(uint16_t port, int write, int width, uint32_t val)
{
    (void) width;
    if (!write)
        return;
    shadow_ready();
    if (port == 0x61) {
        uint8_t old = shadow.snap.port61;
        shadow.snap.port61 = (uint8_t) val;
        shadow.snap.port61_writes++;
        if (((old ^ (uint8_t) val) & 2) && !(val & 1))
            shadow.snap.speaker_dac = 1;          /* bit 1 moved by hand with the timer gated off */
        if (val & 1)
            shadow.snap.speaker_dac = 0;
    } else if (port >= 0x40 && port <= 0x43) {
        pit_observe(port, val);
    } else if (port >= 0x388 && port <= 0x38B) {
        opl_observe(port - 0x388u, val);
    } else if (port >= 0x220 && port <= 0x223) {
        /* the Sound Blaster's OPL: 220h-223h as 388h-38Bh. The DSP and mixer share 224h+. */
        opl_observe(port - 0x220u, val);
    } else if (port == 0x228 || port == 0x229) {
        opl_observe(port - 0x228u, val);
    } else if (port >= 0x224 && port <= 0x22F) {
        sb_observe(port - 0x220u, val);
    }
}

/* ---- decoding -------------------------------------------------------------------------------- */

static void
decode_operator(const uint8_t *reg, uint32_t bank, int offset, msdos_snd_operator *oper)
{
    uint32_t base = bank * 0x100u + (uint32_t) offset;
    uint8_t  r20 = reg[base + 0x20], r40 = reg[base + 0x40], r60 = reg[base + 0x60],
             r80 = reg[base + 0x80];
    oper->tremolo = (r20 >> 7) & 1;
    oper->vibrato = (r20 >> 6) & 1;
    oper->sustain = (r20 >> 5) & 1;
    oper->ksr = (r20 >> 4) & 1;
    oper->multiple = r20 & 15;
    oper->key_scale = r40 >> 6;
    oper->total_level = r40 & 63;
    oper->attack = r60 >> 4;
    oper->decay = r60 & 15;
    oper->sustain_level = r80 >> 4;
    oper->release = r80 & 15;
    oper->wave = reg[base + 0xE0] & (shadow.snap.opl3 ? 7 : 3);
}

static void
decode_opl(msdos_snd_state *snap)
{
    snap->opl3 = snap->opl_reg[0x105] & 1;
    snap->opl_channels = snap->opl3 ? 18 : 9;
    snap->rhythm = snap->opl_reg[0xBD] & 0x3F;
    snap->tremolo_depth = (snap->opl_reg[0xBD] >> 7) & 1;
    snap->vibrato_depth = (snap->opl_reg[0xBD] >> 6) & 1;
    snap->four_op_mask = snap->opl_reg[0x104] & 0x3F;
    for (int chan_index = 0; chan_index < MSDOS_SND_OPL_CHANNELS; chan_index++) {
        msdos_snd_channel *chan = &snap->channel[chan_index];
        uint32_t bank = (uint32_t) (chan_index / 9), index = (uint32_t) (chan_index % 9);
        uint32_t base = bank * 0x100u + index;
        memset(chan, 0, sizeof *chan);
        chan->note = -1;
        if (bank && !snap->opl3)
            continue;
        uint8_t freq_low = snap->opl_reg[base + 0xA0], key_block = snap->opl_reg[base + 0xB0],
                feed_conn = snap->opl_reg[base + 0xC0];
        chan->keyon = (key_block >> 5) & 1;
        chan->fnum = freq_low | ((key_block & 3) << 8);
        chan->block = (key_block >> 2) & 7;
        chan->hz = msdos_snd_opl_hz(chan->fnum, chan->block);
        chan->note = chan->keyon ? msdos_snd_hz_to_note(chan->hz) : -1;
        chan->feedback = (feed_conn >> 1) & 7;
        chan->additive = feed_conn & 1;
        chan->pan = snap->opl3 ? (feed_conn >> 4) & 3 : 3;
        chan->four_op = snap->opl3 && (snap->four_op_mask >> (index + (bank ? 3 : 0)) & 1) && index < 3;
        decode_operator(snap->opl_reg, bank, k_op_offset[index], &chan->mod);
        decode_operator(snap->opl_reg, bank, k_op_offset[index] + 3, &chan->car);
    }
}

static void
decode_speaker(msdos_snd_state *snap)
{
    int gate = snap->port61 & 1, enable = (snap->port61 >> 1) & 1;
    snap->speaker_on = gate && enable && (snap->pit_mode == 2 || snap->pit_mode == 3) && snap->pit_reload > 1;
    snap->speaker_hz = snap->speaker_on ? 1193182.0 / (double) snap->pit_reload : 0.0;
}

static void
read_cards(msdos_snd_state *snap)
{
    snap->card_count = 0;
    for (int slot = 0; slot < SOUND_CARD_MAX; slot++) {
        int which = sound_card_current[slot];
        if (which <= SOUND_NONE)
            continue;
        msdos_snd_card *entry = &snap->card[snap->card_count];
        memset(entry, 0, sizeof *entry);
        if (which == SOUND_INTERNAL) {
            snprintf(entry->name, sizeof entry->name, "Machine's own sound");
        } else {
            const device_t *dev = sound_card_getdevice(which);
            if (dev)
                device_get_name(dev, 0, entry->name);
            const char *internal = sound_card_get_internal_name(which);
            entry->decoded = internal && (!strncmp(internal, "adlib", 5) || !strncmp(internal, "sb", 2));
        }
        if (!entry->name[0])
            snprintf(entry->name, sizeof entry->name, "sound card %d", which);
        snap->card_count++;
    }
}

int
msdos_snd_get(msdos_snd_state *out)
{
    if (!out)
        return 0;
    shadow_ready();
    *out = shadow.snap;
    out->speaker_direct = 0;
    if (pit_devs[0].data && pit_devs[0].get_count == pit_classic_intf.get_count) {
        const pit_t *pit = (const pit_t *) pit_devs[0].data;
        const ctr_t *ctr = &pit->counters[2];
        out->speaker_direct = 1;
        out->port61 = ppi.pb;
        out->pit_mode = ctr->m >= 6 ? ctr->m - 4 : ctr->m;
        out->pit_reload = ctr->l ? ctr->l : 65536;
    }
    decode_speaker(out);
    decode_opl(out);
    read_cards(out);
    return 1;
}

/* ---- text ------------------------------------------------------------------------------------ */

typedef struct {
    char *dst;
    int   size, len;
} sink;

static void
put(sink *text_sink, const char *fmt, ...)
{
    if (text_sink->len >= text_sink->size - 1)
        return;
    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(text_sink->dst + text_sink->len, (size_t) (text_sink->size - text_sink->len), fmt, args);
    va_end(args);
    if (wrote < 0)
        return;
    text_sink->len = wrote >= text_sink->size - text_sink->len ? text_sink->size - 1 : text_sink->len + wrote;
}

static void
put_operator(sink *text_sink, const char *label, const msdos_snd_operator *oper)
{
    put(text_sink, "    %s mult %d  TL %2d  AR %2d DR %2d SL %2d RR %2d  wave %d%s%s%s%s\n", label,
        oper->multiple, oper->total_level, oper->attack, oper->decay, oper->sustain_level, oper->release,
        oper->wave, oper->tremolo ? "  AM" : "", oper->vibrato ? "  VIB" : "",
        oper->sustain ? "  EG-hold" : "", oper->ksr ? "  KSR" : "");
}

int
msdos_snd_text(char *dst, int dstsz)
{
    if (!dst || dstsz < 2)
        return 0;
    dst[0] = 0;
    msdos_snd_state *snap = malloc(sizeof *snap);
    if (!snap)
        return 0;
    msdos_snd_get(snap);
    sink text_sink = { dst, dstsz, 0 };

    put(&text_sink, "PC speaker (decoded, %s)\n", snap->speaker_direct ? "read from the PIT and PPI"
                                                              : "from observed writes");
    put(&text_sink, "  port 61h %02X  gate %d  speaker data %d   PIT 2 mode %d  reload %u\n", snap->port61,
        snap->port61 & 1, (snap->port61 >> 1) & 1, snap->pit_mode, (unsigned) snap->pit_reload);
    if (snap->speaker_on) {
        char note[8];
        msdos_snd_note_name(msdos_snd_hz_to_note(snap->speaker_hz), note, sizeof note);
        put(&text_sink, "  tone on: %.1f Hz (%s)\n", snap->speaker_hz, note);
    } else if (snap->speaker_dac) {
        put(&text_sink, "  off as a tone; bit 1 is toggled by hand (clicks or samples)\n");
    } else {
        put(&text_sink, "  silent\n");
    }
    put(&text_sink, "  writes seen since attach: port 61h %u, PIT 2 %u\n", (unsigned) snap->port61_writes,
        (unsigned) snap->pit2_writes);

    put(&text_sink, "FM chip (decoded from the writes seen since attach)\n");
    if (!snap->opl_seen) {
        put(&text_sink, "  no OPL write seen at 388h-38Bh or 220h-223h/228h-229h\n");
    } else {
        put(&text_sink, "  %s, %u writes   AM depth %d  vibrato depth %d%s\n", snap->opl3 ? "OPL3" : "OPL2",
            (unsigned) snap->opl_writes, snap->tremolo_depth, snap->vibrato_depth,
            (snap->rhythm & 0x20) ? "  rhythm on" : "");
        if (snap->rhythm & 0x20)
            put(&text_sink, "  drums: bass %d snare %d tom %d cymbal %d hi-hat %d\n", (snap->rhythm >> 4) & 1,
                (snap->rhythm >> 3) & 1, (snap->rhythm >> 2) & 1, (snap->rhythm >> 1) & 1, snap->rhythm & 1);
        for (int chan_index = 0; chan_index < snap->opl_channels; chan_index++) {
            const msdos_snd_channel *chan = &snap->channel[chan_index];
            char note[8];
            msdos_snd_note_name(chan->note, note, sizeof note);
            put(&text_sink, "  ch %2d  %-3s  fnum %4d block %d  %8.2f Hz  %-3s  fb %d  %s%s\n", chan_index,
                chan->keyon ? "KEY" : "off", chan->fnum, chan->block, chan->hz, note, chan->feedback,
                chan->additive ? "additive" : "FM",
                chan->four_op ? "  4-oper lead" : "");
            if (chan->keyon) {
                put_operator(&text_sink, "mod", &chan->mod);
                put_operator(&text_sink, "car", &chan->car);
            }
        }
    }

    put(&text_sink, "Sound Blaster DSP (decoded at the default base 220h only)\n");
    if (!snap->sb_seen) {
        put(&text_sink, "  no DSP or mixer write seen\n");
    } else {
        put(&text_sink, "  commands %u, last %02Xh   speaker %s", (unsigned) snap->sb_commands,
            snap->sb_last_command,
            snap->sb_speaker_on < 0 ? "unknown" : snap->sb_speaker_on ? "on" : "off");
        if (snap->sb_sample_rate)
            put(&text_sink, "   sample rate %d Hz", snap->sb_sample_rate);
        if (snap->sb_time_constant >= 0)
            put(&text_sink, " (time constant %d)", snap->sb_time_constant);
        put(&text_sink, "\n");
    }

    put(&text_sink, "Sound cards (from the configuration)\n");
    if (snap->card_count == 0) {
        put(&text_sink, "  no sound device configured; only the PC speaker can sound\n");
    } else {
        for (int card_index = 0; card_index < snap->card_count; card_index++)
            put(&text_sink, "  %s: %s\n", snap->card[card_index].name,
                snap->card[card_index].decoded ? "FM chip and DSP decoded above" : "listed only, not decoded");
    }
    free(snap);
    return text_sink.len;
}
