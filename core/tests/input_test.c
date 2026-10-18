/* Keys and paste: the HID tables, the on-screen layouts, and a line typed
   at FreeDOS's prompt (GLaBIOS, a plain FreeDOS floppy, no FujiNet). */
#include "test_util.h"

int main(void)
{
    msdossession *s = test_session_ex("input", 0, 1);
    const msdos_kbd_key *keys;
    char msg[512];
    int n, i, esc = 0;
    CHECK(s, "new");

    CHECK(msdossession_hid_from_evdev(30) == 0x04, "evdev A");
    CHECK(msdossession_hid_from_evdev(1) == 0x29, "evdev Esc");
    CHECK(msdossession_hid_from_win_scancode(0x1E, 0) == 0x04, "win A");
    CHECK(msdossession_hid_from_macos_keycode(0x00) == 0x04, "mac A");
    CHECK(msdossession_hid_name(0xE0) != NULL, "name of Left Ctrl");

    /* the XT has 83 keys, the 486 the 101-key Enhanced keyboard */
    test_set_pref(s, "machine", MSDOS_M_5160);
    n = msdossession_keyboard_layout(s, &keys);
    CHECK(n == 83, "XT layout has %d keys", n);
    for (i = 0; i < n; i++) {
        CHECK(keys[i].label && keys[i].w > 0 && keys[i].h > 0, "key %d", i);
        if (keys[i].hid == 0x29) esc = 1;
    }
    CHECK(esc, "no Esc");
    test_set_pref(s, "machine", MSDOS_M_486);
    n = msdossession_keyboard_layout(s, &keys);
    CHECK(n == 101, "Enhanced layout has %d keys", n);
    test_set_pref(s, "machine", MSDOS_M_5160);

    CHECK(msdossession_floppy_insert(s, 0, MSDOS_TEST_PLAIN_FLOPPY, 1, msg, sizeof msg) == 0,
          "insert: %s", msg);
    test_start(s, 0);
    msdossession_set_warp(s, 1);
    CHECK(test_wait_text(s, "A:\\>", 120000), "the DOS prompt");
    CHECK(msdossession_paste(s, "ECHO 6*7 IS 42\n") == 0, "paste");
    CHECK(test_wait_text(s, "\n6*7 IS 42", 30000), "the echo");
    /* keys: "VER" and Enter, typed */
    {
        static const uint32_t ver[] = { 0x19, 0x08, 0x15, 0x28 };
        for (i = 0; i < 4; i++) {
            msdossession_key(s, ver[i], 1);
            test_sleep_ms(60);
            msdossession_key(s, ver[i], 0);
            test_sleep_ms(60);
        }
    }
    CHECK(test_wait_text(s, "FreeCom version", 30000), "VER");

    msdossession_stop(s);
    msdossession_free(s);
    puts("input: ok");
    return 0;
}
