/* The pref table across every machine; then GLaBIOS (core/tests/fixtures)
   boots FreeDOS from the FujiNet floppy with FujiNet off, and a COM port
   change applies with a reset: the driver comes up on the new port. */
#include "test_util.h"

int main(void)
{
    msdossession *s = test_session_ex("session", 0, 1);
    int n = msdossession_pref_count(), i, m, applied;
    CHECK(s, "new");

    for (i = 0; i < n; i++) {
        const msdos_pref *p = msdossession_pref(i);
        CHECK(p && p->key && p->page && p->label, "pref %d", i);
        CHECK(msdossession_pref_find(p->key) == i, "find %s", p->key);
    }
    for (m = 0; m < MSDOS_M_COUNT; m++) {
        int cpu = msdossession_pref_find("cpu"), ram = msdossession_pref_find("ram");
        int video = msdossession_pref_find("video"), lo, hi, step;
        test_set_pref(s, "machine", m);
        CHECK(msdossession_machine(s) == m, "machine %d", m);
        CHECK(msdossession_pref_choice(s, cpu, 0), "%s: no CPU", msdos_machine_name(m));
        CHECK(msdossession_pref_choice(s, video, 0), "%s: no video", msdos_machine_name(m));
        msdossession_pref_int_range(s, ram, &lo, &hi, &step);
        CHECK(lo > 0 && lo <= hi && step > 0, "%s: RAM %d..%d/%d", msdos_machine_name(m), lo, hi,
              step);
        CHECK(msdossession_pref_get_int(s, ram) >= lo && msdossession_pref_get_int(s, ram) <= hi,
              "%s: RAM default out of range", msdos_machine_name(m));
    }

    /* GLaBIOS stands in for the 5160's BIOS */
    test_set_pref(s, "machine", MSDOS_M_5160);
    CHECK(msdossession_has_bios(s) && msdossession_machine_available(s, MSDOS_M_5160), "GLaBIOS");
    test_start(s, 0);
    msdossession_set_warp(s, 1);
    CHECK(test_wait_text(s, "GLaBIOS", 30000), "GLaBIOS POST");
    CHECK(test_wait_text(s, "Port: 2F8h", 90000), "FUJINET.SYS on COM2");

    /* COM1: written to 86box.cfg and CONFIG.SYS, applied with a reset */
    test_set_pref(s, "fujinet_com", 0);
    applied = msdossession_apply_settings(s);
    CHECK(applied == MSDOS_APPLY_RESET, "apply gave %d", applied);
    CHECK(test_wait_text(s, "Port: 3F8h", 90000), "FUJINET.SYS on COM1 after the reset");

    /* the buttons */
    CHECK(msdossession_reset(s) == 0, "reset");
    CHECK(test_wait_text(s, "GLaBIOS", 30000), "POST after reset");
    CHECK(msdossession_ctrl_alt_del(s) == 0, "ctrl-alt-del");

    msdossession_stop(s);
    msdossession_free(s);
    puts("session: ok");
    return 0;
}
