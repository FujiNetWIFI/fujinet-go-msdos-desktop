/* Keys and paste: the HID tables, the on-screen layouts, and a line typed
   at FreeDOS's prompt (GLaBIOS, a plain FreeDOS floppy, no FujiNet). */
#include "session_internal.h"
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

    /* the PCjr has its own 62-key Freeboard, not the XT's keys */
    test_set_pref(s, "machine", MSDOS_M_PCJR);
    n = msdossession_keyboard_layout(s, &keys);
    CHECK(n == 62, "PCjr layout has %d keys", n);
    test_set_pref(s, "machine", MSDOS_M_5160);

    /* the game port's sources are state: they merge with no machine running */
    {
        int xpos = 0, ypos = 0;
        unsigned buttons = 0;
        const int up = MSDOS_TARGET(0, MSDOS_ACT_UP), left = MSDOS_TARGET(0, MSDOS_ACT_LEFT);
        const int btn1 = MSDOS_TARGET(0, MSDOS_ACT_B1);

        /* the window's drag and a pad: the pad's stronger axis wins, the window keeps the rest */
        msdossession_pointer_set(s, 0, 10000, -20000, 1);
        session_gamepad_analog(s, 0, 0, -30000, 5000);
        msdossession_joystick_state(s, 0, &xpos, &ypos, &buttons);
        CHECK(xpos == -30000 && ypos == -20000 && buttons == 1, "merge: strongest axis wins (%d,%d,%u)", xpos, ypos, buttons);
        session_gamepad_press(s, 0, btn1, 1);
        msdossession_pointer_set(s, 0, 10000, -20000, 0);
        CHECK(msdossession_buttons_held(s, 0) == 1, "the window letting go keeps the pad's button");
        session_gamepad_press(s, 0, btn1, 0);
        CHECK(msdossession_buttons_held(s, 0) == 0, "and the pad letting go releases it");
        msdossession_pointer_set(s, 0, 0, 0, 0);
        session_gamepad_release(s, 0);
        msdossession_joystick_state(s, 0, &xpos, &ypos, &buttons);
        CHECK(xpos == 0 && ypos == 0 && buttons == 0, "all sources released: centred");
        CHECK(msdossession_buttons_held(s, 1) == 0, "the other stick was never touched");

        /* digital pushes: full deflection, opposed pair cancels, window drag beats nothing */
        session_gamepad_press(s, 0, up, 1);
        session_gamepad_dir(s, 1, 0, MSDOS_ACT_DOWN, 1);
        msdossession_joystick_state(s, 0, NULL, &ypos, NULL);
        CHECK(ypos == 0, "opposed pushes from two pads cancel");
        session_gamepad_dir(s, 1, 0, MSDOS_ACT_DOWN, 0);
        msdossession_joystick_state(s, 0, NULL, &ypos, NULL);
        CHECK(ypos == -32768, "up alone is the stop");
        CHECK(msdossession_target_held(s, up) == 1 && msdossession_target_held(s, MSDOS_TARGET(0, MSDOS_ACT_DOWN)) == 0,
              "target_held follows the axis");
        session_gamepad_press(s, 0, up, 0);

        /* a bound key and a pad on one target: the key's release keeps the pad's hold */
        msdossession_binding_set_key(s, left, 0x0E, NULL, 0);
        CHECK(msdossession_key(s, 0x0E, 1) == 1, "a bound key is taken (no machine needed)");
        session_gamepad_press(s, 0, left, 1);
        CHECK(msdossession_key(s, 0x0E, 0) == 1, "its release is taken too");
        msdossession_joystick_state(s, 0, &xpos, NULL, NULL);
        CHECK(xpos == -32768, "the pad still holds left after the key lets go");
        session_gamepad_press(s, 0, left, 0);
        msdossession_joystick_state(s, 0, &xpos, NULL, NULL);
        CHECK(xpos == 0, "and releases it");
        msdossession_key(s, 0x0E, 1);
        msdossession_release_keyboard(s);
        msdossession_joystick_state(s, 0, &xpos, NULL, NULL);
        CHECK(xpos == 0 && msdossession_target_held(s, left) == 0, "release_keyboard lets go of bound keys");
        msdossession_pointer_set(s, 1, 0, 0, 4);
        msdossession_release_all(s);
        CHECK(msdossession_buttons_held(s, 1) == 0, "release_all lets go of the window's holds");
        msdossession_bindings_reset(s);
        CHECK(msdossession_key(s, 0x0E, 1) == 0, "an unbound key is not taken while no machine runs");
    }

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
