/*
 * KeyForward -- see KeyForward.h.
 *
 * The PC keyboard is positional: the scan code travels, whatever the host's
 * layout makes of it. Without a native scan code (offscreen, other
 * platforms) the common keys are taken from Qt's key instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

static uint32_t hidFromQtKey(int k)
{
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        return 0x04u + (uint32_t)(k - Qt::Key_A);
    if (k >= Qt::Key_1 && k <= Qt::Key_9)
        return 0x1Eu + (uint32_t)(k - Qt::Key_1);
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12)
        return 0x3Au + (uint32_t)(k - Qt::Key_F1);
    switch (k) {
    case Qt::Key_0: return 0x27;
    case Qt::Key_Return: return 0x28;
    case Qt::Key_Enter: return 0x58;
    case Qt::Key_Escape: return 0x29;
    case Qt::Key_Backspace: return 0x2A;
    case Qt::Key_Tab: case Qt::Key_Backtab: return 0x2B;
    case Qt::Key_Space: return 0x2C;
    case Qt::Key_Right: return 0x4F;
    case Qt::Key_Left: return 0x50;
    case Qt::Key_Down: return 0x51;
    case Qt::Key_Up: return 0x52;
    case Qt::Key_Shift: return 0xE1;
    case Qt::Key_Control: return 0xE0;
    case Qt::Key_Alt: return 0xE2;
    default: return 0;
    }
}

uint32_t msdosHidFromQt(const QKeyEvent *e)
{
    const quint32 code = e->nativeScanCode();
    const uint32_t hid = code >= 8 ? msdossession_hid_from_evdev(code - 8) : 0;
    return hid ? hid : hidFromQtKey(e->key());
}

bool msdosAppKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int k = e->key();
    if (m & Qt::MetaModifier)
        return k != Qt::Key_Meta && k != Qt::Key_Super_L && k != Qt::Key_Super_R;
    return (m & Qt::ControlModifier) && (m & Qt::ShiftModifier) && k >= Qt::Key_A && k <= Qt::Key_Z;
}

bool msdosForwardKey(msdossession *session, const QKeyEvent *e, bool down)
{
    const uint32_t hid = msdosHidFromQt(e);
    if (!hid)
        return false;
    if (!down) {
        /* always, so a key let go while a shortcut was held cannot stick */
        if (!e->isAutoRepeat())
            msdossession_key(session, hid, 0);
        return !msdosAppKey(e);
    }
    if (msdosAppKey(e))
        return false;
    return msdossession_key(session, hid, 1) != 0;
}
