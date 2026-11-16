/*
 * The Sound tab's decoder on synthetic I/O: PC speaker, OPL2/OPL3 register file, Sound
 * Blaster DSP. No ROMs, no machine, MinGW-safe.
 */
#include "test_util.h"

#include <math.h>

#include "msdossnd.h"

static void
out8(uint16_t port, uint8_t val)
{
    msdos_snd_io_observe(port, 1, 1, val);
}

static void
opl(uint16_t addr_port, uint16_t data_port, uint8_t reg, uint8_t val)
{
    out8(addr_port, reg);
    out8(data_port, val);
}

static void
near(double got, double want, double tolerance, const char *what)
{
    CHECK(fabs(got - want) <= tolerance, "%s: got %f want %f", what, got, want);
}

static int g_watched[65536];
static void
watch(uint16_t port)
{
    g_watched[port]++;
}

static void
test_helpers(void)
{
    near(msdos_snd_opl_hz(0x244, 4), 440.0, 1.0, "A440");
    near(msdos_snd_opl_hz(0, 4), 0.0, 0.001, "fnum 0");
    CHECK(msdos_snd_opl_hz(100, 9) == 0.0, "block out of range");
    CHECK(msdos_snd_hz_to_note(440.0) == 69, "440 Hz is note 69");
    CHECK(msdos_snd_hz_to_note(261.63) == 60, "middle C");
    CHECK(msdos_snd_hz_to_note(1.0) == -1, "1 Hz has no note");
    char buf[8];
    CHECK(!strcmp(msdos_snd_note_name(69, buf, sizeof buf), "A4"), "name A4");
    CHECK(!strcmp(msdos_snd_note_name(61, buf, sizeof buf), "C#4"), "name C#4");
    CHECK(!strcmp(msdos_snd_note_name(-1, buf, sizeof buf), "--"), "name none");
    CHECK(msdos_snd_get(NULL) == 0, "get(NULL)");
}

static void
test_watch(void)
{
    memset(g_watched, 0, sizeof g_watched);
    msdos_snd_watch_ports(watch);
    msdos_snd_watch_ports(NULL);
    CHECK(g_watched[0x61] && g_watched[0x40] && g_watched[0x43], "PIT and PPI ports");
    for (int port = 0x388; port <= 0x38B; port++)
        CHECK(g_watched[port] == 1, "OPL port %X", port);
    for (int port = 0x220; port <= 0x22B; port++)
        CHECK(g_watched[port] == 1, "SB port %X", port);
    CHECK(!g_watched[0x60] && !g_watched[0x44] && !g_watched[0x230], "nothing extra");
}

static void
test_speaker(void)
{
    msdos_snd_reset();
    msdos_snd_state st;
    CHECK(msdos_snd_get(&st) == 1, "get");
    CHECK(!st.speaker_direct, "no machine: shadow");
    CHECK(!st.speaker_on && st.speaker_hz == 0.0, "silent at start");

    /* counter 2, lobyte/hibyte, mode 3, reload 1193 = 1000 Hz */
    out8(0x43, 0xB6);
    out8(0x42, 1193 & 0xFF);
    out8(0x42, 1193 >> 8);
    out8(0x61, 0x03);
    msdos_snd_get(&st);
    CHECK(st.pit_mode == 3 && st.pit_reload == 1193, "reload %u mode %d", (unsigned) st.pit_reload,
          st.pit_mode);
    CHECK(st.speaker_on, "gate and enable: on");
    near(st.speaker_hz, 1000.15, 0.5, "1000 Hz");
    CHECK(st.pit2_writes == 1 && st.port61_writes == 1, "write counts");

    out8(0x61, 0x01);
    msdos_snd_get(&st);
    CHECK(!st.speaker_on, "enable clear: off");

    /* a mode 2 reload of 0 is 65536 */
    out8(0x43, 0xB4);
    out8(0x42, 0);
    out8(0x42, 0);
    out8(0x61, 0x03);
    msdos_snd_get(&st);
    CHECK(st.pit_reload == 65536 && st.pit_mode == 2, "reload 0 is 65536");
    near(st.speaker_hz, 18.2, 0.05, "18.2 Hz");

    /* a one-shot mode is not a tone */
    out8(0x43, 0xB0);
    out8(0x42, 0x10);
    out8(0x42, 0x00);
    msdos_snd_get(&st);
    CHECK(!st.speaker_on, "mode 0 is not a tone");

    /* counter 0 and 1 control bytes are ignored */
    out8(0x43, 0x36);
    out8(0x40, 0);
    out8(0x40, 0);
    msdos_snd_get(&st);
    CHECK(st.pit_mode == 0 && st.pit_reload == 0x10, "counter 0 untouched the speaker");

    /* hibyte only */
    out8(0x43, 0xA6);
    out8(0x42, 0x04);
    msdos_snd_get(&st);
    CHECK(st.pit_reload == 0x400 && st.pit_mode == 3, "hibyte only");

    /* by hand: bit 1 toggled with the gate off */
    out8(0x61, 0x00);
    out8(0x61, 0x02);
    msdos_snd_get(&st);
    CHECK(st.speaker_dac && !st.speaker_on, "DAC style");
    out8(0x61, 0x03);
    msdos_snd_get(&st);
    CHECK(!st.speaker_dac, "gate back on clears DAC");

    /* reads change nothing */
    uint32_t keep = st.port61_writes;
    msdos_snd_io_observe(0x61, 0, 1, 0);
    msdos_snd_get(&st);
    CHECK(st.port61_writes == keep, "reads ignored");
}

static void
test_opl2(void)
{
    msdos_snd_reset();
    msdos_snd_state st;
    msdos_snd_get(&st);
    CHECK(!st.opl_seen && st.opl_channels == 9, "OPL unseen at start");

    /* channel 0: A440 (fnum 0x244, block 4), key on; modulator/carrier of the AdLib manual */
    opl(0x388, 0x389, 0x20, 0xA1);   /* mod: AM 1, VIB 0, EG 1, KSR 0, mult 1 */
    opl(0x388, 0x389, 0x23, 0x41);   /* car: VIB, mult 1 */
    opl(0x388, 0x389, 0x40, 0x5C);   /* mod: KSL 1, TL 28 */
    opl(0x388, 0x389, 0x43, 0x07);   /* car: TL 7 */
    opl(0x388, 0x389, 0x60, 0xF2);
    opl(0x388, 0x389, 0x63, 0xF4);
    opl(0x388, 0x389, 0x80, 0x73);
    opl(0x388, 0x389, 0x83, 0x25);
    opl(0x388, 0x389, 0xE0, 0x02);
    opl(0x388, 0x389, 0xC0, 0x0E);   /* feedback 7, FM */
    opl(0x388, 0x389, 0xA0, 0x44);
    opl(0x388, 0x389, 0xB0, 0x32 | 0x10);   /* key on (0x20), block 4 (0x10), fnum high 2 */
    msdos_snd_get(&st);
    CHECK(st.opl_seen && !st.opl3 && st.opl_channels == 9, "OPL2");
    CHECK(st.opl_writes == 12, "12 writes, got %u", (unsigned) st.opl_writes);
    const msdos_snd_channel *ch = &st.channel[0];
    CHECK(ch->keyon && ch->fnum == 0x244 && ch->block == 4, "fnum %X block %d", ch->fnum, ch->block);
    near(ch->hz, 440.0, 1.0, "channel 0 Hz");
    CHECK(ch->note == 69, "note %d", ch->note);
    CHECK(ch->feedback == 7 && !ch->additive, "feedback %d additive %d", ch->feedback, ch->additive);
    CHECK(ch->pan == 3 && !ch->four_op, "OPL2 pan");
    CHECK(ch->mod.tremolo == 1 && ch->mod.sustain == 1 && ch->mod.multiple == 1, "mod 20h");
    CHECK(ch->car.vibrato == 1 && ch->car.multiple == 1, "car 20h");
    CHECK(ch->mod.key_scale == 1 && ch->mod.total_level == 28, "mod 40h");
    CHECK(ch->car.total_level == 7, "car 40h");
    CHECK(ch->mod.attack == 15 && ch->mod.decay == 2, "mod 60h");
    CHECK(ch->car.attack == 15 && ch->car.decay == 4, "car 60h");
    CHECK(ch->mod.sustain_level == 7 && ch->mod.release == 3, "mod 80h");
    CHECK(ch->car.sustain_level == 2 && ch->car.release == 5, "car 80h");
    CHECK(ch->mod.wave == 2, "mod wave");

    /* key off keeps the pitch, loses the note */
    opl(0x388, 0x389, 0xB0, 0x12);
    msdos_snd_get(&st);
    CHECK(!st.channel[0].keyon && st.channel[0].note == -1 && st.channel[0].fnum == 0x244,
          "key off");

    /* channel 5 uses operator offsets 0x0D/0x10... (slots 13 and 16): the map is {0,1,2,8,9,10,16,17,18}+3 */
    opl(0x388, 0x389, 0x20 + 10, 0x01);   /* channel 5's modulator is offset 10 */
    opl(0x388, 0x389, 0x20 + 13, 0x02);   /* its carrier is offset 13 */
    msdos_snd_get(&st);
    CHECK(st.channel[5].mod.multiple == 1 && st.channel[5].car.multiple == 2, "channel 5 slots");
    CHECK(st.channel[4].mod.multiple == 0, "channel 4 untouched");

    /* the second bank is ignored without OPL3 */
    opl(0x38A, 0x38B, 0xB0, 0x30);
    msdos_snd_get(&st);
    CHECK(!st.channel[9].keyon && st.opl_channels == 9, "no second bank on OPL2");

    /* rhythm and depths */
    opl(0x388, 0x389, 0xBD, 0xE5);
    msdos_snd_get(&st);
    CHECK(st.rhythm == 0x25 && st.tremolo_depth && st.vibrato_depth, "BDh %02X", st.rhythm);

    /* the Sound Blaster's own OPL ports reach the same file */
    opl(0x220, 0x221, 0xA1, 0x81);
    opl(0x228, 0x229, 0xA2, 0x82);
    msdos_snd_get(&st);
    CHECK(st.opl_reg[0xA1] == 0x81 && st.opl_reg[0xA2] == 0x82, "220h and 228h");

    /* reset forgets */
    msdos_snd_reset();
    msdos_snd_get(&st);
    CHECK(!st.opl_seen && st.opl_reg[0xA1] == 0, "reset");
}

static void
test_opl3(void)
{
    msdos_snd_reset();
    msdos_snd_state st;
    /* register 105h bit 0 turns OPL3 on; before that the high bank only takes 05h */
    opl(0x38A, 0x38B, 0x05, 0x01);
    opl(0x38A, 0x38B, 0x04, 0x3F);        /* 4-op connection: ignored as 04h before OPL3? no: after */
    msdos_snd_get(&st);
    CHECK(st.opl3 && st.opl_channels == 18, "OPL3 on");
    CHECK(st.opl_reg[0x105] == 1, "105h");
    CHECK(st.four_op_mask == 0x3F && st.channel[0].four_op && st.channel[3].four_op == 0 &&
              st.channel[9].four_op,
          "4-op leads: ch0 %d ch3 %d ch9 %d", st.channel[0].four_op, st.channel[3].four_op,
          st.channel[9].four_op);

    opl(0x38A, 0x38B, 0xA0, 0x44);
    opl(0x38A, 0x38B, 0xB0, 0x32);
    opl(0x38A, 0x38B, 0xC0, 0x31);        /* left only? bit 4 = left, bit 5 = right: here left, additive */
    msdos_snd_get(&st);
    const msdos_snd_channel *ch = &st.channel[9];
    CHECK(ch->keyon && ch->fnum == 0x244 && ch->block == 4, "channel 9");
    CHECK(ch->pan == 3 && ch->additive, "pan %d additive %d", ch->pan, ch->additive);

    opl(0x38A, 0x38B, 0xC0, 0x10);
    msdos_snd_get(&st);
    CHECK(st.channel[9].pan == 1, "left only");
    CHECK(st.opl_reg[0xC0] == 0, "low bank untouched");

    /* a low-bank write after a high-bank one goes low */
    opl(0x388, 0x389, 0xC0, 0x20);
    msdos_snd_get(&st);
    CHECK(st.channel[0].pan == 2 && st.channel[9].pan == 1, "banks separate");

    /* the high bank ignores 04h and 05h-less registers on OPL2 */
    msdos_snd_reset();
    opl(0x38A, 0x38B, 0xA0, 0x77);
    msdos_snd_get(&st);
    CHECK(st.opl_reg[0xA0] == 0x77 && st.opl_reg[0x1A0] == 0, "high bank off without OPL3");
}

static void
test_sb(void)
{
    msdos_snd_reset();
    msdos_snd_state st;
    msdos_snd_get(&st);
    CHECK(!st.sb_seen && st.sb_time_constant == -1 && st.sb_speaker_on == -1, "SB unseen");

    out8(0x226, 1);
    out8(0x226, 0);
    out8(0x22C, 0xD1);
    out8(0x22C, 0x40);            /* time constant 131 = 8000 Hz */
    out8(0x22C, 131);
    out8(0x22C, 0x14);            /* 8-bit DMA, length 0x01FF: both parameters must not be commands */
    out8(0x22C, 0xFF);
    out8(0x22C, 0x01);
    msdos_snd_get(&st);
    CHECK(st.sb_seen && st.sb_speaker_on == 1, "speaker on");
    CHECK(st.sb_time_constant == 131 && st.sb_sample_rate == 8000, "rate %d tc %d",
          st.sb_sample_rate, st.sb_time_constant);
    CHECK(st.sb_last_command == 0x14 && st.sb_commands == 3, "last %02X count %u", st.sb_last_command,
          (unsigned) st.sb_commands);

    out8(0x22C, 0x41);            /* 44100 Hz, high byte first */
    out8(0x22C, 0xAC);
    out8(0x22C, 0x44);
    out8(0x22C, 0xD3);
    msdos_snd_get(&st);
    CHECK(st.sb_sample_rate == 44100 && st.sb_time_constant == -1 && st.sb_speaker_on == 0,
          "16-bit rate: %d", st.sb_sample_rate);

    out8(0x224, 0x22);
    out8(0x225, 0xCC);
    msdos_snd_get(&st);
    CHECK(st.sb_mixer[0x22] == 0xCC && st.sb_mixer_index == 0x22, "mixer");

    out8(0x226, 1);               /* a DSP reset */
    msdos_snd_get(&st);
    CHECK(st.sb_speaker_on == -1 && st.sb_sample_rate == 0, "DSP reset");

    /* the OPL ports at 220h-223h are not the DSP */
    msdos_snd_reset();
    opl(0x220, 0x221, 0xA0, 1);
    msdos_snd_get(&st);
    CHECK(!st.sb_seen && st.opl_seen, "220h is the OPL");
}

static void
test_text(void)
{
    msdos_snd_reset();
    char text[8192];
    int len = msdos_snd_text(text, sizeof text);
    CHECK(len > 0 && (size_t) len == strlen(text), "length %d", len);
    CHECK(strstr(text, "PC speaker (decoded, from observed writes)"), "speaker header: %s", text);
    CHECK(strstr(text, "silent"), "silent");
    CHECK(strstr(text, "no OPL write seen"), "no OPL");
    CHECK(strstr(text, "no DSP or mixer write seen"), "no DSP");
    CHECK(strstr(text, "no sound device configured"), "no card");

    out8(0x43, 0xB6);
    out8(0x42, 0xA9);
    out8(0x42, 0x04);             /* 1193 */
    out8(0x61, 3);
    opl(0x388, 0x389, 0xA0, 0x44);
    opl(0x388, 0x389, 0xB0, 0x32 | 0x10);
    opl(0x388, 0x389, 0xBD, 0x20);
    out8(0x22C, 0xD1);
    len = msdos_snd_text(text, sizeof text);
    CHECK(strstr(text, "tone on: 1000.2 Hz (B5)"), "tone line: %s", text);
    CHECK(strstr(text, "OPL2, 3 writes"), "OPL line: %s", text);
    CHECK(strstr(text, "rhythm on"), "rhythm");
    CHECK(strstr(text, "ch  0  KEY"), "channel line");
    CHECK(strstr(text, "A4"), "note name");
    CHECK(strstr(text, "mod mult") && strstr(text, "car mult"), "operators");
    CHECK(strstr(text, "speaker on"), "SB speaker");

    /* a short buffer is cut, not overrun, and stays terminated */
    char tiny[40];
    memset(tiny, 'x', sizeof tiny);
    len = msdos_snd_text(tiny, (int) sizeof tiny);
    CHECK(len == (int) strlen(tiny) && len < (int) sizeof tiny, "tiny buffer");
    CHECK(msdos_snd_text(NULL, 10) == 0 && msdos_snd_text(text, 1) == 0, "bad arguments");
}

int
main(void)
{
    test_helpers();
    test_watch();
    test_speaker();
    test_opl2();
    test_opl3();
    test_sb();
    test_text();
    printf("sound: ok\n");
    return 0;
}
