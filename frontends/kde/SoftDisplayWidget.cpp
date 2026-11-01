/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(msdossession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)), m_mouse(session)
{
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }
void SoftDisplayWidget::setIntegerScale(bool on) { m_integer = on; update(); }

void SoftDisplayWidget::setCaptured(bool on)
{
    const bool was = m_mouse.captured();
    m_mouse.setCaptured(on);
    if (m_mouse.captured() == was)
        return;
    if (m_mouse.captured())
        setCursor(Qt::BlankCursor);
    else
        unsetCursor();
}

bool SoftDisplayWidget::savePng(const QString &path) const
{
    return m_frame.w > 0 && m_frame.view().copy().save(path, "PNG");
}

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

void SoftDisplayWidget::onFrame()
{
    if (m_frame.pull(m_session))
        update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_frame.w <= 0 || m_frame.h <= 0)
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(displayRect(QSizeF(size()), m_frame.w, m_frame.h, m_frame.par, m_integer),
                m_frame.view());
}
