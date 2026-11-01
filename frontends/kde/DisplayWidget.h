/*
 * The PC's picture on the GPU: a QRhiWidget that keeps one texture exactly
 * the size of the frame (rebuilt when the video mode changes) and draws one
 * textured quad, only when the session has published a new frame (see
 * FrameFeed). Qt picks Vulkan, Metal, D3D or OpenGL for it; the scaling,
 * aspect and filtering happen on the GPU. The mouse when it is captured.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRhiWidget>
#include <memory>
#include <rhi/qrhi.h>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "FrameStore.h"
#include "MouseGrab.h"
#include "msdossession.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(msdossession *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }
    void setSmooth(bool smooth) override;
    void setIntegerScale(bool on) override;
    void setCaptured(bool on) override;
    bool captured() const override { return m_mouse.captured(); }
    void onCaptureChanged(std::function<void(bool)> fn) override { m_mouse.changed = std::move(fn); }
    bool savePng(const QString &path) const override;
    quint64 frames() const override { return m_frame.frames; }

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool event(QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override { m_mouse.move(e); }
    void mousePressEvent(QMouseEvent *e) override { setFocus(); m_mouse.press(e); }
    void mouseReleaseEvent(QMouseEvent *e) override { m_mouse.release(e); }
    void wheelEvent(QWheelEvent *e) override { m_mouse.wheel(e); }
    void leaveEvent(QEvent *) override { m_mouse.leave(); }
    /* Tab is the PC's: it reaches the window's key handler instead of
     * moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();
    void makeBindings();

    msdossession *m_session;
    FrameFeed *m_feed;
    FrameStore m_frame;
    MouseGrab m_mouse;
    bool m_dirty = false;      /* m_frame holds a frame not yet uploaded */
    bool m_smooth = true;
    bool m_integer = false;
    bool m_smoothApplied = true;

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest, m_linear;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};
