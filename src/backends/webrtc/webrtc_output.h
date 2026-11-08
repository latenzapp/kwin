/*
    KWin WebRTC Backend - Output
    Headless output for WebRTC streaming.

    Based on KWin's VirtualOutput:
    SPDX-FileCopyrightText: 2018 Roman Gilg <subdiff@gmail.com>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/backendoutput.h"

#include <QObject>

namespace KWin
{

class SoftwareVsyncMonitor;
class WebRTCBackend;
class OutputFrame;

class WebRTCOutput : public BackendOutput
{
    Q_OBJECT

public:
    explicit WebRTCOutput(WebRTCBackend *parent, const std::optional<QString> &connectorName);
    ~WebRTCOutput() override;

    std::chrono::nanoseconds frameCallbackDelay() const override;
    std::expected<void, OutputError> testPresentation(const std::shared_ptr<OutputFrame> &frame) override;
    std::expected<void, OutputError> present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame) override;
    // Fast path for a cursor-only update: forwards to
    // WebRTCCursorLayer::notifyAsyncMove() instead of waiting for the next
    // full present(). See compositor.cpp's per-cursor repaint lambda for when
    // this gets called.
    std::expected<void, OutputError> presentAsync(OutputLayer *layer, std::optional<std::chrono::nanoseconds> allowedVrrDelay) override;

    void init(const QPoint &logicalPosition, const QSize &pixelSize, qreal scale,
              const QList<OutputModeline> &modes);

    void applyChanges(const OutputConfiguration &config) override;

    void setOutputLayer(std::unique_ptr<OutputLayer> &&layer);
    OutputLayer *outputLayer() const;
    void setCursorLayer(std::unique_ptr<OutputLayer> &&layer);
    OutputLayer *cursorLayer() const;

    // Marks the output as streaming and, when enabling, requests one
    // composite; with KWIN_WEBRTC_CONTINUOUS=1 present() also re-arms the
    // render loop on every present while the flag is set.
    void setContinuousMode(bool enabled);
    bool isContinuousMode() const { return m_continuousMode; }

    // Wake the render loop for ONE composite, on a desktop with no damage.
    //
    // Both halves are needed and neither is enough: RenderLoop::scheduleRepaint()
    // wakes the loop, but the compositor skips a layer whose needsRepaint() is
    // false and endFrame() -- where a forced capture is consumed -- never runs.
    //
    // Separate from setContinuousMode() so StreamStart can request a composite
    // whatever the current flag: on a static desktop nothing else would
    // produce the first frame.
    void kickRepaint();

    // Dynamically change the output resolution (creates a new mode and applies it)
    void changeMode(const QSize &pixelSize);
    void changeRefreshRate(uint32_t fps);

    // Drive the render loop faster than the mode this output advertises.
    //
    // The mode's refresh rate is what clients see (plasmashell, every app's
    // frame callbacks) and it stays pinned to the negotiated stream fps. The
    // render loop's rate is something else: it is the grid a scene change has
    // to wait for before it is composited at all, and at 60 Hz that costs
    // 8.3 ms of pure scheduling on average -- nothing is scanned out here, the
    // "vblank" is a QTimer (SoftwareVsyncMonitor). Composing on a finer grid
    // shrinks that wait; the rate at which frames reach the peer stays capped
    // by the submit throttle in WebRTCEglBackend::captureFrame().
    //
    // 0 restores "follow the mode".
    void setCaptureRefreshRate(uint32_t hz);

    // Set position for multi-monitor layout (wraps protected setState)
    void setPosition(const QPoint &pos);

    // Set the DPI scale factor (wraps protected setState, same shape as
    // setPosition). Only for the pre-Workspace path in
    // WebRTCEglBackend::setOutputScale -- once Workspace exists the change has
    // to go through applyOutputConfiguration, or clients are never relaid out.
    void setScale(qreal scale);

private:
    void vblank(std::chrono::nanoseconds timestamp);
    // KWIN_WEBRTC_PRESENT_MODE=adaptive: completes the pending frame one event
    // loop turn after present(), at its target time. See webrtc_output.cpp.
    void completeAdaptiveFrame();
    // Single place that decides what the render loop and the software vsync
    // monitor actually run at: the capture override when set, the mode's own
    // rate otherwise. Takes the mode rate as an argument so callers can invoke
    // it either side of setState() without an ordering trap.
    void applyRenderRate(uint32_t modeRefreshMilliHz);
    // Shared tail of changeMode()/changeRefreshRate(): installs a fresh
    // single-entry mode list, re-applies the render rate, preserves the rest
    // of the state, and emits changed().
    void applySingleMode(const QSize &pixelSize, uint64_t refreshMilliHz);
    void syncLogicalOutput();

    friend class WebRTCBackend;

    std::unique_ptr<OutputLayer> m_layer;
    std::unique_ptr<OutputLayer> m_cursorLayer;
    WebRTCBackend *m_backend;
    std::unique_ptr<SoftwareVsyncMonitor> m_vsyncMonitor;
    int m_identifier;
    std::shared_ptr<OutputFrame> m_frame;
    bool m_continuousMode = false; // set while a stream is active
    uint32_t m_captureHz = 0;      // 0 = render loop follows the advertised mode
};

} // namespace KWin
