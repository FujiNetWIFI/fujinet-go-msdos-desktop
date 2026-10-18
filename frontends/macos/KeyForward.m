/*
 * KeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

#import <Carbon/Carbon.h>   /* kVK_* virtual key codes */

/* The device-dependent modifier bits in -[NSEvent modifierFlags] (IOKit's
 * NX_DEVICE*KEYMASK): the generic Shift flag stays set while either Shift
 * is held, so letting go of one of two would go unseen without these. */
enum {
    DEV_LCTL = 0x00000001u,
    DEV_LSHIFT = 0x00000002u,
    DEV_RSHIFT = 0x00000004u,
    DEV_LCMD = 0x00000008u,
    DEV_RCMD = 0x00000010u,
    DEV_LALT = 0x00000020u,
    DEV_RALT = 0x00000040u,
    DEV_RCTL = 0x00002000u,
    DEV_ALL = DEV_LCTL | DEV_LSHIFT | DEV_RSHIFT | DEV_LCMD | DEV_RCMD | DEV_LALT | DEV_RALT | DEV_RCTL,
};

/* HID usages the remapped Mac keys stand for. */
enum { HID_PRINT_SCREEN = 0x46, HID_SCROLL_LOCK = 0x47, HID_PAUSE = 0x48, HID_INSERT = 0x49 };

uint32_t MSDOSHidFromKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
    case kVK_Help: return HID_INSERT;
    case kVK_F13: return HID_PRINT_SCREEN;
    case kVK_F14: return HID_SCROLL_LOCK;
    case kVK_F15: return HID_PAUSE;
    default: return msdossession_hid_from_macos_keycode(keyCode);
    }
}

BOOL MSDOSForwardKey(msdossession *session, NSEvent *event, BOOL down)
{
    const uint32_t hid = MSDOSHidFromKeyCode([event keyCode]);
    if (!session || !hid) return NO;
    /* Command shortcuts are the menus' */
    if (down && ([event modifierFlags] & NSEventModifierFlagCommand)) return NO;
    return msdossession_key(session, hid, down ? 1 : 0) != 0;
}

void MSDOSForwardFlags(msdossession *session, NSEvent *event)
{
    const NSEventModifierFlags f = [event modifierFlags];
    NSEventModifierFlags generic = 0;
    NSEventModifierFlags device = 0;
    if (!session) return;
    switch ([event keyCode]) {
    case kVK_Shift: generic = NSEventModifierFlagShift; device = DEV_LSHIFT; break;
    case kVK_RightShift: generic = NSEventModifierFlagShift; device = DEV_RSHIFT; break;
    case kVK_Control: generic = NSEventModifierFlagControl; device = DEV_LCTL; break;
    case kVK_RightControl: generic = NSEventModifierFlagControl; device = DEV_RCTL; break;
    case kVK_Option: generic = NSEventModifierFlagOption; device = DEV_LALT; break;
    case kVK_RightOption: generic = NSEventModifierFlagOption; device = DEV_RALT; break;
    case kVK_CapsLock: {
        /* one event per press, carrying the light's new state: a tap of
         * the PC's Caps Lock, which keeps its own light */
        const uint32_t hid = MSDOSHidFromKeyCode(kVK_CapsLock);
        if (hid) {
            msdossession_key(session, hid, 1);
            msdossession_key(session, hid, 0);
        }
        return;
    }
    case kVK_Command:
    case kVK_RightCommand:
        if (f & NSEventModifierFlagCommand) msdossession_release_keys(session);
        return;
    default:
        return;
    }
    const uint32_t hid = MSDOSHidFromKeyCode([event keyCode]);
    if (!hid) return;
    /* a synthetic event may carry no device bits: the generic one then */
    const BOOL down = (f & DEV_ALL) ? (f & device) != 0 : (f & generic) != 0;
    msdossession_key(session, hid, down ? 1 : 0);
}

NSColor *MSDOSAccentColor(void)
{
    return [NSColor colorWithSRGBRed:((MSDOSSESSION_ACCENT_RGB >> 16) & 0xff) / 255.0
                               green:((MSDOSSESSION_ACCENT_RGB >> 8) & 0xff) / 255.0
                                blue:(MSDOSSESSION_ACCENT_RGB & 0xff) / 255.0
                               alpha:1.0];
}

NSColor *MSDOSInkColor(void)
{
    return [NSColor colorWithSRGBRed:0x1e / 255.0 green:0x1e / 255.0 blue:0x1e / 255.0 alpha:1.0];
}
