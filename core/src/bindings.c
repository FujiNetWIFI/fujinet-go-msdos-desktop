/*
 * bindings -- the remappable key / gamepad table for the game port.
 *
 * One flat table of 16 targets (msdossession.h: MSDOS_TARGET(stick, act), per
 * stick Up, Down, Left, Right, Button 1-4). A target holds at most one
 * keyboard key (a HID usage: the PC's keyboard is positional) and up to two
 * gamepad inputs (the second exists for the triggers, which have always been
 * buttons 3 and 4 beside West and North). Rebinding STEALS: one key drives one
 * target; a pad input is unique within a stick, because a pad drives one stick
 * at a time (Joystick A and B may both use South).
 *
 * The table is the app's, not a machine's: it is not part of the per-machine
 * profiles and prefs_migrate never touches it. There are no default keys (the
 * PC owns its keyboard; Map can bind one, and a bound key then stops reaching
 * DOS). Seeds its defaults LAZILY so the table works with no session behind it.
 * Persisted as one "bindings" setting holding only the entries that differ from
 * the defaults: "<target>.k:<usage>", "<target>.b:<pad input>" and
 * "<target>.c:<second pad input>", space separated.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t key;       /* HID usage, 0 = none */
    int button;         /* pad input, NONE = none */
    int button2;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[MSDOS_TARGET_COUNT];
static slot s_defaults[MSDOS_TARGET_COUNT];
static int s_seeded;
static struct msdossession *s_store;    /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void compute_defaults_locked(void)
{
    int target, stick;
    for (target = 0; target < MSDOS_TARGET_COUNT; target++) {
        s_defaults[target].key = 0;
        s_defaults[target].button = MSDOS_PAD_BTN_NONE;
        s_defaults[target].button2 = MSDOS_PAD_BTN_NONE;
    }
    /* The same on both sticks (a pad drives one at a time): the D-pad pushes
     * the stick to its stops, South, East, West and North are buttons 1-4 and
     * the triggers are also buttons 3 and 4. */
    for (stick = 0; stick < MSDOS_JOYSTICKS; stick++) {
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_UP)].button = MSDOS_PAD_BTN_DPAD_UP;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_DOWN)].button = MSDOS_PAD_BTN_DPAD_DOWN;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_LEFT)].button = MSDOS_PAD_BTN_DPAD_LEFT;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_RIGHT)].button = MSDOS_PAD_BTN_DPAD_RIGHT;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_B1)].button = MSDOS_PAD_BTN_SOUTH;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_B2)].button = MSDOS_PAD_BTN_EAST;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_B3)].button = MSDOS_PAD_BTN_WEST;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_B3)].button2 = MSDOS_PAD_BTN_LEFT_TRIGGER;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_B4)].button = MSDOS_PAD_BTN_NORTH;
        s_defaults[MSDOS_TARGET(stick, MSDOS_ACT_B4)].button2 = MSDOS_PAD_BTN_RIGHT_TRIGGER;
    }
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

static int valid_button(int button)
{
    return button == MSDOS_PAD_BTN_NONE || MSDOS_PAD_BTN_IS_NAMED(button)
        || MSDOS_PAD_BTN_IS_RAW(button) || MSDOS_PAD_BTN_IS_HAT(button);
}

static int valid_key(uint32_t key)
{
    return key == 0 || (key >= 4 && key <= MSDOS_HID_USAGE_MAX);
}

static int holds_button(const slot *entry, int button)
{
    return entry->button == button || entry->button2 == button;
}

/* A slot never keeps a second input without a first. */
static void tidy(slot *entry)
{
    if (entry->button == MSDOS_PAD_BTN_NONE && entry->button2 != MSDOS_PAD_BTN_NONE) {
        entry->button = entry->button2;
        entry->button2 = MSDOS_PAD_BTN_NONE;
    }
    if (entry->button2 == entry->button) entry->button2 = MSDOS_PAD_BTN_NONE;
}

/* ---- persistence ---------------------------------------------------------- */

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int target;
    out[0] = '\0';
    for (target = 0; target < MSDOS_TARGET_COUNT; target++) {
        if (s_table[target].key != s_defaults[target].key && len + 40 < outsz)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", target, s_table[target].key);
        if (s_table[target].button != s_defaults[target].button && len + 40 < outsz)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", target, s_table[target].button);
        if (s_table[target].button2 != s_defaults[target].button2 && len + 40 < outsz)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.c:%d",
                                    len ? " " : "", target, s_table[target].button2);
    }
}

static void persist_locked(void)
{
    char packed[1024];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    msdossession_set_str(s_store, "bindings", packed);
}

static void apply_persisted_locked(const char *packed)
{
    uint8_t key_set[MSDOS_TARGET_COUNT] = { 0 }, pad_set[MSDOS_TARGET_COUNT] = { 0 };
    const char *cursor = packed;
    int target, other;

    while (cursor && *cursor) {
        int index = -1, consumed = 0;
        char kind = 0;
        long value = 0;
        if (sscanf(cursor, "%d.%c:%ld%n", &index, &kind, &value, &consumed) == 3
            && index >= 0 && index < MSDOS_TARGET_COUNT) {
            if (kind == 'k' && valid_key((uint32_t)value) && value >= 0) {
                s_table[index].key = (uint32_t)value;
                key_set[index] = 1;
            } else if ((kind == 'b' || kind == 'c') && valid_button((int)value)) {
                if (kind == 'b') s_table[index].button = (int)value;
                else s_table[index].button2 = (int)value;
                pad_set[index] = 1;
            }
        }
        cursor += consumed > 0 ? consumed : 1;
        while (*cursor == ' ' || *cursor == ';') cursor++;
    }
    for (target = 0; target < MSDOS_TARGET_COUNT; target++) tidy(&s_table[target]);

    /* What the setting binds displaces the DEFAULT holder of the same input
     * (a default added since it was written, say): one input, one target. */
    for (target = 0; target < MSDOS_TARGET_COUNT; target++) {
        for (other = 0; other < MSDOS_TARGET_COUNT; other++) {
            if (other == target) continue;
            if (key_set[target] && !key_set[other] && s_table[target].key
                && s_table[other].key == s_table[target].key)
                s_table[other].key = 0;
            if (pad_set[target] && !pad_set[other]
                && target / MSDOS_ACT_COUNT == other / MSDOS_ACT_COUNT) {
                int mine[2] = { s_table[target].button, s_table[target].button2 }, which;
                for (which = 0; which < 2; which++) {
                    if (mine[which] == MSDOS_PAD_BTN_NONE) continue;
                    if (s_table[other].button == mine[which]) s_table[other].button = MSDOS_PAD_BTN_NONE;
                    if (s_table[other].button2 == mine[which]) s_table[other].button2 = MSDOS_PAD_BTN_NONE;
                    tidy(&s_table[other]);
                }
            }
        }
    }
}

void bindings_init(struct msdossession *sess)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = sess;
    if (sess) apply_persisted_locked(msdossession_get_str(sess, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

int bindings_target_for_key(uint32_t hid)
{
    int target, found = -1;
    if (!hid) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (target = 0; target < MSDOS_TARGET_COUNT; target++)
        if (s_table[target].key == hid) { found = target; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

int msdossession_target_for_key(msdossession *sess, uint32_t hid_usage)
{
    (void)sess;
    return bindings_target_for_key(hid_usage);
}

/* Pad inputs are scoped to the stick the pad drives: only that stick's eight
 * targets are searched. An unmapped adapter's first four buttons (the raw
 * band) are buttons 1-4 as long as that button target is still on its default
 * input and nothing else on the stick took the raw button. */
int bindings_target_for_pad(int stick, int button)
{
    int act, found = -1;
    if (button == MSDOS_PAD_BTN_NONE || stick < 0 || stick >= MSDOS_JOYSTICKS) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (act = 0; act < MSDOS_ACT_COUNT; act++)
        if (holds_button(&s_table[MSDOS_TARGET(stick, act)], button)) {
            found = MSDOS_TARGET(stick, act);
            break;
        }
    if (found < 0 && button >= MSDOS_PAD_BTN_RAW_BASE && button < MSDOS_PAD_BTN_RAW_BASE + 4) {
        const int target = MSDOS_TARGET(stick, MSDOS_ACT_B1 + (button - MSDOS_PAD_BTN_RAW_BASE));
        if (s_table[target].button == s_defaults[target].button) found = target;
    }
    pthread_mutex_unlock(&s_lock);
    return found;
}

int msdossession_target_for_button(msdossession *sess, int stick, int button)
{
    (void)sess;
    return bindings_target_for_pad(stick, button);
}

msdos_binding msdossession_binding_get(msdossession *sess, int target)
{
    msdos_binding result = { 0, MSDOS_PAD_BTN_NONE, MSDOS_PAD_BTN_NONE };
    (void)sess;
    if (target < 0 || target >= MSDOS_TARGET_COUNT) return result;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    result.key = s_table[target].key;
    result.button = s_table[target].button;
    result.button2 = s_table[target].button2;
    pthread_mutex_unlock(&s_lock);
    return result;
}

/* ---- rebinding ------------------------------------------------------------ */

static void describe_stolen(const int *victims, int nvictims, char *stolen, int stolensz)
{
    int pos, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (pos = 0; pos < nvictims && len < stolensz; pos++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        pos ? ", " : "", msdos_target_name(victims[pos]));
}

void msdossession_binding_set_key(msdossession *sess, int target, uint32_t hid_usage,
                                  char *stolen, int stolensz)
{
    int victims[MSDOS_TARGET_COUNT], nvictims = 0, other;
    (void)sess;
    if (stolen && stolensz > 0) stolen[0] = '\0';
    if (target < 0 || target >= MSDOS_TARGET_COUNT || !valid_key(hid_usage)) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (hid_usage)
        for (other = 0; other < MSDOS_TARGET_COUNT; other++)
            if (other != target && s_table[other].key == hid_usage) {
                s_table[other].key = 0;
                victims[nvictims++] = other;
            }
    s_table[target].key = hid_usage;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void msdossession_binding_set_button(msdossession *sess, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[MSDOS_TARGET_COUNT], nvictims = 0, other;
    (void)sess;
    if (stolen && stolensz > 0) stolen[0] = '\0';
    if (target < 0 || target >= MSDOS_TARGET_COUNT || !valid_button(button)) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != MSDOS_PAD_BTN_NONE)
        for (other = 0; other < MSDOS_TARGET_COUNT; other++) {
            /* only within the stick: the other stick's pad is another pad */
            if (other == target || other / MSDOS_ACT_COUNT != target / MSDOS_ACT_COUNT
                || !holds_button(&s_table[other], button))
                continue;
            if (s_table[other].button == button) s_table[other].button = MSDOS_PAD_BTN_NONE;
            if (s_table[other].button2 == button) s_table[other].button2 = MSDOS_PAD_BTN_NONE;
            tidy(&s_table[other]);
            victims[nvictims++] = other;
        }
    s_table[target].button = button;
    s_table[target].button2 = MSDOS_PAD_BTN_NONE;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void msdossession_binding_clear(msdossession *sess, int target)
{
    (void)sess;
    if (target < 0 || target >= MSDOS_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    s_table[target].key = 0;
    s_table[target].button = MSDOS_PAD_BTN_NONE;
    s_table[target].button2 = MSDOS_PAD_BTN_NONE;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
}

void msdossession_bindings_reset(msdossession *sess)
{
    (void)sess;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
}

void msdossession_reset_controls(msdossession *sess)
{
    msdossession_bindings_reset(sess);
}

int msdossession_controls_show_bindings(msdossession *sess)
{
    return msdossession_get_int(sess, "controllers_show_bindings", 0) != 0;
}

void msdossession_set_controls_show_bindings(msdossession *sess, int on)
{
    msdossession_set_int(sess, "controllers_show_bindings", on ? 1 : 0);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[MSDOS_ACT_COUNT] = {
    "Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Button 4",
};

const char *msdos_target_short_name(int target)
{
    if (target < 0 || target >= MSDOS_TARGET_COUNT) return "?";
    return act_names[target % MSDOS_ACT_COUNT];
}

const char *msdos_target_name(int target)
{
    static char names[MSDOS_TARGET_COUNT][32];
    if (target < 0 || target >= MSDOS_TARGET_COUNT) return "?";
    if (!names[target][0])
        snprintf(names[target], sizeof names[target], "Joystick %c: %s",
                 'A' + target / MSDOS_ACT_COUNT, msdos_target_short_name(target));
    return names[target];
}

const char *msdos_pad_button_name(int button)
{
    static const char *const named[MSDOS_PAD_BTN_COUNT] = {
        "South", "East", "West", "North", "Back", "Guide", "Start", "Left Stick", "Right Stick",
        "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
        "Left Trigger", "Right Trigger",
    };
    static const char *const hat_dirs[MSDOS_PAD_HAT_DIRS] = {
        "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left", "Left", "Up-Left",
    };
    static char raw[MSDOS_PAD_BTN_RAW_LAST - MSDOS_PAD_BTN_RAW_BASE + 1][16];
    static char hats[MSDOS_PAD_HAT_LAST - MSDOS_PAD_HAT_BASE + 1][24];
    if (MSDOS_PAD_BTN_IS_NAMED(button)) return named[button];
    if (MSDOS_PAD_BTN_IS_RAW(button)) {
        char *text = raw[button - MSDOS_PAD_BTN_RAW_BASE];
        if (!text[0]) snprintf(text, sizeof raw[0], "Button %d", button - MSDOS_PAD_BTN_RAW_BASE + 1);
        return text;
    }
    if (MSDOS_PAD_BTN_IS_HAT(button)) {
        const int rel = button - MSDOS_PAD_HAT_BASE;
        char *text = hats[rel];
        if (!text[0])
            snprintf(text, sizeof hats[0], "Hat %d %s", rel / MSDOS_PAD_HAT_DIRS,
                     hat_dirs[rel % MSDOS_PAD_HAT_DIRS]);
        return text;
    }
    return "";
}
