/* FujiNet's link speed: a plain FreeDOS floppy that loads FUJINET.SYS
   (COM2, 115200), then COPY C:\CONFIG.EXE NUL from FujiNet's autorun disk.
   Sector replies are counted in the debugger's FujiNet byte tap against
   the BIOS tick count, so the rate is in the PC's own time: it has to be
   near 115200 bps, not the ~1 KB/s of pacing in host time, and no reply may
   be lost (no RESEND). Then FujiNet restarts and the drive must still
   answer. "fifo" (default) keeps the 16550's FIFO, "nofifo" turns it off,
   the 8250 path a PCjr takes; there the closing END FUJINET.SYS leaves
   unread holds the port's receive side shut, so only the status poll can
   notice the restart. SKIPs (77) when the runtime was not built. */
#include "msdosdebug.h"
#include "test_util.h"

#define TICKS_PER_SEC 18.2065

static uint16_t ticks(msdosdebug *d)
{
    uint8_t t[2] = {0, 0};
    msdosdebug_read(d, 0x46C, t, 2);
    return (uint16_t)(t[0] | (t[1] << 8));
}

static int screen_has(msdossession *s, const char *text)
{
    static char buf[8192];
    return msdossession_copy_screen_text(s, buf, sizeof buf) > 0 && strstr(buf, text) != NULL;
}

static int dos_error(msdossession *s)
{
    return screen_has(s, "Abort") || screen_has(s, "eneral failure");
}

int main(int argc, char **argv)
{
    static msdos_fuji_packet pk[512];
    const int nofifo = argc > 1 && !strcmp(argv[1], "nofifo");
    msdossession *s = test_session_ex(nofifo ? "fnspeed-nofifo" : "fnspeed", 1, 1);
    msdossession_start_opts o = {1, 0, 0};
    msdosdebug *d;
    char msg[512], line[96], uart[512], *p;
    char drive = 'C';
    uint64_t seen = 0;
    uint16_t t0 = 0, t1 = 0;
    long bytes = 0;
    int sectors = 0, resends = 0, n, i, t;
    double secs, rate;
    CHECK(s, "new");

    test_set_pref(s, "machine", MSDOS_M_5160);
    test_set_pref(s, "fujinet_com", 1);
    CHECK(msdossession_floppy_insert(s, 0, MSDOS_TEST_FUJINET_FLOPPY, 1, msg, sizeof msg) == 0,
          "insert: %s", msg);
    if (msdossession_start(s, &o) != 0 || !msdossession_fujinet_running(s)) {
        fprintf(stderr, "SKIP: no FujiNet runtime (%s)\n", msdossession_last_error(s));
        msdossession_free(s);
        return 77;
    }
    d = msdossession_debugger(s);
    CHECK(d, "debugger");
    for (t = 0; t < 5000 && !msdossession_fujinet_link(s); t += 50)
        test_sleep_ms(50);
    CHECK(msdossession_fujinet_link(s), "the COM2 link");
    CHECK(test_wait_text(s, "attached", 120000), "FUJINET.SYS found FujiNet");
    CHECK(test_wait_text(s, "A:\\>", 60000), "the DOS prompt");
    {
        static char screen[8192];
        msdossession_copy_screen_text(s, screen, sizeof screen);
        if ((p = strstr(screen, "drives ")) != NULL && p[7] >= 'C' && p[7] <= 'Z')
            drive = p[7];
    }

    if (nofifo)
        msdosdebug_command(d, "o 2fa 0", msg, sizeof msg);
    msdosdebug_uart(d, uart, sizeof uart);
    printf("%s", uart);
    CHECK(strstr(uart, nofifo ? "FIFO off" : "FIFO on"), "the FIFO");
    CHECK(strstr(uart, "Speed 115200"), "115200 bps");

    msdosdebug_fujinet_trace_clear(d);
    snprintf(line, sizeof line, "COPY %c:\\CONFIG.EXE NUL\nECHO FNSPEED-DONE\n", drive);
    CHECK(msdossession_paste(s, line) == 0, "paste");
    for (t = 0; t < 240000 && !screen_has(s, "\nFNSPEED-DONE"); t += 20) {
        const uint16_t now = ticks(d);
        n = msdosdebug_fujinet_trace(d, pk, 512);
        for (i = 0; i < n; i++) {
            if (pk[i].serial <= seen)
                continue;
            seen = pk[i].serial;
            if (!pk[i].to_pc && pk[i].command == 0x05)
                resends++;
            if (pk[i].to_pc && pk[i].nbytes >= 512) {
                if (!sectors++)
                    t0 = now;
                else
                    bytes += 512;
                t1 = now;
            }
        }
        if (!(t % 500))
            CHECK(!dos_error(s), "a DOS error during the copy");
        test_sleep_ms(20);
    }
    CHECK(screen_has(s, "\nFNSPEED-DONE"), "the copy finished");
    CHECK(!dos_error(s), "a DOS error during the copy");

    secs = (uint16_t)(t1 - t0) / TICKS_PER_SEC;
    rate = secs > 0 ? bytes / secs : 0;
    printf("fujinet_speed %s: %d sectors, %ld bytes in %.2f s of PC time: %.0f B/s, %d RESENDs\n",
           nofifo ? "nofifo" : "fifo", sectors, bytes, secs, rate, resends);
    CHECK(sectors >= 32, "only %d sectors", sectors);
    CHECK(resends == 0, "%d RESENDs: replies were lost", resends);
    CHECK(rate >= 4000, "%.0f B/s: far below 115200 bps", rate);
    CHECK(rate <= 14400, "%.0f B/s: faster than 115200 bps allows", rate);

    /* FujiNet restarts: the link must come back and the drive answer. */
    CHECK(msdossession_fujinet_restart(s) == 0, "restart FujiNet");
    test_sleep_ms(500);
    for (t = 0; t < 10000 && !msdossession_fujinet_link(s); t += 50)
        test_sleep_ms(50);
    CHECK(msdossession_fujinet_link(s), "the link after a restart");
    snprintf(line, sizeof line, "DIR %c:\nECHO FNSPEED-AGAIN\n", drive);
    CHECK(msdossession_paste(s, line) == 0, "paste");
    CHECK(test_wait_text(s, "\nFNSPEED-AGAIN", 120000), "DIR after the restart");
    CHECK(!dos_error(s), "a DOS error after the restart");

    msdossession_stop(s);
    msdossession_free(s);
    printf("fujinet_speed %s: ok\n", nofifo ? "nofifo" : "fifo");
    return 0;
}
