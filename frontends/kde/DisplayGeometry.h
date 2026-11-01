/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display: the frame's size at its pixel aspect (x1000, 4:3 or
 * square), integer-scaled when asked and it fits, centred.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>
#include <QtGlobal>

inline QRectF displayRect(QSizeF avail, int fw, int fh, int par, bool integer)
{
    const qreal w = avail.width(), h = avail.height();
    const qreal pw = fw * par / 1000.0, ph = fh;
    qreal sw, sh;
    if (integer && pw <= w && ph <= h) {
        const int k = (int)qMin(w / pw, h / ph);
        sw = pw * k;
        sh = ph * k;
    } else if (w / h > pw / ph) {
        sh = h;
        sw = sh * pw / ph;
    } else {
        sw = w;
        sh = sw * ph / pw;
    }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
