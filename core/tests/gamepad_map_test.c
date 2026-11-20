/* The gamepad layer end to end with an SDL3 virtual gamepad and NO machine
   running (the sources are state): hot-plug pickup, the default mapping onto
   the analog stick (D-pad to the stops, left stick proportional, South, East,
   West, North and the triggers as buttons 1-4), a button rebound, a key bound
   to the stick, the sources not stepping on each other (window drag, pad, key),
   Map's capture, the triggers freezing nothing, hot-unplug releasing everything
   the pad held and a reconnect getting its stick back, one-stick and no game
   port modes, and the stick assignment written to the settings, restored on a
   restart, found through a stale GUID by the name, kept through garbage and
   removed by Automatic. SKIPs (77) where SDL has no virtual joysticks or real
   pads leave no stick free. Waits are long: slow runners are slow to
   enumerate. */
#include <SDL3/SDL.h>

#include "session_internal.h"
#include "test_util.h"

#define PAD_NAME "msdostest virtual pad"
#define WAIT_STEPS 500          /* x 20 ms = 10 s */
#define HID_F 0x09

static int failures;
static int vstick;              /* the stick the virtual pad was given */

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

/* The merged buttons of the virtual pad's stick, until they are `want`. */
static void expect_buttons(msdossession *sess, unsigned want, const char *what)
{
    unsigned held = 0;
    for (int step = 0; step < WAIT_STEPS; step++) {
        held = msdossession_buttons_held(sess, vstick);
        if (held == want) {
            printf("ok: %s\n", what);
            return;
        }
        test_sleep_ms(20);
    }
    printf("FAIL: %s (buttons %02X, wanted %02X)\n", what, held, want);
    failures++;
}

/* The stick's axes by sign (-1, 0, +1 each): the exact numbers are the dead
   zone's business (gamepad_test). */
static void expect_axes(msdossession *sess, int signx, int signy, const char *what)
{
    int xpos = 0, ypos = 0;
    for (int step = 0; step < WAIT_STEPS; step++) {
        msdossession_joystick_state(sess, vstick, &xpos, &ypos, NULL);
        if (((xpos > 0) - (xpos < 0)) == signx && ((ypos > 0) - (ypos < 0)) == signy) {
            printf("ok: %s\n", what);
            return;
        }
        test_sleep_ms(20);
    }
    printf("FAIL: %s (axes %d,%d, wanted signs %d,%d)\n", what, xpos, ypos, signx, signy);
    failures++;
}

static void expect_axis_values(msdossession *sess, int want_x, int want_y, const char *what)
{
    int xpos = 0, ypos = 0;
    for (int step = 0; step < WAIT_STEPS; step++) {
        msdossession_joystick_state(sess, vstick, &xpos, &ypos, NULL);
        if (xpos == want_x && ypos == want_y) {
            printf("ok: %s\n", what);
            return;
        }
        test_sleep_ms(20);
    }
    printf("FAIL: %s (axes %d,%d, wanted %d,%d)\n", what, xpos, ypos, want_x, want_y);
    failures++;
}

static void make_paths(msdossession_paths *paths, char *cfg, size_t cfgsz, char *data, size_t datasz)
{
    test_tmpdir(cfg, cfgsz, "gmcfg");
    test_tmpdir(data, datasz, "gmdata");
    memset(paths, 0, sizeof *paths);
    paths->config_dir = cfg;
    paths->data_dir = data;
    paths->fujinet_lib = "";        /* the pad only */
}

/* A session on the directories in paths, with only the gamepad thread: no
   machine (86Box starts paused and a fatal error exits the process). */
static msdossession *session_up(const msdossession_paths *paths)
{
    msdossession *sess = msdossession_new(paths);
    if (!sess) return NULL;
    if (gamepad_start(sess) != 0) {
        printf("FAIL: gamepad_start: %s\n", msdossession_last_error(sess));
        msdossession_free(sess);
        return NULL;
    }
    return sess;
}

static void session_down(msdossession *sess)
{
    gamepad_stop(sess);
    msdossession_free(sess);
}

/* The virtual pad's slot now, or -1. */
static int pad_slot_now(msdossession *sess)
{
    for (int idx = 0; idx < msdossession_gamepad_count(sess); idx++) {
        char name[64];
        msdossession_gamepad_name(sess, idx, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            return idx;
    }
    return -1;
}

static int wait_for_pad(msdossession *sess)
{
    int idx = pad_slot_now(sess);
    for (int tries = 0; tries < WAIT_STEPS && idx < 0; tries++) {
        test_sleep_ms(20);
        idx = pad_slot_now(sess);
    }
    return idx;
}

static void describe_pad(SDL_VirtualJoystickDesc *desc)
{
    SDL_INIT_INTERFACE(desc);
    desc->type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc->naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc->nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc->name = PAD_NAME;
}

static int has_entry(msdossession *sess, const char *guid, int stick)
{
    char want[160];
    snprintf(want, sizeof want, "%s|%s=%d", guid, PAD_NAME, stick);
    return strstr(msdossession_get_str(sess, "pad_assign", ""), want) != NULL;
}

/* A stale entry (GUID guid, the pad's name) goes to a different pad of the
   same name that connects first, by the name fallback. The pad with exactly
   that GUID then connects and takes it over: the decoy goes automatic. */
static void takeover(const msdossession_paths *paths, SDL_VirtualJoystickDesc *desc,
                     const char *guid, int want)
{
    SDL_VirtualJoystickDesc other = *desc;
    SDL_JoystickID decoy, owner = 0;
    char text[300], decoy_guid[64];
    msdossession *sess = session_up(paths);

    if (!sess) { failures++; return; }
    snprintf(text, sizeof text, "%s|%s=%d", guid, PAD_NAME, want);
    msdossession_set_str(sess, "pad_assign", text);
    session_down(sess);

    other.vendor_id = (Uint16)(desc->vendor_id ^ 0x5a5a);
    other.product_id = (Uint16)(desc->product_id ^ 0xa5a5);
    decoy = SDL_AttachVirtualJoystick(&other);
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(decoy), decoy_guid, sizeof decoy_guid);
    if (!decoy || !strcmp(decoy_guid, guid)) {
        printf("skip: the takeover test needs a second virtual pad with another GUID\n");
        if (decoy) SDL_DetachVirtualJoystick(decoy);
        return;
    }
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(decoy); return; }
    {
        const int idx = wait_for_pad(sess);
        check(idx >= 0 && msdossession_gamepad_assignment(sess, idx) == want,
              "takeover: a same-name pad with another GUID gets the entry by name");
    }
    owner = SDL_AttachVirtualJoystick(desc);
    for (int tries = 0; tries < WAIT_STEPS && msdossession_gamepad_count(sess) < 2; tries++)
        test_sleep_ms(20);
    check(owner && msdossession_gamepad_count(sess) == 2, "takeover: the second pad is picked up");
    if (msdossession_gamepad_count(sess) == 2) {
        check(msdossession_gamepad_assignment(sess, 1) == want,
              "the pad with the entry's GUID takes the assignment over");
        check(msdossession_gamepad_assignment(sess, 0) == -1, "and the name-only pad is automatic");
    }
    session_down(sess);
    if (owner) SDL_DetachVirtualJoystick(owner);
    SDL_DetachVirtualJoystick(decoy);
}

/* A pad's stick assignment survives a restart. The virtual pad stays attached
   across the sessions (SDL is held open by the caller). */
static void persistence(const msdossession_paths *paths)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    msdossession *sess;
    char guid[64];
    int idx, want, auto_stick;

    describe_pad(&desc);
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("FAIL: the virtual pad would not attach again: %s\n", SDL_GetError());
        failures++;
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { session_down(sess); SDL_DetachVirtualJoystick(vid); return; }
    check(msdossession_gamepad_assignment(sess, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(msdossession_get_str(sess, "pad_assign", ""), guid) == NULL, "and has no entry");
    auto_stick = msdossession_gamepad_effective_stick(sess, idx);
    want = auto_stick == 0 ? 1 : 0;

    msdossession_gamepad_assign(sess, idx, want);
    check(msdossession_gamepad_assignment(sess, idx) == want
          && msdossession_gamepad_effective_stick(sess, idx) == want, "assigning drives the other stick");
    check(has_entry(sess, guid, want), "and writes <guid>|<name>=<stick> to pad_assign");
    {
        char path[600], line[2048];
        int found = 0;
        FILE *cfgfile;
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        msdossession_settings_flush(sess);
        cfgfile = fopen(path, "r");
        if (cfgfile) {
            while (fgets(line, sizeof line, cfgfile))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(cfgfile);
        }
        check(found, "the settings file on disk holds it");
    }

    /* restart: a new session on the same settings */
    session_down(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess);
    check(idx >= 0, "restart: the pad is picked up again");
    if (idx >= 0) {
        check(msdossession_gamepad_assignment(sess, idx) == want, "the assignment is restored on device add");
        check(msdossession_gamepad_effective_stick(sess, idx) == want, "and the pad drives that stick");
    }

    /* a stale GUID (another USB port): the name finds the entry */
    {
        char text[300];
        snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=%d", PAD_NAME, want);
        msdossession_set_str(sess, "pad_assign", text);
    }
    session_down(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess);
    check(idx >= 0 && msdossession_gamepad_assignment(sess, idx) == want,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry, and the next run starts automatic */
    msdossession_gamepad_assign(sess, idx, want);
    check(has_entry(sess, guid, want), "assigned under the pad's own GUID again");
    msdossession_gamepad_assign(sess, idx, -1);
    check(msdossession_gamepad_assignment(sess, idx) == -1, "Automatic: no explicit stick");
    check(strstr(msdossession_get_str(sess, "pad_assign", ""), PAD_NAME) == NULL,
          "and the entry is gone from pad_assign");
    session_down(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess);
    check(idx >= 0 && msdossession_gamepad_assignment(sess, idx) == -1
          && msdossession_gamepad_effective_stick(sess, idx) == auto_stick, "after a restart it is automatic again");

    /* garbage in the setting is ignored, silently */
    msdossession_set_str(sess, "pad_assign", ";;|=;zz|x=9;%%%;" "00000000000000000000000000000000|" PAD_NAME "=7");
    session_down(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess);
    check(idx >= 0 && msdossession_gamepad_assignment(sess, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");
    msdossession_gamepad_assign(sess, idx, want);
    check(has_entry(sess, guid, want), "and a new assignment still writes");
    msdossession_gamepad_assign(sess, idx, -1);

    /* a hot replug within a run restores it too */
    msdossession_gamepad_assign(sess, idx, want);
    SDL_DetachVirtualJoystick(vid);
    for (int tries = 0; tries < WAIT_STEPS && pad_slot_now(sess) >= 0; tries++)
        test_sleep_ms(20);
    check(pad_slot_now(sess) < 0, "unplugged: the pad is gone");
    vid = SDL_AttachVirtualJoystick(&desc);
    idx = vid ? wait_for_pad(sess) : -1;
    check(idx >= 0 && msdossession_gamepad_assignment(sess, idx) == want,
          "unplugged and plugged back in: the stick comes back");
    msdossession_gamepad_assign(sess, idx, -1);

    session_down(sess);
    if (vid) SDL_DetachVirtualJoystick(vid);
    takeover(paths, &desc, guid, want);
}

int main(void)
{
    char cfg[512], data[512], name[64];
    msdossession_paths paths;
    msdossession *sess;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *joy;
    int baseline, idx, joystick_pref;
    unsigned generation;

    /* held open for the whole run: a session stopping must not take the
       virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        return 1;
    make_paths(&paths, cfg, sizeof cfg, data, sizeof data);
    sess = session_up(&paths);
    if (!sess)
        return 1;

    /* Real pads may already be attached: let the first device scan find
       them, so the virtual one is known by the slot it adds. */
    test_sleep_ms(700);
    baseline = msdossession_gamepad_count(sess);

    describe_pad(&desc);
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        session_down(sess);
        return 77;
    }
    joy = SDL_OpenJoystick(vid);
    if (!joy)
        return 1;

    for (int tries = 0; tries < WAIT_STEPS && msdossession_gamepad_count(sess) <= baseline; tries++)
        test_sleep_ms(20);
    vstick = -1;
    for (idx = 0; idx < msdossession_gamepad_count(sess); idx++) {
        msdossession_gamepad_name(sess, idx, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            vstick = msdossession_gamepad_effective_stick(sess, idx);
    }
    if (vstick < 0 || vstick >= MSDOS_JOYSTICKS) {
        const int found = msdossession_gamepad_count(sess) > baseline;
        printf("%s: the virtual pad got no stick (%d; real pads attached?)\n", found ? "SKIP" : "FAIL", vstick);
        SDL_CloseJoystick(joy);
        SDL_DetachVirtualJoystick(vid);
        session_down(sess);
        return found ? 77 : 1;
    }
    printf("ok: the virtual pad was picked up as joystick %c\n", 'A' + vstick);
    check(msdossession_game_port_mode(sess) == MSDOS_PORT_TWO && msdossession_joystick_count(sess) == 2,
          "the default game port is two sticks");

    /* buttons: the default map */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect_buttons(sess, 1u, "South is button 1");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect_buttons(sess, 0, "released");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, true);
    expect_buttons(sess, 2u, "East is button 2");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
    expect_buttons(sess, 4u, "West is button 3");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, true);
    expect_buttons(sess, 8u, "North is button 4");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, true);
    expect_buttons(sess, 9u, "two buttons are OR'd");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, false);
    expect_buttons(sess, 0, "released");

    /* the triggers are buttons 3 and 4 too; holding one must not freeze the
       pad layer (it once dispatched under the pad lock) */
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect_buttons(sess, 4u, "the left trigger is button 3");
    generation = msdossession_gamepad_generation(sess);
    check(msdossession_gamepad_effective_stick(sess, 0) >= -1 && generation >= 1u,
          "with a trigger held the pad lock is free (generation and stick answer)");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect_buttons(sess, 4u, "West still holds button 3 when the trigger lets go (one target, two inputs)");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, false);
    expect_buttons(sess, 0, "released");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    expect_buttons(sess, 8u, "the right trigger is button 4");
    msdossession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    {
        int button = -1, got = 0;
        for (int step = 0; step < WAIT_STEPS && !got; step++) {
            got = msdossession_gamepad_capture_poll(sess, &button);
            if (!got) test_sleep_ms(20);
        }
        check(got && button == MSDOS_PAD_BTN_LEFT_TRIGGER, "Map capture reports a trigger");
    }
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect_buttons(sess, 0, "the trigger that armed Map did nothing, and the right one let go");

    /* the D-pad pushes the stick to the stops, opposed pushes cancel */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect_axis_values(sess, 0, -32768, "D-pad up is the stick fully up");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect_axis_values(sess, -32768, -32768, "up and left together are a diagonal");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    expect_axis_values(sess, -32768, 0, "up and down together cancel");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect_axis_values(sess, -32768, 32767, "and down is back once up lets go");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect_axis_values(sess, 0, 0, "released");

    /* the left stick is proportional: with a dead zone, and apart from the D-pad */
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    expect_axes(sess, 1, -1, "the stick right and up deflects right and up");
    {
        int xpos = 0;
        msdossession_joystick_state(sess, vstick, &xpos, NULL, NULL);
        check(xpos > 8000 && xpos < 32767, "and by its own amount, not to the stop");
    }
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect_axes(sess, -1, -1, "the D-pad pushes the stick to the stop and the strongest source wins");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect_axes(sess, 1, -1, "and gives it back to the analog stick");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect_axes(sess, 0, 0, "the stick centred: the axes are centred");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 2000);
    test_sleep_ms(200);
    expect_axes(sess, 0, 0, "a tremor inside the dead zone is nothing");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);

    /* the window and the pad are separate sources: dragging the window's
       stick while a pad holds, and letting go of either */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect_buttons(sess, 1u, "pad: button 1");
    msdossession_pointer_set(sess, vstick, 12000, -30000, 2u);
    check(msdossession_buttons_held(sess, vstick) == 3u, "the window adds its button 2 to the pad's button 1");
    {
        int xpos = 0, ypos = 0;
        msdossession_joystick_state(sess, vstick, &xpos, &ypos, NULL);
        check(xpos == 12000 && ypos == -30000, "and drives the axes while the pad's stick is centred");
    }
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    expect_axes(sess, 1, 1, "the pad's stronger deflection wins an axis, the window keeps the other");
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 0);
    msdossession_pointer_set(sess, vstick, 0, 0, 0);
    check(msdossession_buttons_held(sess, vstick) == 1u, "the window letting go does not release the pad's button");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect_buttons(sess, 0, "released");

    /* a key bound to a stick direction: taken from the PC, and its release
       leaves a pad's hold on the same stick alone */
    msdossession_binding_set_key(sess, MSDOS_TARGET(vstick, MSDOS_ACT_B2), HID_F, NULL, 0);
    check(msdossession_key(sess, HID_F, 1) == 1, "a bound key is used (and does not reach 86Box)");
    check(msdossession_buttons_held(sess, vstick) == 2u, "it holds button 2 of the stick");
    check(msdossession_key(sess, HID_F, 1) == 1 && msdossession_buttons_held(sess, vstick) == 2u,
          "an auto-repeat changes nothing");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect_buttons(sess, 3u, "with the pad's button 1");
    msdossession_binding_set_key(sess, MSDOS_TARGET(vstick, MSDOS_ACT_B2), 0, NULL, 0);
    check(msdossession_key(sess, HID_F, 0) == 1 && msdossession_buttons_held(sess, vstick) == 1u,
          "the release lets go of what the press held, even after an unbind, and keeps the pad's hold");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    msdossession_binding_set_key(sess, MSDOS_TARGET(vstick, MSDOS_ACT_RIGHT), HID_F, NULL, 0);
    msdossession_key(sess, HID_F, 1);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect_axis_values(sess, 32767, 0, "key and D-pad both push right");
    msdossession_release_keyboard(sess);
    expect_axis_values(sess, 32767, 0, "release_keyboard (main window focus loss) keeps the pad's hold");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect_axis_values(sess, 0, 0, "released");
    msdossession_bindings_reset(sess);

    /* a button rebound: West is button 1 now, and South nothing */
    msdossession_binding_set_button(sess, MSDOS_TARGET(vstick, MSDOS_ACT_B1), MSDOS_PAD_BTN_WEST, NULL, 0);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
    expect_buttons(sess, 1u, "West, bound to button 1, is button 1 and not button 3 any more");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    test_sleep_ms(150);
    expect_buttons(sess, 0, "South, which lost button 1 to it, is nothing");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);

    /* a press whose binding changes while held releases what it asserted */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
    expect_buttons(sess, 1u, "West held");
    msdossession_bindings_reset(sess);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, false);
    expect_buttons(sess, 0, "rebinding while held leaves nothing stuck");

    /* Map's capture swallows the press: it does nothing else */
    msdossession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, true);
    {
        int button = -1, got = 0;
        for (int step = 0; step < WAIT_STEPS && !got; step++) {
            got = msdossession_gamepad_capture_poll(sess, &button);
            if (!got) test_sleep_ms(20);
        }
        check(got && button == MSDOS_PAD_BTN_START, "capture reports the button pressed");
        check(msdossession_gamepad_capture_poll(sess, &button) == 0, "once");
    }
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, false);
    msdossession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    test_sleep_ms(150);
    check(msdossession_buttons_held(sess, vstick) == 0u, "while armed, South does not press button 1");
    msdossession_gamepad_capture_cancel(sess);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);

    /* one stick on the game port (4 buttons): B is not offered, a pad drives A */
    joystick_pref = msdossession_pref_find("joystick");
    msdossession_pref_set_choice(sess, joystick_pref, 2);
    check(msdossession_game_port_mode(sess) == MSDOS_PORT_ONE_4BTN && msdossession_joystick_count(sess) == 1,
          "4-button game port: one stick");
    idx = pad_slot_now(sess);
    msdossession_gamepad_assign(sess, idx, 1);
    check(msdossession_gamepad_assignment(sess, idx) == -1, "Joystick B cannot be assigned");
    check(msdossession_gamepad_effective_stick(sess, idx) == 0, "the pad drives A");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, true);
    vstick = 0;
    expect_buttons(sess, 8u, "and its buttons are the port's four");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, false);

    /* no game port: pads are inert, and held buttons are let go */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect_buttons(sess, 1u, "held before the port goes away");
    msdossession_pref_set_choice(sess, joystick_pref, 0);
    check(msdossession_joystick_fitted(sess) == 0, "no game port");
    for (int step = 0; step < WAIT_STEPS && msdossession_gamepad_effective_stick(sess, idx) != -1; step++)
        test_sleep_ms(20);
    check(msdossession_gamepad_effective_stick(sess, idx) == -1, "the pad drives nothing");
    expect_buttons(sess, 0, "and what it held was let go");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    test_sleep_ms(200);
    check(msdossession_buttons_held(sess, 0) == 0u && msdossession_buttons_held(sess, 1) == 0u,
          "a press on a pad with no game port holds nothing");
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    msdossession_pref_set_choice(sess, joystick_pref, 1);
    for (int step = 0; step < WAIT_STEPS && msdossession_gamepad_effective_stick(sess, idx) < 0; step++)
        test_sleep_ms(20);
    vstick = msdossession_gamepad_effective_stick(sess, idx);
    check(vstick >= 0, "two sticks again: the pad is back on one");
    test_sleep_ms(300);     /* let the pad thread see the move before pressing: it lets go of everything when a pad changes stick */

    /* hot-unplug while held: the stick, a button and a direction are
       released, not left holding forever */
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_NORTH, true);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, 30000);
    expect_axes(sess, 1, 1, "held before the unplug");
    expect_buttons(sess, 9u, "with two buttons");
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(vid);
    for (int step = 0; step < WAIT_STEPS && msdossession_gamepad_count(sess) != baseline; step++)
        test_sleep_ms(20);
    check(msdossession_gamepad_count(sess) == baseline, "the unplugged pad is gone");
    expect_buttons(sess, 0, "and its buttons are released");
    expect_axes(sess, 0, 0, "and its stick is centred");

    /* reconnect: it is picked up again and drives the same stick */
    vid = SDL_AttachVirtualJoystick(&desc);
    joy = vid ? SDL_OpenJoystick(vid) : NULL;
    idx = joy ? wait_for_pad(sess) : -1;
    check(idx >= 0, "plugged back in: picked up");
    if (idx >= 0) {
        check(msdossession_gamepad_effective_stick(sess, idx) == vstick, "on the same stick");
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, true);
        expect_buttons(sess, 2u, "and working");
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, false);
        expect_buttons(sess, 0, "released");
    }
    if (joy) SDL_CloseJoystick(joy);
    if (vid) SDL_DetachVirtualJoystick(vid);
    msdossession_bindings_reset(sess);

    session_down(sess);

    persistence(&paths);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
