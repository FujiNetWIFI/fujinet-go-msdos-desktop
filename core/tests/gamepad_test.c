/* The gamepad layer's pure helpers (no hardware), and that the SDL gamepad
   subsystem starts and stops cleanly with nothing attached, with no machine. */
#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "session_internal.h"
#include "test_util.h"

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

int main(void)
{
    int assign[4], axis_x = 99, axis_y = 99;
    msdossession *sess;

    /* the analog stick, with a radial dead zone rescaled */
    msdos_axes_from_stick(0.0f, 0.0f, &axis_x, &axis_y);
    check(axis_x == 0 && axis_y == 0, "a centred stick is centre");
    msdos_axes_from_stick(0.08f, -0.08f, &axis_x, &axis_y);
    check(axis_x == 0 && axis_y == 0, "a tremor inside the dead zone is nothing");
    msdos_axes_from_stick(1.0f, 0.0f, &axis_x, &axis_y);
    check(axis_x == 32767 && axis_y == 0, "full right is the stop");
    msdos_axes_from_stick(-1.0f, 0.0f, &axis_x, &axis_y);
    check(axis_x == -32767 || axis_x == -32768, "full left is the other stop");
    msdos_axes_from_stick(0.0f, 1.0f, &axis_x, &axis_y);
    check(axis_x == 0 && axis_y == 32767, "full down is +y (SDL's convention)");
    msdos_axes_from_stick(1.0f, 1.0f, &axis_x, &axis_y);
    check(axis_x > 20000 && axis_x < 32767 && axis_x == axis_y, "a corner is clamped to the circle");
    msdos_axes_from_stick(0.5f, 0.0f, &axis_x, &axis_y);
    check(axis_x > 12000 && axis_x < 20000 && axis_y == 0, "half deflection is part way, past the dead zone");
    msdos_axes_from_stick(0.13f, 0.0f, &axis_x, &axis_y);
    check(axis_x >= 0 && axis_x < 1000, "just past the dead zone starts from zero, not a jump");

    /* the hat as switches */
#define ACTBIT(act) (1u << (act))
    check(msdos_acts_from_dir(MSDOS_DIR_UP) == ACTBIT(MSDOS_ACT_UP)
          && msdos_acts_from_dir(MSDOS_DIR_DOWN) == ACTBIT(MSDOS_ACT_DOWN)
          && msdos_acts_from_dir(MSDOS_DIR_LEFT) == ACTBIT(MSDOS_ACT_LEFT)
          && msdos_acts_from_dir(MSDOS_DIR_RIGHT) == ACTBIT(MSDOS_ACT_RIGHT), "four directions");
    check(msdos_acts_from_dir(MSDOS_DIR_UP | MSDOS_DIR_LEFT) == (ACTBIT(MSDOS_ACT_UP) | ACTBIT(MSDOS_ACT_LEFT)),
          "a diagonal is two");
    check(msdos_acts_from_dir(MSDOS_DIR_UP | MSDOS_DIR_DOWN) == 0 && msdos_acts_from_dir(0) == 0
          && msdos_acts_from_dir(MSDOS_DIR_UP | MSDOS_DIR_DOWN | MSDOS_DIR_LEFT) == ACTBIT(MSDOS_ACT_LEFT),
          "opposites cancel and a cancelled axis leaves the other alone");

    /* stick assignment */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(msdos_pad_for_stick(assign, 2, 0) == 0 && msdos_pad_for_stick(assign, 2, 1) == 1,
          "connection order: first pad A, second pad B");
    check(msdos_pad_for_stick(assign, 1, 1) == -1, "one pad leaves B alone");
    assign[0] = 1; assign[1] = -1;
    check(msdos_pad_for_stick(assign, 2, 1) == 0 && msdos_pad_for_stick(assign, 2, 0) == 1,
          "an explicit assignment wins and the other pad fills the gap");
    assign[0] = 1; assign[1] = 1;
    check(msdos_pad_for_stick(assign, 2, 1) == 0 && msdos_pad_for_stick(assign, 2, 0) == -1,
          "two pads on one stick: the first wins, the other stick stays empty");

    /* SDL button tables round-trip */
    {
        int button, pass = 1;
        for (button = 0; button < MSDOS_PAD_BTN_LEFT_TRIGGER; button++) {
            const int sdl = msdos_sdl_button_from_pad(button);
            if (sdl < 0 || msdos_pad_button_from_sdl((uint8_t)sdl) != button) pass = 0;
        }
        check(pass, "every named button round-trips through the SDL tables");
        check(msdos_pad_button_from_sdl(200) == MSDOS_PAD_BTN_NONE, "an unknown SDL button is NONE");
    }

    /* the subsystem with no hardware: starts, reports pads, stops */
    sess = test_session_ex("gpad", 0, 0);
    CHECK(sess, "new");
    check(gamepad_start(sess) == 0, "the gamepad thread starts with no machine");
    check(msdossession_gamepad_count(sess) >= 0, "gamepad count answers");
    check(msdossession_gamepad_effective_stick(sess, 99) == -1, "an absent pad drives nothing");
    msdossession_gamepad_capture_begin(sess);
    check(msdossession_gamepad_capture_poll(sess, NULL) == 0, "nothing captured with no pad");
    msdossession_gamepad_capture_cancel(sess);
    gamepad_stop(sess);
    msdossession_free(sess);
    check(1, "and stops cleanly");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
