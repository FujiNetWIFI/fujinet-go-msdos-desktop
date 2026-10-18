/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Frames are pulled by serial: copy_frame does nothing when the PC has not
 * drawn anything new. The frame's size follows the video mode (CGA's
 * 640x200, VGA's 720x400 for text, SVGA up to 1024x768); its pixel aspect
 * (x1000) is what the 4:3 setting asks for, so a 200-line mode is shown on a
 * monitor's shape.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

DisplayWidget::DisplayWidget(msdossession *session, QWidget *parent)
    : QWidget(parent), m_session(session),
      m_image(MSDOSSESSION_FB_MAX_WIDTH, MSDOSSESSION_FB_MAX_HEIGHT, QImage::Format_RGB32)
{
    m_image.fill(Qt::black);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    int w, h, par;
    /* 0x00RRGGBB is QImage::Format_RGB32 as it is; the frame is packed at
     * its own width from the start of the buffer */
    if (msdossession_copy_frame(m_session, reinterpret_cast<uint32_t *>(m_image.bits()), &w, &h, &par,
                                &m_serial) && w > 0 && h > 0) {
        m_w = w;
        m_h = h;
        m_par = par > 0 ? par : 1000;
        m_frames++;
        update();
    }
}

void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }
void DisplayWidget::setIntegerScale(bool on) { m_integer = on; update(); }

/* The packed frame as an image of its own size (sharing the buffer). */
static QImage frameView(const QImage &buf, int w, int h)
{
    return QImage(buf.constBits(), w, h, w * 4, QImage::Format_RGB32);
}

bool DisplayWidget::savePng(const QString &path) const
{
    return m_w > 0 && frameView(m_image, m_w, m_h).copy().save(path, "PNG");
}

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_w <= 0 || m_h <= 0)
        return;
    const double w = width(), h = height();
    const double pw = m_w * m_par / 1000.0, ph = m_h;
    double sw, sh;
    if (m_integer && pw <= w && ph <= h) {
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
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh), frameView(m_image, m_w, m_h));
}

/* ---- the mouse ----------------------------------------------------------------- */

void DisplayWidget::setCaptured(bool on)
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
    if (on)
        setCursor(Qt::BlankCursor);
    else
        unsetCursor();
    emit captureChanged(on);
}

static unsigned buttonBit(Qt::MouseButton b)
{
    return b == Qt::LeftButton ? 1u : b == Qt::RightButton ? 2u : b == Qt::MiddleButton ? 4u : 0u;
}

/* Movement while captured, as the host pointer's: the mickeys stop where
 * the host pointer meets the window's edge. */
void DisplayWidget::mouseMoveEvent(QMouseEvent *e)
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

void DisplayWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (!m_captured) {
        if (e->button() == Qt::LeftButton)
            setCaptured(true);
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        setCaptured(false);
        return;
    }
    m_buttons |= buttonBit(e->button());
    msdossession_mouse_buttons(m_session, m_buttons);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
    const unsigned bit = buttonBit(e->button());
    if (!m_captured || !(m_buttons & bit))
        return;
    m_buttons &= ~bit;
    msdossession_mouse_buttons(m_session, m_buttons);
}

void DisplayWidget::wheelEvent(QWheelEvent *e)
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

void DisplayWidget::leaveEvent(QEvent *)
{
    m_haveLast = false;
}
