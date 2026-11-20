/* The remappable table, pure (no machine started): the defaults (the pad's
   D-pad and buttons, no keys), steal-and-describe, the stick scoping of pad
   inputs, persistence through the settings store, the diff format, names, and
   that the table is the app's and not a machine profile's. */
#include "test_util.h"

#define HID_A 0x04
#define HID_W 0x1A
#define HID_SPACE 0x2C
#define HID_UP 0x52

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

int main(void)
{
    char base[512], cfg[600], data[600], stolen[128];
    msdossession_paths paths;
    msdossession *sess;
    msdos_binding bind;
    int target, other, clash = 0, stick;

    test_tmpdir(base, sizeof base, "bind");
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    test_mkdir(data);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    sess = msdossession_new(&paths);
    CHECK(sess, "new");

    check(MSDOS_TARGET_COUNT == 16 && MSDOS_TARGET(0, MSDOS_ACT_UP) == 0
          && MSDOS_TARGET(1, MSDOS_ACT_UP) == 8 && MSDOS_TARGET(1, MSDOS_ACT_B4) == 15,
          "16 targets: two sticks of Up Down Left Right and buttons 1-4");

    /* defaults: no keys at all, the pad as it always was */
    for (target = 0; target < MSDOS_TARGET_COUNT; target++)
        if (msdossession_binding_get(sess, target).key) clash++;
    check(clash == 0, "there are no default keys (the PC owns its keyboard)");
    for (stick = 0; stick < MSDOS_JOYSTICKS; stick++) {
        check(msdossession_binding_get(sess, MSDOS_TARGET(stick, MSDOS_ACT_B1)).button == MSDOS_PAD_BTN_SOUTH
              && msdossession_binding_get(sess, MSDOS_TARGET(stick, MSDOS_ACT_B2)).button == MSDOS_PAD_BTN_EAST
              && msdossession_binding_get(sess, MSDOS_TARGET(stick, MSDOS_ACT_B3)).button == MSDOS_PAD_BTN_WEST
              && msdossession_binding_get(sess, MSDOS_TARGET(stick, MSDOS_ACT_B4)).button == MSDOS_PAD_BTN_NORTH,
              "South, East, West, North are buttons 1-4");
        check(msdossession_binding_get(sess, MSDOS_TARGET(stick, MSDOS_ACT_B3)).button2 == MSDOS_PAD_BTN_LEFT_TRIGGER
              && msdossession_binding_get(sess, MSDOS_TARGET(stick, MSDOS_ACT_B4)).button2 == MSDOS_PAD_BTN_RIGHT_TRIGGER,
              "the triggers are also buttons 3 and 4");
        check(msdossession_target_for_button(sess, stick, MSDOS_PAD_BTN_LEFT_TRIGGER) == MSDOS_TARGET(stick, MSDOS_ACT_B3)
              && msdossession_target_for_button(sess, stick, MSDOS_PAD_BTN_RIGHT_TRIGGER) == MSDOS_TARGET(stick, MSDOS_ACT_B4)
              && msdossession_target_for_button(sess, stick, MSDOS_PAD_BTN_DPAD_UP) == MSDOS_TARGET(stick, MSDOS_ACT_UP)
              && msdossession_target_for_button(sess, stick, MSDOS_PAD_BTN_DPAD_RIGHT) == MSDOS_TARGET(stick, MSDOS_ACT_RIGHT),
              "the D-pad is the four directions; lookup is by stick");
        check(msdossession_target_for_button(sess, stick, MSDOS_PAD_BTN_START) == -1
              && msdossession_target_for_button(sess, stick, MSDOS_PAD_BTN_NONE) == -1,
              "Start and 'none' are nothing");
    }
    check(msdossession_target_for_button(sess, 0, MSDOS_PAD_BTN_RAW_BASE) == MSDOS_TARGET(0, MSDOS_ACT_B1)
          && msdossession_target_for_button(sess, 1, MSDOS_PAD_BTN_RAW_BASE + 3) == MSDOS_TARGET(1, MSDOS_ACT_B4)
          && msdossession_target_for_button(sess, 0, MSDOS_PAD_BTN_RAW_BASE + 4) == -1,
          "an unmapped adapter's first four buttons are buttons 1-4");
    check(msdossession_target_for_key(sess, HID_A) == -1, "no key is bound");

    /* a key: steal one target's for another */
    msdossession_binding_set_key(sess, MSDOS_TARGET(0, MSDOS_ACT_LEFT), HID_A, stolen, sizeof stolen);
    check(stolen[0] == '\0' && msdossession_target_for_key(sess, HID_A) == MSDOS_TARGET(0, MSDOS_ACT_LEFT),
          "binding a free key takes nothing from anyone");
    msdossession_binding_set_key(sess, MSDOS_TARGET(1, MSDOS_ACT_B1), HID_A, stolen, sizeof stolen);
    check(!strcmp(stolen, "Joystick A: Left") && msdossession_target_for_key(sess, HID_A) == MSDOS_TARGET(1, MSDOS_ACT_B1)
          && msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_LEFT)).key == 0,
          "binding it again steals it, and names the old holder");
    msdossession_binding_set_key(sess, MSDOS_TARGET(0, MSDOS_ACT_B1), 0xE7 + 1, stolen, sizeof stolen);
    check(msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_B1)).key == 0, "a usage past the table is refused");
    msdossession_binding_set_key(sess, MSDOS_TARGET(0, MSDOS_ACT_B1), HID_SPACE, NULL, 0);
    msdossession_binding_set_key(sess, MSDOS_TARGET(0, MSDOS_ACT_UP), HID_UP, NULL, 0);
    check(msdossession_target_for_key(sess, HID_SPACE) == MSDOS_TARGET(0, MSDOS_ACT_B1)
          && msdossession_target_for_key(sess, HID_UP) == MSDOS_TARGET(0, MSDOS_ACT_UP),
          "any key can be bound: Space, an arrow");

    /* pad inputs: unique within a stick, free between sticks */
    msdossession_binding_set_button(sess, MSDOS_TARGET(0, MSDOS_ACT_B2), MSDOS_PAD_BTN_SOUTH, stolen, sizeof stolen);
    check(!strcmp(stolen, "Joystick A: Button 1") && msdossession_target_for_button(sess, 0, MSDOS_PAD_BTN_SOUTH) == MSDOS_TARGET(0, MSDOS_ACT_B2)
          && msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_B1)).button == MSDOS_PAD_BTN_NONE,
          "South moved to button 2 steals it from button 1 on that stick");
    check(msdossession_target_for_button(sess, 1, MSDOS_PAD_BTN_SOUTH) == MSDOS_TARGET(1, MSDOS_ACT_B1),
          "and Joystick B keeps South for its button 1: the sticks are separate scopes");
    check(msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_B2)).button2 == MSDOS_PAD_BTN_NONE,
          "rebinding a button replaces both inputs of a target");
    msdossession_binding_set_button(sess, MSDOS_TARGET(0, MSDOS_ACT_B1), MSDOS_PAD_BTN_LEFT_TRIGGER, stolen, sizeof stolen);
    bind = msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_B3));
    check(!strcmp(stolen, "Joystick A: Button 3") && bind.button == MSDOS_PAD_BTN_WEST && bind.button2 == MSDOS_PAD_BTN_NONE,
          "taking a trigger leaves the target its first input");
    msdossession_binding_set_button(sess, MSDOS_TARGET(0, MSDOS_ACT_B3), MSDOS_PAD_BTN_START, NULL, 0);
    check(msdossession_target_for_button(sess, 0, MSDOS_PAD_BTN_START) == MSDOS_TARGET(0, MSDOS_ACT_B3)
          && msdossession_target_for_button(sess, 0, MSDOS_PAD_BTN_WEST) == -1,
          "a raw or named input binds");
    msdossession_binding_set_button(sess, MSDOS_TARGET(0, MSDOS_ACT_B4), MSDOS_PAD_HAT_BASE + 2, NULL, 0);
    check(msdossession_target_for_button(sess, 0, MSDOS_PAD_HAT_BASE + 2) == MSDOS_TARGET(0, MSDOS_ACT_B4),
          "so does a hat direction");
    msdossession_binding_set_button(sess, MSDOS_TARGET(0, MSDOS_ACT_B4), 17, NULL, 0);
    check(msdossession_target_for_button(sess, 0, 17) == -1, "a number outside every band is refused");

    /* nothing is double-bound after all that */
    for (target = 0; target < MSDOS_TARGET_COUNT; target++) {
        const msdos_binding first = msdossession_binding_get(sess, target);
        for (other = target + 1; other < MSDOS_TARGET_COUNT; other++) {
            const msdos_binding second = msdossession_binding_get(sess, other);
            if (first.key && first.key == second.key) clash++;
            if (target / MSDOS_ACT_COUNT == other / MSDOS_ACT_COUNT)
                if ((first.button != MSDOS_PAD_BTN_NONE && (first.button == second.button || first.button == second.button2))
                    || (first.button2 != MSDOS_PAD_BTN_NONE && (first.button2 == second.button || first.button2 == second.button2)))
                    clash++;
        }
    }
    check(clash == 0, "no key or pad input drives two targets");

    /* clear and the persisted diff */
    msdossession_binding_clear(sess, MSDOS_TARGET(0, MSDOS_ACT_B3));
    bind = msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_B3));
    check(bind.key == 0 && bind.button == MSDOS_PAD_BTN_NONE && bind.button2 == MSDOS_PAD_BTN_NONE, "Clear unbinds a target");
    check(strstr(msdossession_get_str(sess, "bindings", ""), "6.b:-1") != NULL, "and persists as a diff entry");

    msdossession_free(sess);
    sess = msdossession_new(&paths);
    CHECK(sess, "new again");
    check(msdossession_target_for_key(sess, HID_SPACE) == MSDOS_TARGET(0, MSDOS_ACT_B1)
          && msdossession_target_for_key(sess, HID_A) == MSDOS_TARGET(1, MSDOS_ACT_B1)
          && msdossession_binding_get(sess, MSDOS_TARGET(0, MSDOS_ACT_B3)).button == MSDOS_PAD_BTN_NONE
          && msdossession_target_for_button(sess, 0, MSDOS_PAD_BTN_SOUTH) == MSDOS_TARGET(0, MSDOS_ACT_B2),
          "the table persisted through a new session");

    /* it is the app's: switching machines changes none of it */
    msdossession_set_int(sess, "machine", MSDOS_M_PCJR);
    check(msdossession_target_for_key(sess, HID_SPACE) == MSDOS_TARGET(0, MSDOS_ACT_B1),
          "the bindings do not follow the machine");
    msdossession_set_int(sess, "machine", MSDOS_M_5160);
    check(strstr(msdossession_get_str(sess, "bindings", ""), "k:") != NULL, "and sit in one plain 'bindings' setting");

    /* garbage in the setting is ignored entry by entry */
    msdossession_set_str(sess, "bindings", "zz 99.k:4 3.k:99999 3.b:500 4.x:1 0.k:4 5.b:-1");
    msdossession_free(sess);
    sess = msdossession_new(&paths);
    check(msdossession_target_for_key(sess, 4) == 0 && msdossession_binding_get(sess, 5).button == MSDOS_PAD_BTN_NONE
          && msdossession_binding_get(sess, 3).button == MSDOS_PAD_BTN_DPAD_RIGHT,
          "malformed or out-of-range entries are skipped, the rest apply");

    /* reset */
    msdossession_bindings_reset(sess);
    check(msdossession_target_for_key(sess, 4) == -1
          && msdossession_binding_get(sess, 5).button == MSDOS_PAD_BTN_EAST
          && !strcmp(msdossession_get_str(sess, "bindings", "x"), ""),
          "Defaults restores every target and persists nothing");
    msdossession_binding_set_key(sess, 0, HID_W, NULL, 0);
    msdossession_reset_controls(sess);
    check(msdossession_target_for_key(sess, HID_W) == -1, "Reset Controls does the same");

    /* show bindings */
    check(msdossession_controls_show_bindings(sess) == 0, "bindings are hidden by default");
    msdossession_set_controls_show_bindings(sess, 1);
    check(msdossession_controls_show_bindings(sess) == 1
          && msdossession_get_int(sess, "controllers_show_bindings", 0) == 1, "Show bindings persists");

    /* names */
    check(!strcmp(msdos_target_name(MSDOS_TARGET(0, MSDOS_ACT_UP)), "Joystick A: Up")
          && !strcmp(msdos_target_name(MSDOS_TARGET(1, MSDOS_ACT_B4)), "Joystick B: Button 4")
          && !strcmp(msdos_target_short_name(MSDOS_TARGET(1, MSDOS_ACT_B2)), "Button 2")
          && !strcmp(msdos_target_name(99), "?"),
          "target names");
    check(!strcmp(msdos_pad_button_name(MSDOS_PAD_BTN_DPAD_LEFT), "D-pad Left")
          && !strcmp(msdos_pad_button_name(MSDOS_PAD_BTN_RAW_BASE + 4), "Button 5")
          && !strcmp(msdos_pad_button_name(MSDOS_PAD_HAT_BASE + 9), "Hat 1 Up-Right")
          && !strcmp(msdos_pad_button_name(MSDOS_PAD_BTN_NONE), ""),
          "pad input names");
    for (target = 0; target < MSDOS_TARGET_COUNT; target++)
        if (!msdos_target_name(target)[0]) clash++;
    check(clash == 0, "every target has a name");

    msdossession_free(sess);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
