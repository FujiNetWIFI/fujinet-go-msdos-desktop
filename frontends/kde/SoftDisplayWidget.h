/*
 * The software display: the fallback for a machine with no usable GPU device
 * (renderFailed). A QWidget that scales with QPainter, fed by FrameFeed like
 * the GPU one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "FrameStore.h"
#include "MouseGrab.h"
#include "msdossession.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(msdossession *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setSmooth(bool smooth) override;
    void setIntegerScale(bool on) override;
    void setCaptured(bool on) override;
    bool captured() const override { return m_mouse.captured(); }
    void onCaptureChanged(std::function<void(bool)> fn) override { m_mouse.changed = std::move(fn); }
    bool savePng(const QString &path) const override;
    quint64 frames() const override { return m_frame.frames; }

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override { m_mouse.move(e); }
    void mousePressEvent(QMouseEvent *e) override { setFocus(); m_mouse.press(e); }
    void mouseReleaseEvent(QMouseEvent *e) override { m_mouse.release(e); }
    void wheelEvent(QWheelEvent *e) override { m_mouse.wheel(e); }
    void leaveEvent(QEvent *) override { m_mouse.leave(); }
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    msdossession *m_session;
    FrameFeed *m_feed;
    FrameStore m_frame;
    MouseGrab m_mouse;
    bool m_smooth = true;
    bool m_integer = false;
};
