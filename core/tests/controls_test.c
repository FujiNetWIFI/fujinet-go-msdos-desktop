/* The Controllers window's layouts, pure (no machine started): the three
   keyboard skins (key counts, every key inside the case, no two keys on one
   spot, colours, the PCjr's 62 keys and that every one of them reaches a
   scan code the PCjr keyboard accepts), and the two Kraft sticks (parts,
   which are live in each game port mode, hit tests, the drag vector). */
#include <stdint.h>
#include <stdbool.h>

#include <86box/86box.h>
#include <86box/keyboard.h>

#include "test_util.h"

extern const scancode scancode_pcjr[512];

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static int keycaps(int machine, int *distinct)
{
    int count, idx, caps = 0, last = -1, seen = 0;
    const msdos_skin_item *item = msdossession_keyboard_skin_layout(machine, &count, NULL, NULL);
    unsigned char marks[256] = { 0 };
    for (idx = 0; idx < count; idx++)
        if (item[idx].kind == MSDOS_SKIN_KEYCAP) {
            caps++;
            if (item[idx].key >= 0 && item[idx].key < 256 && !marks[item[idx].key]) {
                marks[item[idx].key] = 1;
                seen++;
            }
            last = idx;
        }
    (void)last;
    if (distinct) *distinct = seen;
    return caps;
}

/* every key sits inside the case, and no two different keys overlap */
static int geometry_ok(int machine, char *why, size_t whysz)
{
    int count, one, two;
    float width, height;
    const msdos_skin_item *item = msdossession_keyboard_skin_layout(machine, &count, &width, &height);
    for (one = 0; one < count; one++) {
        if (item[one].x < -0.001f || item[one].y < -0.001f || item[one].x + item[one].w > width + 0.001f
            || item[one].y + item[one].h > height + 0.001f) {
            snprintf(why, whysz, "item %d (%s) leaves the case", one, item[one].label ? item[one].label : "");
            return 0;
        }
        if (item[one].kind != MSDOS_SKIN_KEYCAP) continue;
        for (two = one + 1; two < count; two++) {
            if (item[two].kind != MSDOS_SKIN_KEYCAP || item[two].key == item[one].key) continue;
            if (item[one].x < item[two].x + item[two].w - 0.01f && item[two].x < item[one].x + item[one].w - 0.01f
                && item[one].y < item[two].y + item[two].h - 0.01f && item[two].y < item[one].y + item[one].h - 0.01f) {
                snprintf(why, whysz, "%s and %s overlap", item[one].label, item[two].label);
                return 0;
            }
        }
    }
    return 1;
}

static int count_kind(int machine, int kind)
{
    int count, idx, found = 0;
    const msdos_skin_item *item = msdossession_keyboard_skin_layout(machine, &count, NULL, NULL);
    for (idx = 0; idx < count; idx++)
        if (item[idx].kind == kind) found++;
    return found;
}

static const msdos_skin_item *skin_key(int machine, uint32_t hid)
{
    int count, idx;
    const msdos_kbd_key *keys;
    const msdos_skin_item *item = msdossession_keyboard_skin_layout(machine, &count, NULL, NULL);
    msdossession_keyboard_layout_for_machine(machine, &keys);
    for (idx = 0; idx < count; idx++)
        if (item[idx].kind == MSDOS_SKIN_KEYCAP && keys[item[idx].key].hid == hid)
            return &item[idx];
    return NULL;
}

int main(void)
{
    char why[160];
    int count, distinct, idx, pos, machine, bad;
    float width, height;
    const msdos_skin_item *item;
    const msdos_kbd_key *keys;
    const msdos_control *ctl;

    /* ---- the keyboards */
    check(keycaps(MSDOS_M_5160, &distinct) == 83 && distinct == 83, "the Model F XT has 83 keys");
    check(keycaps(MSDOS_M_5150, NULL) == 83, "the 5150 has the same");
    check(keycaps(MSDOS_M_PCJR, &distinct) == 63 && distinct == 62,
          "the PCjr Freeboard has 62 keys (its Enter is drawn in two pieces)");
    check(keycaps(MSDOS_M_486, &distinct) == 101 && distinct == 101, "the Model M has 101 keys");
    check(keycaps(MSDOS_M_286, NULL) == 101 && keycaps(MSDOS_M_PENTIUM, NULL) == 101, "so does every AT");
    check(msdossession_keyboard_skin_layout(-1, &count, NULL, NULL) == NULL && count == 0
          && msdossession_keyboard_skin_layout(MSDOS_M_COUNT, &count, NULL, NULL) == NULL,
          "an unknown machine has no skin");
    check(msdossession_keyboard_skin_hit(-1, 1.0f, 1.0f) == -1, "and nothing to hit");

    for (machine = 0; machine < MSDOS_M_COUNT; machine++) {
        why[0] = 0;
        item = msdossession_keyboard_skin_layout(machine, &count, &width, &height);
        check(item && count > 0 && width > 10.0f && height > 4.0f && item[0].kind == MSDOS_SKIN_BODY
              && !strcmp(item[0].label, "Case") && item[0].w == width && item[0].h == height,
              msdos_machine_name(machine));
        check(geometry_ok(machine, why, sizeof why), why[0] ? why : "the keys fit the case and clear each other");
        msdossession_keyboard_layout_for_machine(machine, &keys);
        bad = 0;
        for (idx = 0; idx < count; idx++)
            if (item[idx].kind == MSDOS_SKIN_KEYCAP && (item[idx].key < 0
                || item[idx].key >= msdossession_keyboard_layout_for_machine(machine, NULL)
                || strcmp(item[idx].label, keys[item[idx].key].label)))
                bad++;
        check(bad == 0, "each key names its index in the layout and carries its legend");
    }


    /* every legend is American: US QWERTY (Y beside T on the top letter row, Z on
       the bottom one), US shifted symbols, ASCII only apart from the arrows */
    for (machine = 0; machine < MSDOS_M_COUNT; machine++) {
        const msdos_skin_item *caps = msdossession_keyboard_skin_layout(machine, &count, NULL, NULL);
        const msdos_skin_item *tee = skin_key(machine, 0x17), *why = skin_key(machine, 0x1C);
        const msdos_skin_item *aye = skin_key(machine, 0x04), *zed = skin_key(machine, 0x1D);
        const msdos_skin_item *ess = skin_key(machine, 0x16);
        int foreign = 0;
        for (idx = 0; idx < count; idx++) {
            const char *texts[2] = { caps[idx].label, caps[idx].shifted };
            for (pos = 0; pos < 2; pos++)
                for (const unsigned char *chr = (const unsigned char *)(texts[pos] ? texts[pos] : ""); *chr; chr++) {
                    if (*chr >= 0x80) {
                        /* only the arrows U+2190..U+2193 */
                        if (!(chr[0] == 0xE2 && chr[1] == 0x86 && chr[2] >= 0x90 && chr[2] <= 0x93)) foreign++;
                        chr += 2;
                    }
                }
        }
        check(foreign == 0, "legends are ASCII or arrows: no umlauts, no section sign");
        check(tee && why && aye && zed && ess && !strcmp(tee->label, "T") && !strcmp(why->label, "Y")
              && !strcmp(zed->label, "Z") && why->y == tee->y && why->x > tee->x && zed->y > aye->y
              && zed->y > why->y && !strcmp(skin_key(machine, 0x1D)->label, "Z"),
              "QWERTY: Y follows T on the top letter row, Z is on the bottom row (not QWERTZ)");
    }
    {
        const msdos_skin_item *grave = skin_key(MSDOS_M_486, 0x35), *bslash = skin_key(MSDOS_M_486, 0x31);
        check(grave && !strcmp(grave->label, "`") && grave->shifted && !strcmp(grave->shifted, "~")
              && bslash && !strcmp(bslash->label, "\\") && bslash->shifted && !strcmp(bslash->shifted, "|"),
              "the Model M has US ` ~ and \\ | keys");
    }

    /* colours and the classes of the photographed keyboards */
    item = skin_key(MSDOS_M_5160, 0x29);
    check(item && item->cap_class == MSDOS_CAP_MODIFIER && item->fill_rgb == MSDOS_XT_MODIFIER_RGB, "XT: Esc is a grey-green cap");
    item = skin_key(MSDOS_M_5160, 0x04);
    check(item && item->cap_class == MSDOS_CAP_LIGHT && item->fill_rgb == MSDOS_XT_LIGHT_RGB, "XT: A is a cream cap");
    item = skin_key(MSDOS_M_5160, 0x28);
    check(item && item->fill_rgb == MSDOS_XT_LIGHT_RGB, "XT: Enter is cream");
    item = skin_key(MSDOS_M_5160, 0x3A);
    check(item && item->cap_class == MSDOS_CAP_MODIFIER, "XT: F1 is grey-green");
    item = skin_key(MSDOS_M_5160, 0x57);
    check(item && item->cap_class == MSDOS_CAP_MODIFIER, "XT: keypad + is grey-green");
    item = skin_key(MSDOS_M_5160, 0x5F);
    check(item && item->cap_class == MSDOS_CAP_LIGHT, "XT: keypad 7 is cream");
    item = skin_key(MSDOS_M_486, 0x2B);
    check(item && item->fill_rgb == MSDOS_MM_MODIFIER_RGB, "Model M: Tab is grey");
    item = skin_key(MSDOS_M_486, 0x59);
    check(item && item->fill_rgb == MSDOS_MM_LIGHT_RGB, "Model M: keypad 1 is light");
    item = skin_key(MSDOS_M_486, 0x57);
    check(item && item->fill_rgb == MSDOS_MM_MODIFIER_RGB, "Model M: keypad + is grey");
    item = skin_key(MSDOS_M_PCJR, 0x14);
    check(item && item->cap_class == MSDOS_CAP_CHICLET && item->fill_rgb == MSDOS_PCJR_KEY_RGB, "PCjr: every key is one pale chiclet colour");
    item = skin_key(MSDOS_M_PCJR, 0x1E);
    check(item && item->fn_legend && !strcmp(item->fn_legend, "F1") && item->alt_rgb == MSDOS_PCJR_FN_RGB,
          "PCjr: 1 carries the green F1 legend");
    item = skin_key(MSDOS_M_PCJR, 0x27);
    check(item && item->fn_legend && !strcmp(item->fn_legend, "F10"), "PCjr: 0 is F10");
    item = skin_key(MSDOS_M_PCJR, 0x52);
    check(item && item->fn_legend && !strcmp(item->fn_legend, "Home"), "PCjr: Up carries Home");
    check(count_kind(MSDOS_M_486, MSDOS_SKIN_LED) == 3 && count_kind(MSDOS_M_5160, MSDOS_SKIN_LED) == 0
          && count_kind(MSDOS_M_PCJR, MSDOS_SKIN_LED) == 0, "only the Model M has lock lights");
    item = msdossession_keyboard_skin_layout(MSDOS_M_486, &count, NULL, NULL);
    for (idx = 0, bad = 0; idx < count; idx++)
        if (item[idx].kind == MSDOS_SKIN_LED && (item[idx].fill_rgb != MSDOS_MM_LED_OFF_RGB
            || item[idx].alt_rgb != MSDOS_MM_LED_ON_RGB || item[idx].key < 0 || item[idx].key > 2))
            bad++;
    check(bad == 0, "the lights have off and on colours and a lock bit");
    {
        /* the two Enter pieces are one key */
        int first = -1, second = -1;
        item = msdossession_keyboard_skin_layout(MSDOS_M_PCJR, &count, NULL, NULL);
        for (idx = 0; idx < count; idx++) {
            if (item[idx].shape == MSDOS_SHAPE_JOIN_BELOW) first = idx;
            if (item[idx].shape == MSDOS_SHAPE_JOIN_ABOVE) second = idx;
        }
        check(first >= 0 && second >= 0 && item[first].key == item[second].key
              && item[second].x < item[first].x && item[second].y == item[first].y + item[first].h,
              "the PCjr's L-shaped Enter: two pieces of one key");
        check(msdossession_keyboard_skin_hit(MSDOS_M_PCJR, item[second].x + 0.1f, item[second].y + 0.5f) == second
              && msdossession_keyboard_skin_hit(MSDOS_M_PCJR, item[first].x + 0.2f, item[first].y + 0.5f) == first,
              "and a click on either piece hits it");
    }

    /* hit tests */
    item = skin_key(MSDOS_M_5160, 0x04);
    idx = msdossession_keyboard_skin_hit(MSDOS_M_5160, item->x + item->w * 0.5f, item->y + item->h * 0.5f);
    check(idx >= 0 && msdossession_keyboard_skin_layout(MSDOS_M_5160, &count, NULL, NULL)[idx].key == item->key,
          "a click in the middle of a cap hits that key");
    check(msdossession_keyboard_skin_hit(MSDOS_M_5160, -1.0f, -1.0f) == -1
          && msdossession_keyboard_skin_hit(MSDOS_M_5160, 0.1f, 0.1f) == -1, "the bare case hits nothing");

    /* ---- the PCjr's keys and the PCjr keyboard's scan codes */
    count = msdossession_keyboard_layout_for_machine(MSDOS_M_PCJR, &keys);
    check(count == 62, "the PCjr layout has 62 keys");
    for (idx = 0, bad = 0; idx < count; idx++) {
        const unsigned scan = msdossession_hid_to_scancode(keys[idx].hid);
        if (!scan || scan >= 512 || scancode_pcjr[scan].mk[0] == 0 || scancode_pcjr[scan].brk[0] == 0) {
            printf("  (%s, usage 0x%02X, scan 0x%X: the PCjr keyboard does not take it)\n", keys[idx].label,
                   (unsigned)keys[idx].hid, scan);
            bad++;
        }
        for (pos = idx + 1; pos < count; pos++)
            if (keys[pos].hid == keys[idx].hid) { printf("  (usage 0x%02X twice)\n", (unsigned)keys[idx].hid); bad++; }
    }
    check(bad == 0, "every Freeboard key reaches a scan code scancode_pcjr accepts, and none is twice");
    for (idx = 0, bad = 0; idx < count; idx++)
        if (keys[idx].hid == 0xE6) {
            bad = msdossession_hid_to_scancode(0xE6) == 0x138 && scancode_pcjr[0x138].mk[0] == 0x54;
            break;
        }
    check(bad == 1, "Fn sends Right Alt, which the PCjr keyboard makes its Fn scan code (0x54)");
    for (machine = 0; machine < MSDOS_M_COUNT; machine++) {
        count = msdossession_keyboard_layout_for_machine(machine, &keys);
        for (idx = 0, bad = 0; idx < count; idx++)
            if (!msdossession_hid_to_scancode(keys[idx].hid)) bad++;
        check(bad == 0, "every key of the layout has a scan code");
    }

    /* ---- the Kraft sticks */
    ctl = msdossession_controls_layout(MSDOS_PORT_TWO, &count, &width, &height);
    check(ctl && count > 20 && width == MSDOS_CONTROLS_W && height == MSDOS_CONTROLS_H, "two sticks: the extent");
    for (machine = MSDOS_PORT_NONE; machine <= MSDOS_PORT_ONE_4BTN; machine++) {
        int total;
        float cw, ch;
        const msdos_control *view = msdossession_controls_layout(machine, &total, &cw, &ch);
        check(total == count && cw == width && ch == height, "every mode has the same parts and extent");
        for (idx = 0, bad = 0; idx < total; idx++)
            if (view[idx].x < -0.001f || view[idx].y < -0.001f || view[idx].x + view[idx].w > cw + 0.001f
                || view[idx].y + view[idx].h > ch + 0.001f)
                bad++;
        check(bad == 0, "and every part is inside it");
    }
    {
        int found[2][8] = {{ 0 }}, enabled[3][2][4] = {{{ 0 }}}, trims = 0, sticks = 0;
        for (machine = MSDOS_PORT_NONE; machine <= MSDOS_PORT_ONE_4BTN; machine++) {
            const msdos_control *view = msdossession_controls_layout(machine, &count, NULL, NULL);
            for (idx = 0; idx < count; idx++) {
                if (view[idx].kind == MSDOS_CTL_BUTTON || view[idx].kind == MSDOS_CTL_PILL) {
                    enabled[machine][view[idx].stick][view[idx].target % MSDOS_ACT_COUNT - MSDOS_ACT_B1] = view[idx].enabled;
                    if (machine == MSDOS_PORT_TWO) found[view[idx].stick][view[idx].target % MSDOS_ACT_COUNT]++;
                }
                if (machine == MSDOS_PORT_TWO && view[idx].kind == MSDOS_CTL_TRIM) trims++;
                if (machine == MSDOS_PORT_TWO && view[idx].kind == MSDOS_CTL_STICK) sticks++;
            }
        }
        check(found[0][MSDOS_ACT_B1] == 1 && found[0][MSDOS_ACT_B2] == 1 && found[0][MSDOS_ACT_B3] == 1
              && found[0][MSDOS_ACT_B4] == 1 && found[1][MSDOS_ACT_B1] == 1 && found[1][MSDOS_ACT_B4] == 1,
              "each stick has its four buttons: 1 on the knob, 2 on the side pad, 3 and 4 as pills");
        check(trims == 4 && sticks == 2, "two trim sliders and a steering area per stick");
        check(!enabled[MSDOS_PORT_NONE][0][0] && !enabled[MSDOS_PORT_NONE][1][0] && !enabled[MSDOS_PORT_NONE][0][2],
              "no game port: both sticks dimmed");
        check(enabled[MSDOS_PORT_TWO][0][0] && enabled[MSDOS_PORT_TWO][0][1] && enabled[MSDOS_PORT_TWO][1][0]
              && enabled[MSDOS_PORT_TWO][1][1] && !enabled[MSDOS_PORT_TWO][0][2] && !enabled[MSDOS_PORT_TWO][1][3],
              "two buttons each: A and B live with buttons 1 and 2, the pills for 3 and 4 dimmed");
        check(enabled[MSDOS_PORT_ONE_4BTN][0][0] && enabled[MSDOS_PORT_ONE_4BTN][0][2] && enabled[MSDOS_PORT_ONE_4BTN][0][3]
              && !enabled[MSDOS_PORT_ONE_4BTN][1][0] && !enabled[MSDOS_PORT_ONE_4BTN][1][1],
              "one 4-button stick: A has all four, B is dimmed");
    }

    /* hit tests: the plate steers, the knob presses button 1, the side pad button 2, the pills 3 and 4 */
    for (machine = MSDOS_PORT_NONE; machine <= MSDOS_PORT_ONE_4BTN; machine++) {
        const int stick_a = msdossession_controls_hit(machine, 3.0f, 3.0f);
        ctl = msdossession_controls_layout(machine, &count, NULL, NULL);
        for (idx = 0, pos = -1; idx < count; idx++)
            if (ctl[idx].kind == MSDOS_CTL_STICK && ctl[idx].stick == 0) pos = idx;
        check(machine == MSDOS_PORT_NONE ? stick_a == -1 : stick_a == pos, "a point on the plate (away from the knob) steers stick A");
    }
    ctl = msdossession_controls_layout(MSDOS_PORT_TWO, &count, NULL, NULL);
    for (idx = 0; idx < count; idx++) {
        if (ctl[idx].kind == MSDOS_CTL_STICK && ctl[idx].stick == 1) {
            int hit = msdossession_controls_hit(MSDOS_PORT_TWO, ctl[idx].cenx, ctl[idx].ceny);
            check(hit >= 0 && ctl[hit].kind == MSDOS_CTL_BUTTON && ctl[hit].target == MSDOS_TARGET(1, MSDOS_ACT_B1),
                  "the knob at rest is button 1 of stick B (a round part, hit as a circle)");
            check(msdossession_controls_hit(MSDOS_PORT_TWO, ctl[idx].cenx + 1.2f, ctl[idx].ceny) == idx,
                  "beside the knob the plate steers");
            check(msdossession_controls_hit(MSDOS_PORT_TWO, ctl[idx].cenx + 0.9f, ctl[idx].ceny + 0.9f) == idx,
                  "and the corner of the knob's box is outside its circle");
        }
        if (ctl[idx].kind == MSDOS_CTL_BUTTON && ctl[idx].target == MSDOS_TARGET(0, MSDOS_ACT_B2))
            check(msdossession_controls_hit(MSDOS_PORT_TWO, ctl[idx].x + ctl[idx].w * 0.5f, ctl[idx].y + ctl[idx].h * 0.5f) == idx,
                  "the side pad is button 2");
        if (ctl[idx].kind == MSDOS_CTL_PILL && ctl[idx].target == MSDOS_TARGET(0, MSDOS_ACT_B3)) {
            check(msdossession_controls_hit(MSDOS_PORT_TWO, ctl[idx].x + 1.0f, ctl[idx].y + 0.4f) == -1,
                  "a dimmed pill is not hit");
            pos = msdossession_controls_hit(MSDOS_PORT_ONE_4BTN, ctl[idx].x + 1.0f, ctl[idx].y + 0.4f);
            check(pos >= 0 && msdossession_controls_layout(MSDOS_PORT_ONE_4BTN, NULL, NULL, NULL)[pos].target
                  == MSDOS_TARGET(0, MSDOS_ACT_B3), "a live pill is button 3");
            check(msdossession_controls_hit(MSDOS_PORT_ONE_4BTN, ctl[idx].x - 0.2f, ctl[idx].y + 0.4f) == pos,
                  "pills have a grown hit area");
        }
    }
    check(msdossession_controls_hit(MSDOS_PORT_TWO, -1.0f, -1.0f) == -1
          && msdossession_controls_hit(MSDOS_PORT_TWO, 100.0f, 100.0f) == -1, "outside the picture is nothing");

    /* the drag vector */
    {
        int xval = 1, yval = 1;
        msdossession_stick_from_vector(0.0f, 0.0f, 1.9f, &xval, &yval);
        check(xval == 0 && yval == 0, "no drag is centre");
        msdossession_stick_from_vector(1.9f, -1.9f, 1.9f, &xval, &yval);
        check(xval == 32767 && yval == -32768, "a drag of the full reach is the stops");
        msdossession_stick_from_vector(9.0f, 9.0f, 1.9f, &xval, &yval);
        check(xval == 32767 && yval == 32767, "past the reach is clamped");
        msdossession_stick_from_vector(0.95f, 0.0f, 1.9f, &xval, &yval);
        check(xval > 16000 && xval < 16500 && yval == 0, "half the reach is half the axis");
        msdossession_stick_from_vector(-1.0f, 0.0f, 0.0f, &xval, &yval);
        check(xval == 0, "a zero reach is centre, not a division");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
