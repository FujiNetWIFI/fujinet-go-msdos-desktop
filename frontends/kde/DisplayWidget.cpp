/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEvent>
#include <QFile>
#include <QMatrix4x4>
#include <QVector4D>
#include <QWindow>

/* An empty shader (a wrong resource path) makes pipeline creation fail and
 * the display silently fall back to software: say so. */
static QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    QShader sh;
    if (f.open(QIODevice::ReadOnly))
        sh = QShader::fromSerialized(f.readAll());
    if (!sh.isValid())
        qWarning("DisplayWidget: shader %s is missing or empty; the GPU display cannot start", path);
    return sh;
}

DisplayWidget::DisplayWidget(msdossession *session, QWidget *parent)
    : QRhiWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)), m_mouse(session)
{
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(m_feed, &FrameFeed::frameReady, this, &DisplayWidget::onFrame);
}

DisplayWidget::~DisplayWidget()
{
    m_feed->setActive(false);
}

void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }
void DisplayWidget::setIntegerScale(bool on) { m_integer = on; update(); }

void DisplayWidget::setCaptured(bool on)
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

bool DisplayWidget::savePng(const QString &path) const
{
    return m_frame.w > 0 && m_frame.view().copy().save(path, "PNG");
}

/* ---- idle ------------------------------------------------------------------
 * A hidden or minimised window has nobody to draw for: the feed unregisters
 * from the session, so the GUI thread gets no wakeups at all until it is
 * shown again. The machine itself keeps running. */

void DisplayWidget::updateActive()
{
    QWidget *w = window();
    m_feed->setActive(isVisible() && w && !w->isMinimized());
}

void DisplayWidget::showEvent(QShowEvent *e)
{
    QRhiWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void DisplayWidget::hideEvent(QHideEvent *e)
{
    QRhiWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool DisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QRhiWidget::eventFilter(watched, e);
}

bool DisplayWidget::event(QEvent *e)
{
    /* The compositor can mark a window occluded without minimising it */
    if (e->type() == QEvent::ScreenChangeInternal || e->type() == QEvent::WindowActivate)
        updateActive();
    return QRhiWidget::event(e);
}

/* ---- frames ---------------------------------------------------------------- */

void DisplayWidget::onFrame()
{
    if (!m_frame.pull(m_session))
        return;
    m_dirty = true;
    update();
}

/* ---- GPU ------------------------------------------------------------------- */

void DisplayWidget::releaseResources()
{
    m_pipeline.reset();
    m_srb.reset();
    m_linear.reset();
    m_nearest.reset();
    m_tex.reset();
    m_ubuf.reset();
}

void DisplayWidget::makeBindings()
{
    m_smoothApplied = m_smooth;
    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex.get(), m_smooth ? m_linear.get() : m_nearest.get()),
    });
    m_srb->create();
}

void DisplayWidget::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        releaseResources();
        m_rhi = rhi();
    }
    if (m_pipeline)
        return;

    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    m_ubuf->create();
    m_nearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    m_nearest->create();
    m_linear.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge));
    m_linear->create();
    /* exactly the frame's size; render() rebuilds it when the mode changes */
    const QSize ts = m_frame.w > 0 ? QSize(m_frame.w, m_frame.h) : QSize(1, 1);
    m_tex.reset(m_rhi->newTexture(QRhiTexture::BGRA8, ts));
    m_tex->create();
    /* a fresh texture is empty and a still picture publishes nothing more */
    m_dirty = m_frame.w > 0;

    makeBindings();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, loadShader(":/shaders/display.vert.qsb") },
        { QRhiShaderStage::Fragment, loadShader(":/shaders/display.frag.qsb") },
    });
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) {
        m_pipeline.reset();
        emit renderFailed();
    }
}

void DisplayWidget::render(QRhiCommandBuffer *cb)
{
    if (!m_pipeline)
        return;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    const bool have = m_frame.w > 0 && m_frame.h > 0;

    bool rebind = m_smooth != m_smoothApplied;
    if (have && m_tex->pixelSize() != QSize(m_frame.w, m_frame.h)) {
        /* a new video mode: a texture of exactly that size */
        m_tex.reset(m_rhi->newTexture(QRhiTexture::BGRA8, QSize(m_frame.w, m_frame.h)));
        m_tex->create();
        m_dirty = true;
        rebind = true;
    }
    if (rebind)
        makeBindings();

    if (have && m_dirty) {
        QRhiTextureSubresourceUploadDescription sub;
        const qsizetype bytes = (qsizetype)m_frame.w * m_frame.h * 4;
        sub.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(m_frame.px.data()), bytes));
        sub.setDataStride(m_frame.w * 4);
        sub.setSourceSize(QSize(m_frame.w, m_frame.h));
        u->uploadTexture(m_tex.get(), QRhiTextureUploadDescription({ QRhiTextureUploadEntry(0, 0, sub) }));
        m_dirty = false;
    }

    const QSize px = renderTarget()->pixelSize();
    if (have) {
        const QRectF r = displayRect(QSizeF(px), m_frame.w, m_frame.h, m_frame.par, m_integer);
        /* pixel rect -> clip space (y up), then through the backend's
         * correction so Vulkan's flipped y comes out the right way up */
        auto clip = [&](double x, double y) {
            return m_rhi->clipSpaceCorrMatrix() *
                   QVector4D(float(x / px.width() * 2.0 - 1.0), float(1.0 - y / px.height() * 2.0), 0.f, 1.f);
        };
        const QVector4D tl = clip(r.left(), r.top());
        const QVector4D br = clip(r.right(), r.bottom());
        const float rect[4] = { tl.x(), tl.y(), br.x(), br.y() };
        u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof rect, rect);
    }

    cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, u);
    if (have) {
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
        cb->setShaderResources(m_srb.get());
        cb->draw(4);
    }
    cb->endPass();
}
