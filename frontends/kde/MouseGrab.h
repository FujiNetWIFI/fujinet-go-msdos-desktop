/*
 * The captured mouse, shared by the GPU and the software display: movement
 * as mickeys, buttons and the wheel, to the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QMouseEvent>
#include <QPointF>
#include <QWheelEvent>
#include <functional>

#include "msdossession.h"

class MouseGrab {
public:
    explicit MouseGrab(msdossession *session) : m_session(session) {}

    bool captured() const { return m_captured; }
    std::function<void(bool)> changed;   /* the new state */

    void setCaptured(bool on)
    {
        on = on && msdossession_mouse_fitted(m_session);
        if (on == m_captured)
            return;
        m_captured = on;
        m_haveLast = false;
        m_wheel = 0;
        if (!on && m_buttons) {
            m_buttons = 0;
            msdossession_mouse_buttons(m_session, 0);
        }
        if (changed)
            changed(on);
    }

    /* Movement while captured, as the host pointer's: the mickeys stop where
     * the host pointer meets the window's edge. */
    void move(QMouseEvent *e)
    {
        if (!m_captured) {
            m_haveLast = false;
            return;
        }
        QPointF at = e->position();
        if (m_haveLast) {
            const int dx = (int)(at.x() - m_last.x()), dy = (int)(at.y() - m_last.y());
            if (dx || dy)
                msdossession_mouse_motion(m_session, dx, dy);
            if (!dx) at.setX(m_last.x());
            if (!dy) at.setY(m_last.y());
        }
        m_last = at;
        m_haveLast = true;
    }

    void press(QMouseEvent *e)
    {
        if (!m_captured) {
            if (e->button() == Qt::LeftButton)
                setCaptured(true);
            return;
        }
        if (e->button() == Qt::MiddleButton) {
            setCaptured(false);
            return;
        }
        m_buttons |= bit(e->button());
        msdossession_mouse_buttons(m_session, m_buttons);
    }

    void release(QMouseEvent *e)
    {
        const unsigned b = bit(e->button());
        if (!m_captured || !(m_buttons & b))
            return;
        m_buttons &= ~b;
        msdossession_mouse_buttons(m_session, m_buttons);
    }

    void wheel(QWheelEvent *e)
    {
        if (!m_captured) {
            e->ignore();
            return;
        }
        m_wheel += e->angleDelta().y();
        while (m_wheel >= 120 || m_wheel <= -120) {
            const int dz = m_wheel > 0 ? 1 : -1;
            msdossession_mouse_wheel(m_session, dz);
            m_wheel -= dz * 120;
        }
        e->accept();
    }

    void leave() { m_haveLast = false; }

private:
    static unsigned bit(Qt::MouseButton b)
    {
        return b == Qt::LeftButton ? 1u : b == Qt::RightButton ? 2u : b == Qt::MiddleButton ? 4u : 0u;
    }

    msdossession *m_session;
    bool m_captured = false;
    QPointF m_last;
    bool m_haveLast = false;
    int m_wheel = 0;            /* wheel travel not yet a notch */
    unsigned m_buttons = 0;
};
