/*
 * The frame a display holds: XRGB (0x00RRGGBB) pixels packed at the frame's
 * own width, copied from the session when it has a new one. The frame's size
 * follows the video mode, so the buffer grows to the largest in use.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <cstdint>
#include <vector>

#include "msdossession.h"

struct FrameStore {
    std::vector<uint32_t> px;
    int w = 0, h = 0, par = 1000;
    uint64_t serial = 0;
    quint64 frames = 0;

    FrameStore() : px((size_t)1024 * 768) {}

    /* True when a new frame was copied in. */
    bool pull(msdossession *session)
    {
        int nw = 0, nh = 0, npar = 0;
        bool got = msdossession_copy_frame_n(session, px.data(), px.size(), &nw, &nh, &npar, &serial);
        if (!got && nw > 0 && nh > 0 && (size_t)nw * nh > px.size()) {
            px.resize((size_t)nw * nh);   /* a bigger video mode */
            got = msdossession_copy_frame_n(session, px.data(), px.size(), &nw, &nh, &npar, &serial);
        }
        if (!got || nw <= 0 || nh <= 0)
            return false;
        w = nw;
        h = nh;
        par = npar > 0 ? npar : 1000;
        frames++;
        return true;
    }

    /* The held frame as an image sharing the buffer (valid until the next pull). */
    QImage view() const
    {
        return QImage(reinterpret_cast<const uchar *>(px.data()), w, h, w * 4, QImage::Format_RGB32);
    }
};
