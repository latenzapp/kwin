/*
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_cursor_layer.h"
#include "webrtc_egl_backend.h"
#include "webrtc_logging.h"

#include "opengl/glrendertimequery.h"
#include "opengl/glutils.h"
#include "pipeline/WebRTCLog.hpp"

#include <QHashFunctions>

namespace KWin
{

WebRTCCursorLayer::WebRTCCursorLayer(BackendOutput *output, WebRTCEglBackend *backend)
    : OutputLayer(output, OutputLayerType::CursorOnly)
    , m_backend(backend)
{
}

WebRTCCursorLayer::~WebRTCCursorLayer()
{
    (void)m_backend->openglContext()->makeCurrent();
    m_framebuffer.reset();
    m_texture.reset();
}

std::optional<OutputLayerBeginFrameInfo> WebRTCCursorLayer::beginFrame(OutputFrame *frame)
{
    if (!m_backend->openglContext()->makeCurrent()) {
        return std::nullopt;
    }

    const QSize bufferSize = targetRect().size();
    if (bufferSize.isEmpty()) {
        WEBRTC_LOG("WebRTCCursorLayer::doBeginFrame: empty targetRect, skipping");
        return std::nullopt;
    }
    WEBRTC_LOG("WebRTCCursorLayer::doBeginFrame: size=%dx%d", bufferSize.width(), bufferSize.height());
    if (!m_texture || m_texture->size() != bufferSize) {
        m_texture = GLTexture::allocate(GL_RGBA8, bufferSize);
        if (!m_texture) {
            return std::nullopt;
        }
        m_framebuffer = std::make_unique<GLFramebuffer>(m_texture.get());
    }
    m_query = GLRenderTimeQuery::begin(m_backend->openglContextRef());
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_framebuffer.get()),
        .repaint = Region::infinite(),
    };
}

bool WebRTCCursorLayer::endFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    EglContext *context = m_backend->openglContext();
    QImage buffer(m_framebuffer->size(), QImage::Format_RGBA8888_Premultiplied);

    GLFramebuffer::pushFramebuffer(m_framebuffer.get());
    context->glReadnPixels(0, 0, buffer.width(), buffer.height(), GL_RGBA, GL_UNSIGNED_BYTE, buffer.sizeInBytes(), buffer.bits());
    GLFramebuffer::popFramebuffer();

    // Decide whether anything changed BEFORE paying for the conversion.
    //
    // The readback has to happen -- it is the only way to know what the
    // compositor drew. A hash over the raw readback answers "is this the same
    // shape as last time" in one pass, and the size rides along so a
    // different geometry can never be mistaken for a matching hash. The flip
    // and the conversion run only on a real shape change, which is also the
    // only time the bridge message goes out.
    const quint64 hash = qHashBits(buffer.constBits(), static_cast<size_t>(buffer.sizeInBytes()));
    if (hash != m_lastShapeHash || buffer.size() != m_lastShapeSize) {
        m_lastShapeHash = hash;
        m_lastShapeSize = buffer.size();

        // The bridge's CursorImage contract is straight-alpha RGBA8; GL gives
        // us premultiplied.
        const QImage shape = buffer.mirrored(false, true).convertToFormat(QImage::Format_RGBA8888);
        const uint32_t outputIdx = m_backend->getOutputIndex(m_output);
        m_backend->sendCursorImage(outputIdx, static_cast<uint32_t>(shape.width()),
                                    static_cast<uint32_t>(shape.height()),
                                    static_cast<int32_t>(hotspot().x()),
                                    static_cast<int32_t>(hotspot().y()), shape.constBits(),
                                    static_cast<size_t>(shape.sizeInBytes()));
    }

    if (frame && m_query) {
        m_query->end();
        frame->addRenderTimeQuery(std::move(m_query));
    }

    // Every render also refreshes position and hotspot; sendPosition()
    // dedupes.
    sendPosition();

    return true;
}

FormatModifierMap WebRTCCursorLayer::supportedDrmFormats() const
{
    return m_backend->supportedFormats();
}

void WebRTCCursorLayer::releaseBuffers()
{
    m_framebuffer.reset();
    m_texture.reset();
}

bool WebRTCCursorLayer::notifyAsyncMove()
{
    sendPosition();
    return true;
}

void WebRTCCursorLayer::pushPresentState()
{
    sendPosition();
}

void WebRTCCursorLayer::sendPosition()
{
    const Rect rect = targetRect();
    const QPoint pos = rect.topLeft();
    const QPoint hot(static_cast<int>(hotspot().x()), static_cast<int>(hotspot().y()));
    const bool visible = isEnabled();

    if (m_havePushedPos && pos == m_lastSentPos && hot == m_lastSentHotspot
        && visible == m_lastSentVisible) {
        return;
    }
    m_havePushedPos = true;
    m_lastSentPos = pos;
    m_lastSentHotspot = hot;
    m_lastSentVisible = visible;

    const uint32_t outputIdx = m_backend->getOutputIndex(m_output);
    m_backend->sendCursorPosition(outputIdx, pos.x(), pos.y(), hot.x(), hot.y(), visible);
}

} // namespace KWin
