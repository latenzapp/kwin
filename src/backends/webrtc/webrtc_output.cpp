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
#include "webrtc_output.h"
#include "webrtc_backend.h"
#include "webrtc_cursor_layer.h"
#include "webrtc_logging.h"

#include "compositor.h"
#include "core/outputconfiguration.h"
#include "core/outputlayer.h"
#include "core/colorspace.h"
#include "core/region.h"
#include "core/renderbackend.h"
#include "core/renderloop.h"
#include "pipeline/WebRTCLog.hpp"
#include "pipeline/WebRTCEnv.hpp"
#include "utils/softwarevsyncmonitor.h"
#include "workspace.h"

#include <algorithm>
#include <cstdlib>
#include <strings.h>

namespace KWin
{

// KWIN_WEBRTC_CONTINUOUS=1: re-arm the render loop on every present while
// streaming, instead of composing on damage.
static bool s_forceContinuousEnv()
{
    static const bool value = NWebRTCPipeline::envFlag("KWIN_WEBRTC_CONTINUOUS");
    return value;
}

// KWIN_WEBRTC_HDR_BLEND=pq: blend an HDR output directly on PQ code values.
// Skips the conversion pass but darkens translucency and fades; see
// WebRTCOutput::setHdr.
static bool s_hdrBlendPqEnv()
{
    static const bool value = [] {
        const char *v = NWebRTCPipeline::envValue("KWIN_WEBRTC_HDR_BLEND");
        return v && strcasecmp(v, "pq") == 0;
    }();
    return value;
}

// KWIN_WEBRTC_PRESENT_MODE=adaptive (default) | vsync: how this output paces
// compositing when nothing physical requires a vblank grid.
//
// vsync is what a real display gets: the render loop runs in
// PresentationMode::VSync against a SoftwareVsyncMonitor, and a damage event
// arriving just after a composite started waits for the next slot of a grid
// that, here, exists only in software.
//
// adaptive is KWin's own scheduling for variable-refresh displays and does what
// this output actually wants: RenderLoopPrivate::scheduleRepaint puts the next
// presentation at max(now, last + interval), so damage on an idle loop composes
// immediately and continuous damage composes exactly once per interval, with
// the freshest content. The interval is the loop's refresh rate -- the mode's
// (60 Hz by default) or KWIN_WEBRTC_CAPTURE_HZ -- and it is the pacing of the
// stream.
//
// It works together with frameCallbackDelay(): adaptive scheduling lets a late
// client commit compose at once instead of waiting for a slot.
//
// Presentation is reported with the frame's TARGET time, not the moment the
// composite finished. AdaptiveSync starts the composite expectedCompositingTime
// early (render estimate + 1 ms), so a fast render would otherwise stamp the
// previous presentation early, pull the next one forward by the same amount,
// and land two submits less than an interval apart -- inside the throttle
// window of WebRTCEglBackend::captureFrame, which then drops a frame that was
// already composed and loses a whole interval. Stamping the target keeps consecutive
// presentations exactly one interval apart, which is what the throttle was
// designed around.
static bool s_adaptivePresentEnv()
{
    static const bool value = [] {
        const char *v = NWebRTCPipeline::envValue("KWIN_WEBRTC_PRESENT_MODE");
        return !(v && strcasecmp(v, "vsync") == 0);
    }();
    return value;
}

// KWIN_WEBRTC_CALLBACK_LEAD_MS: how far ahead of the next compose the deferred
// frame callbacks go out in adaptive mode (see frameCallbackDelay()). Negative
// disables the deferral. The default, 2 ms, covers the millisecond timer
// rounding and a client's own draw.
static std::chrono::nanoseconds s_callbackLeadEnv()
{
    static const std::chrono::nanoseconds value = [] {
        const char *v = NWebRTCPipeline::envValue("KWIN_WEBRTC_CALLBACK_LEAD_MS");
        const double ms = v ? atof(v) : 2.0;
        return std::chrono::nanoseconds(static_cast<int64_t>(ms * 1'000'000.0));
    }();
    return value;
}

static bool s_callbackBoundedEnv()
{
    static const bool value = [] {
        const char *v = NWebRTCPipeline::envValue("KWIN_WEBRTC_CALLBACK_ESTIMATE");
        return !(v && strcasecmp(v, "predicted") == 0);
    }();
    return value;
}

WebRTCOutput::WebRTCOutput(WebRTCBackend *parent, const std::optional<QString> &connectorName)
    : BackendOutput()
    , m_backend(parent)
    , m_vsyncMonitor(SoftwareVsyncMonitor::create())
{
    connect(m_vsyncMonitor.get(), &VsyncMonitor::vblankOccurred, this, &WebRTCOutput::vblank);
    if (s_adaptivePresentEnv()) {
        renderLoop()->setPresentationMode(PresentationMode::AdaptiveSync);
        WEBRTC_LOG("output: presentation mode adaptive (compose on damage, one per interval), frame callback lead %.1f ms",
                   s_callbackLeadEnv().count() / 1e6);
    } else {
        WEBRTC_LOG("output: presentation mode vsync (software vblank grid)");
    }

    static int identifier = -1;
    m_identifier = ++identifier;
    setInformation(Information{
        .name = connectorName.value_or(QStringLiteral("WEBRTC-%1").arg(identifier)),
        .physicalSize = QSize(),
        .edid = Edid{},
        // HDR only when the peer asks for it (setHdr). The capability lets clients see
        // an HDR output once it is one.
        .capabilities = Capability::HighDynamicRange | Capability::WideColorGamut,
        .panelOrientation = OutputTransform::Kind::Normal,
        .internal = false,
        .mstPath = QByteArray(),
    });
}

bool WebRTCOutput::setHdr(bool on, uint32_t referenceNits, uint32_t maxNits, double minNits)
{
    if (on && m_hdrUnavailable) {
        return false;
    }
    if (on == m_state.highDynamicRange
        && (!on || (m_state.referenceLuminance == referenceNits && m_state.colorDescription->maxHdrLuminance() == double(maxNits)))) {
        return false;
    }
    State next = m_state;
    next.highDynamicRange = on;
    next.wideColorGamut = on;
    std::shared_ptr<ColorDescription> colour = ColorDescription::sRGB;
    if (on) {
        next.referenceLuminance = referenceNits;
        colour = std::make_shared<ColorDescription>(Colorimetry::BT2020, TransferFunction(TransferFunction::PerceptualQuantizer),
                                                    double(referenceNits), minNits, double(maxNits), double(maxNits));
    }
    next.originalColorDescription = colour;
    next.colorDescription = colour;
    // Blend in gamma 2.2 over the output's luminance range, as KWin's DRM
    // backend does (DrmOutput::tryKmsColorOffloading): alpha blending on PQ
    // code values makes every translucent edge, shadow and fade far too dark.
    // The layer composes into a shadow buffer in this space and converts it
    // to PQ in one pass (WebRTCEglLayer::endFrame); it does so whenever the
    // two descriptions differ, so SDR (both sRGB, the same object) has no pass.
    //
    // The range starts at the output's black, not at 0 as in the DRM
    // backend. KWin's black point compensation maps every surface's black to
    // that level (0.005 nits: the PQ transfer function's own minimum), and in
    // a gamma 2.2 that starts at 0 such a black is a code of 0.0039 instead
    // of 0, so whatever is blended over it comes out lighter than on SDR.
    // From the black level up, gamma 2.2 is a pure power law of the values
    // SDR blends, scaled: grey mixes come out exactly as on an SDR output.
    // Saturated ones mix in the BT.2020 container, as on KWin's DRM HDR.
    //
    // layerBlendingColor stays the encoding: there is no KMS blending stage
    // after the layer here, the buffer goes to the peer as it is.
    const double black = std::max(colour->minLuminance(), colour->transferFunction().minLuminance);
    next.blendingColor = (on && !s_hdrBlendPqEnv())
        ? colour->withTransferFunction(TransferFunction(TransferFunction::gamma22, black, double(maxNits)))
        : colour;
    next.layerBlendingColor = colour;
    setState(next);
    Q_EMIT changed();
    return true;
}

void WebRTCOutput::hdrUnavailable()
{
    m_hdrUnavailable = true;
    setHdr(false, 0, 0, 0);
}

WebRTCOutput::~WebRTCOutput()
{
}

std::expected<void, OutputError> WebRTCOutput::testPresentation(const std::shared_ptr<OutputFrame> &frame)
{
    return {};
}

std::expected<void, OutputError> WebRTCOutput::present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame)
{
    m_frame = frame;
    if (s_adaptivePresentEnv()) {
        // Nothing to wait for on a headless output: the frame is "presented" as
        // soon as it exists. Queued rather than inline because present() runs
        // inside Compositor::composite(), between prepareNewFrame() and
        // newFramePrepared(), and RenderLoopPrivate::scheduleNextRepaint()
        // refuses to schedule while preparingNewFrame is set -- an inline
        // presented() would complete the frame but never arm the next one.
        QMetaObject::invokeMethod(this, &WebRTCOutput::completeAdaptiveFrame, Qt::QueuedConnection);
    } else {
        m_vsyncMonitor->arm();
    }

    // Damage-driven by default: the render loop goes idle between scene
    // changes exactly like stock KWin on a real display, and window/surface
    // commits wake it through the ordinary OutputLayer::scheduleRepaint()
    // path. Rescheduling every tick would make an idle desktop composite
    // (makeCurrent, swapchain acquire, prePaint, glFlush) at the full render
    // rate with no frame sent. The consumers that need extra composites
    // request them: the throttle's deferred-damage tick reschedules itself
    // in WebRTCEglBackend::captureFrame, and StreamStart/resize go through
    // armForcedCapture()/kickRepaint().
    // KWIN_WEBRTC_CONTINUOUS=1 composes on every tick instead.
    if (m_continuousMode && s_forceContinuousEnv()) {
        // scheduleRepaint(nullptr) keeps needsRepaint() true WITHOUT
        // injecting fake damage (deviceRepaints() stays empty), so the
        // damage-gated capture check in WebRTCEglLayer::endFrame still
        // sees only real scene damage (adding device repaints here would
        // defeat that gate).
        if (m_layer) {
            m_layer->scheduleRepaint(nullptr);
        }
        renderLoop()->scheduleRepaint();
    }

    if (m_cursorLayer) {
        static_cast<WebRTCCursorLayer *>(m_cursorLayer.get())->pushPresentState();
    }

    return {};
}

std::expected<void, OutputError> WebRTCOutput::presentAsync(OutputLayer *layer, std::optional<std::chrono::nanoseconds> allowedVrrDelay)
{
    Q_UNUSED(allowedVrrDelay);
    if (layer != m_cursorLayer.get() || !static_cast<WebRTCCursorLayer *>(layer)->notifyAsyncMove()) {
        return std::unexpected(OutputError{
            .code = OutputErrorCode::Other,
            .message = QStringLiteral("only the cursor layer moves asynchronously"),
        });
    }
    return {};
}

void WebRTCOutput::init(const QPoint &logicalPosition, const QSize &pixelSize, qreal scale,
                        const QList<OutputModeline> &modes)
{
    QList<std::shared_ptr<OutputMode>> modeList;
    for (const auto &mode : modes) {
        modeList.push_back(std::make_shared<OutputMode>(mode));
    }
    if (modeList.empty()) {
        modeList.push_back(std::make_shared<OutputMode>(OutputModeline(pixelSize, 60000, OutputModeline::Flag::Preferred)));
    }

    applyRenderRate(modeList.front()->refreshRate());
    setState(State{
        .position = logicalPosition,
        .scale = scale,
        .modes = modeList,
        .currentMode = modeList.front(),
        .scaleSetting = scale,
    });

}

void WebRTCOutput::applyChanges(const OutputConfiguration &config)
{
    auto props = config.constChangeSet(this);
    if (!props) {
        return;
    }

    // Debug: log what's being applied
    const QSize desiredSize = props->desiredMode ? props->desiredMode->size() : QSize();
    const QSize requestedSize = props->currentMode ? props->currentMode->size() : QSize();
    WEBRTC_LOG("output: applyChanges desiredSize=%dx%d mode=%dx%d currentMode=%dx%d",
               desiredSize.width(), desiredSize.height(),
               requestedSize.width(), requestedSize.height(),
               m_state.currentMode ? m_state.currentMode->size().width() : 0,
               m_state.currentMode ? m_state.currentMode->size().height() : 0);

    Q_EMIT aboutToChange(props.get());

    State next = m_state;
    next.enabled = props->enabled.value_or(m_state.enabled);
    next.transform = props->transform.value_or(m_state.transform);
    next.position = props->pos.value_or(m_state.position);
    next.scale = props->scale.value_or(m_state.scale);
    next.scaleSetting = props->scaleSetting.value_or(m_state.scaleSetting);
    // Ignore desiredMode, currentMode and customModes from external output
    // management (kscreen): the streaming backend controls the mode, and calls
    // changeMode() itself when the peer requests a resolution. This keeps
    // kscreen from requesting a saved resolution that conflicts with the
    // client-requested one.
    if (!next.currentMode) {
        next.currentMode = next.modes.front();
    }
    next.uuid = props->uuid.value_or(m_state.uuid);
    next.replicationSource = props->replicationSource.value_or(m_state.replicationSource);
    next.priority = props->priority.value_or(m_state.priority);
    next.deviceOffset = props->deviceOffset.value_or(m_state.deviceOffset);
    setState(next);
    applyRenderRate(next.currentMode->refreshRate());

    Q_EMIT changed();
}

void WebRTCOutput::vblank(std::chrono::nanoseconds timestamp)
{
    if (m_frame) {
        m_frame->presented(timestamp, PresentationMode::VSync);
        m_frame.reset();
    }
}

void WebRTCOutput::completeAdaptiveFrame()
{
    if (!m_frame) {
        return;
    }
    // See s_adaptivePresentEnv(): the target, unless the composite overran it.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto target = m_frame->targetPageflipTime().time_since_epoch();
    m_frame->presented(std::max(now, target), PresentationMode::AdaptiveSync);
    m_frame.reset();
}

std::chrono::nanoseconds WebRTCOutput::frameCallbackDelay() const
{
    // Without a real vblank the compositor sends wl_surface.frame right after a
    // compose, so a client that redraws on the callback commits at the very start
    // of the interval and its frame waits almost a full interval for the next
    // compose. Hold the callbacks until shortly before that compose is due: the
    // client draws, commits, and the compose picks the buffer up a few ms later.
    // Compose-to-send age is unchanged; it is the commit-to-compose wait that
    // shrinks. The lead covers the client's own render time and timer jitter;
    // there is no cliff if it is too small, because under adaptive scheduling a
    // commit that arrives after the slot composes at once instead of waiting a
    // whole grid period -- which is why this deferral is tied to that mode.
    if (!s_adaptivePresentEnv() || s_callbackLeadEnv() < std::chrono::nanoseconds::zero()) {
        return std::chrono::nanoseconds::zero();
    }
    const auto interval = std::chrono::nanoseconds(1'000'000'000'000) / renderLoop()->refreshRate();
    // From the end of this compose (when the callback would go out) to the start of the
    // next one: one interval minus the render loop's own head start (render estimate + 1 ms).
    // The prediction's variance margin is bounded here: one slow composite keeps
    // predictedRenderTime() high enough to clamp this delay to zero for a long time,
    // which puts every client's draw right after the compose. The margin serves the
    // render loop's deadline, not this estimate.
    // KWIN_WEBRTC_CALLBACK_ESTIMATE=predicted selects the unbounded
    // predictedRenderTime().
    const auto estimate = s_callbackBoundedEnv()
        ? std::min(renderLoop()->predictedRenderTime(), renderLoop()->averageRenderTime() + std::chrono::milliseconds(2))
        : renderLoop()->predictedRenderTime();
    const auto nextComposeIn = interval - estimate - std::chrono::milliseconds(1);
    return std::clamp(nextComposeIn - s_callbackLeadEnv(), std::chrono::nanoseconds::zero(), interval);
}

void WebRTCOutput::setOutputLayer(std::unique_ptr<OutputLayer> &&layer)
{
    m_layer = std::move(layer);
}

OutputLayer *WebRTCOutput::outputLayer() const
{
    return m_layer.get();
}

void WebRTCOutput::setCursorLayer(std::unique_ptr<OutputLayer> &&layer)
{
    m_cursorLayer = std::move(layer);
}

OutputLayer *WebRTCOutput::cursorLayer() const
{
    return m_cursorLayer.get();
}

void WebRTCOutput::kickRepaint()
{
    if (m_layer) {
        m_layer->scheduleRepaint(nullptr);
    }
    renderLoop()->scheduleRepaint();
}

void WebRTCOutput::setContinuousMode(bool enabled)
{
    m_continuousMode = enabled;
    if (enabled) {
        // Kick one full composite so the pipeline's pendingForcedCapture is
        // consumed even on a perfectly static desktop.
        kickRepaint();
    }
}

void WebRTCOutput::changeMode(const QSize &pixelSize)
{
    if (pixelSize.isEmpty())
        return;

    // New resolution, preserving the current refresh rate.
    applySingleMode(pixelSize, m_state.currentMode ? m_state.currentMode->refreshRate() : 60000);
}

void WebRTCOutput::changeRefreshRate(uint32_t fps)
{
    if (fps < 15 || fps > 240)
        return;

    // Same resolution, new refresh rate.
    applySingleMode(m_state.currentMode ? m_state.currentMode->size() : QSize(1920, 1080),
                    static_cast<uint64_t>(fps) * 1000);
}

void WebRTCOutput::applySingleMode(const QSize &pixelSize, uint64_t refreshMilliHz)
{
    auto newMode = std::make_shared<OutputMode>(OutputModeline(pixelSize, static_cast<uint32_t>(refreshMilliHz), OutputModeline::Flag::Preferred));

    QList<std::shared_ptr<OutputMode>> modeList;
    modeList.push_back(newMode);

    applyRenderRate(newMode->refreshRate());

    // Preserve all existing state except modes — this ensures position,
    // scale, enabled, etc. remain intact so that plasmashell repositions
    // its panels correctly when the output resolution changes.
    State next = m_state;
    next.modes = modeList;
    next.currentMode = newMode;
    setState(next);

    // Emit changed() so plasmashell and workspace recalculate geometry
    Q_EMIT changed();
    syncLogicalOutput();
}

void WebRTCOutput::applyRenderRate(uint32_t modeRefreshMilliHz)
{
    const uint32_t rate = m_captureHz ? m_captureHz * 1000u : modeRefreshMilliHz;
    renderLoop()->setRefreshRate(static_cast<int>(rate));
    m_vsyncMonitor->setRefreshRate(static_cast<int>(rate));
}

void WebRTCOutput::setCaptureRefreshRate(uint32_t hz)
{
    // 15..480 Hz, or 0 for "follow the mode".
    if (hz != 0 && (hz < 15 || hz > 480))
        return;
    if (hz == m_captureHz)
        return;

    m_captureHz = hz;
    applyRenderRate(m_state.currentMode ? m_state.currentMode->refreshRate() : 60000);
    WEBRTC_LOG("output: capture refresh rate set to %u Hz (mode stays %u mHz)", hz,
               m_state.currentMode ? m_state.currentMode->refreshRate() : 60000u);
}

void WebRTCOutput::setPosition(const QPoint &pos)
{
    State next = m_state;
    next.position = pos;
    setState(next);
    Q_EMIT changed();
    syncLogicalOutput();
}

void WebRTCOutput::setScale(qreal scale)
{
    State next = m_state;
    next.scale = scale;
    // scaleSetting is what output management reads back; leaving it behind
    // would make the next kscreen round trip revert the change.
    next.scaleSetting = scale;
    setState(next);
    Q_EMIT changed();
    syncLogicalOutput();
}

void WebRTCOutput::syncLogicalOutput()
{
    // Since KWin 6.7 a LogicalOutput no longer follows its BackendOutput's
    // changed(): the workspace copies geometry, mode and scale across only
    // after applyOutputConfiguration() or a hotplug. These setters are
    // neither -- the peer asked for them -- so without this the windows and
    // the shell keep laying out against the previous size.
    if (Workspace *ws = workspace()) {
        ws->syncLogicalOutputs();
    }
}

} // namespace KWin

#include "moc_webrtc_output.cpp"
