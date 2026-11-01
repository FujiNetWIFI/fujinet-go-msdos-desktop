/*
 * What the main window needs from its display, so the GPU display and the
 * software fallback are interchangeable.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QString>
#include <QtGlobal>
#include <functional>

class QWidget;

class DisplayView {
public:
    virtual ~DisplayView() = default;
    virtual QWidget *widget() = 0;
    virtual void setSmooth(bool smooth) = 0;
    virtual void setIntegerScale(bool on) = 0;
    /* The mouse belongs to the PC: hidden, its movement sent as mickeys. A
     * click on the picture captures it (when a mouse is fitted); Ctrl+End
     * or the middle button lets it go. */
    virtual void setCaptured(bool on) = 0;
    virtual bool captured() const = 0;
    /* Called with the new state whenever the capture changes. */
    virtual void onCaptureChanged(std::function<void(bool)> fn) = 0;
    /* The last frame as a PNG. */
    virtual bool savePng(const QString &path) const = 0;
    virtual quint64 frames() const = 0;
};
