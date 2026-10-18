/*
 * The PC's picture: a QWidget that pulls frames from the session, and the
 * mouse when it is captured.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>

#include "msdossession.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(msdossession *session, QWidget *parent = nullptr);
    void setSmooth(bool smooth);
    void setIntegerScale(bool on);
    /* The mouse belongs to the PC: hidden, its movement sent as mickeys. A
     * click on the picture captures it (when a mouse is fitted); Ctrl+End
     * or the middle button lets it go. */
    void setCaptured(bool on);
    bool captured() const { return m_captured; }
    /* The last frame as a PNG. */
    bool savePng(const QString &path) const;
    quint64 frames() const { return m_frames; }

signals:
    void captureChanged(bool on);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void leaveEvent(QEvent *e) override;
    /* Tab is the PC's: it reaches the window's key handler instead of
     * moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void tick();

    msdossession *m_session;
    QImage m_image;             /* the session's buffer, MSDOSSESSION_FB_MAX_* */
    int m_w = 0, m_h = 0, m_par = 1000;
    uint64_t m_serial = 0;
    quint64 m_frames = 0;
    QTimer m_timer;
    bool m_smooth = true;
    bool m_integer = false;
    bool m_captured = false;
    QPointF m_last;
    bool m_haveLast = false;
    int m_wheel = 0;            /* wheel travel not yet a notch */
    unsigned m_buttons = 0;
};
