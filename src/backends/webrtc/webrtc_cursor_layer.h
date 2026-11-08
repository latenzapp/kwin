/*
    KWin WebRTC Backend - Decoupled Cursor Layer

    A CursorOnly OutputLayer that lets the compositor render the mouse
    cursor separately from the primary scene (see endFrame() below),
    instead of it being baked into the captured frame by a software
    CursorItem. Requires KWIN_FORCE_SW_CURSOR to be unset (see
    s_forceSoftwareCursor / WorkspaceScene::layerCandidates() in
    src/scene/workspacescene.cpp) -- if it is set, this layer is never
    selected by the compositor and the cursor is composited into the primary
    frame (no CursorImage/CursorPos messages are sent then).

    Shape: rendered into a private FBO (same technique as
    X11WindowedEglCursorLayer / WaylandEglCursorLayer) and read back with
    glReadnPixels on each render; converted and pushed to the peer as a
    CursorImage bridge message only when its hash changed. Unlike the
    X11/Wayland backends, there is no host windowing system riding along to
    move the cursor for free, so position also has to be pushed explicitly
    -- see notifyAsyncMove(), called from WebRTCOutput::presentAsync() after
    every cursor-only update.

    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"

#include <QImage>
#include <QPoint>
#include <QSize>
#include <memory>

namespace KWin
{

class GLFramebuffer;
class GLTexture;
class GLRenderTimeQuery;
class WebRTCEglBackend;

class WebRTCCursorLayer : public OutputLayer
{
    Q_OBJECT

public:
    WebRTCCursorLayer(BackendOutput *output, WebRTCEglBackend *backend);
    ~WebRTCCursorLayer() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame(OutputFrame *frame) override;
    bool endFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

    // Called by WebRTCOutput::presentAsync() after every cursor-only update
    // (compositor.cpp's per-cursor repaint lambda renders this layer first
    // when the shape changed, then calls presentAsync()). targetRect()/
    // hotspot() are already fresh. Pushes the position, since nothing else
    // moves the cursor here. Always returns true: this layer has nothing
    // further to do besides forwarding position over the bridge.
    bool notifyAsyncMove();

    // Called by WebRTCOutput::present() for enable/disable and shape-bearing
    // updates (i.e. whenever endFrame() ran). Pushes visibility/position if
    // it changed since the last push; endFrame() already pushed the shape.
    //
    // A newly connected peer gets the current shape and position from
    // CBeamBridge, which caches the last cursor messages whether or not a
    // client is attached and replays them right after sendHello(), like the
    // output list. Forcing a cursor repaint does not work here:
    // OutputLayer::scheduleRepaint() emits repaintScheduled() synchronously,
    // and compositor.cpp's per-cursor lambda, also connected to it, sees no
    // item-level damage (`cursorView->needsRepaint()` stays false), calls
    // presentAsync() and resets the layer's repaints before composite() sees
    // them.
    void pushPresentState();

private:
    void sendPosition();

    WebRTCEglBackend *const m_backend;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    std::unique_ptr<GLTexture> m_texture;
    std::unique_ptr<GLRenderTimeQuery> m_query;

    // Dedupe state: avoid re-sending identical shape/position bridge
    // messages every frame just because the layer re-rendered.
    //
    // A hash and a size rather than the last QImage: the comparison is the
    // only thing the copy would be kept for, and holding a full-resolution
    // image per layer just for that would be wasteful.
    quint64 m_lastShapeHash = 0;
    QSize m_lastShapeSize;
    bool m_havePushedPos = false;
    QPoint m_lastSentPos;
    QPoint m_lastSentHotspot;
    bool m_lastSentVisible = false;
};

} // namespace KWin
