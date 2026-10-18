/* The real in-process FujiNet on a COM port: GLaBIOS boots the FujiNet
   floppy, FUJINET.SYS finds FujiNet on that port, and CONFIG asks it for
   the slots. SD-only fnconfig: no network hosts. The COM port is the
   argument (default 2). SKIPs (77) when the runtime was not built. */
#include "test_util.h"

int main(int argc, char **argv)
{
    static const char *const ports[] = { "3F8h", "2F8h", "3E8h", "2E8h" };
    const int com = argc > 1 ? atoi(argv[1]) : 2;
    msdossession *s = test_session_ex("netboot", 1, 1);
    msdossession_start_opts o = {1, 0, 0};
    char want[32];
    int t;
    CHECK(s, "new");
    CHECK(com >= 1 && com <= 4, "COM%d", com);
    test_set_pref(s, "machine", MSDOS_M_5160);
    test_set_pref(s, "fujinet_com", com - 1);
    if (msdossession_start(s, &o) != 0 || !msdossession_fujinet_running(s)) {
        fprintf(stderr, "SKIP: no FujiNet runtime (%s)\n", msdossession_last_error(s));
        msdossession_free(s);
        return 77;
    }
    for (t = 0; t < 5000 && !msdossession_fujinet_link(s); t += 50)
        test_sleep_ms(50);
    CHECK(msdossession_fujinet_link(s), "the COM%d link", com);
    snprintf(want, sizeof want, "Port: %s", ports[com - 1]);
    CHECK(test_wait_text(s, want, 60000), "FUJINET.SYS on COM%d", com);
    CHECK(test_wait_text(s, "attached", 60000), "FUJINET.SYS found FujiNet");
    CHECK(test_wait_fujinet_log(s, "READ DEVICE SLOTS", 120000), "CONFIG asked for the slots");

    msdossession_stop(s);
    msdossession_free(s);
    printf("netboot COM%d: ok\n", com);
    return 0;
}
