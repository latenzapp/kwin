/*
    KWin WebRTC Backend - EGL Backend
    Headless compositing: GBM buffer → DMABUF → CBeamBridge → the peer

    The per-output layer that does the loaning is WebRTCEglLayer, in
    webrtc_egl_layer.h. What is here is the backend: the EGL/GBM context, the
    per-output pipelines, the bridge's callback surface, and output topology.

    Nothing is encoded in this process; the peer, a separate program,
    imports the loaned buffer and returns it through BufferRelease.

    Multi-monitor: each output has its own stream lifecycle
    (StreamStart/StreamStop) and its own loan table. Transport is a single
    CBeamBridge unix socket shared by all outputs; frames carry their output
    index in the bridge protocol's header (see kwin_bridge_protocol.h) rather
    than being encoded in-band, so there is no wire-format coupling between
    outputs.

    Based on KWin's VirtualEglBackend:
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "opengl/eglbackend.h"
#include "utils/damagejournal.h"

// The per-output layer this backend creates. Its own file;
// included here rather than forward-declared because compatibleOutputLayers
// and addOutput construct one.
#include "webrtc_egl_layer.h"

#include "pipeline/CBeamBridge.hpp"

#include <chrono>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <map>
#include <vector>

namespace KWin
{

class EglSwapchainSlot;
class EglSwapchain;
class GraphicsBufferAllocator;
class WebRTCBackend;
class GLFramebuffer;
class GLTexture;
class WebRTCEglBackend;
class GLRenderTimeQuery;

/**
 * @brief OpenGL Backend using Egl on a GBM surface with WebRTC frame capture.
 *
 * Extends the VirtualEglBackend pattern: after each composite the GBM
 * buffer's DMABUF is loaned to the peer, a separate process.
 *
 * Multi-monitor: each output gets its own SOutputPipeline (stream lifecycle,
 * throttle state and loan table). Transport is shared.
 */
class WebRTCEglBackend : public EglBackend
{
    Q_OBJECT

public:
    WebRTCEglBackend(WebRTCBackend *b, RenderDevice *renderDevice);
    ~WebRTCEglBackend() override;

    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;
    bool init() override;

    WebRTCBackend *backend() const;

    // ── WebRTC Pipeline ──────────────────────────────────────────────────

    struct SWebRTCConfig {
        uint32_t    width           = 1920;
        uint32_t    height          = 1080;
        uint32_t    fps             = 60;
        uint32_t    bitrateBps      = 8000000; // default bitrate
        std::string codec           = "h264";
    };

    // ── Per-output stream state ──────────────────────────────────────────
    struct SOutputPipeline {
        uint32_t outputIdx = 0;
        SWebRTCConfig config;
        // Whether the peer has sent StreamStart for this output (gates
        // captureFrame).
        bool streamActive = false;
        // Damage-gated capture (see WebRTCEglLayer::endFrame): captureFrame is normally
        // skipped when the scene reports no real damage, so an idle desktop
        // costs no loan and no cross-process import. This forces the NEXT
        // captureFrame through regardless of damage -- set on StreamStart
        // and after a resolution change, both of which invalidate whatever
        // the damage journal thinks is "unchanged": the peer has no
        // previous frame to build on.
        bool pendingForcedCapture = true;

        // Until when we keep waking the render loop to deliver that forced
        // capture.
        //
        // One composite is one chance: if that tick cannot deliver -- the
        // stream flag not yet visible on this thread, no current buffer yet,
        // a capture that returned no loan -- the loop goes back to sleep and
        // the first frame would wait for the next unrelated damage, which on
        // a static desktop can be arbitrarily late.
        //
        // So the owed frame is a DEBT with a deadline rather than one attempt.
        // Bounded because an output that cannot compose at all must not spin
        // for ever: after this, the ordinary damage-driven behaviour resumes.
        std::chrono::steady_clock::time_point forcedCaptureDeadline{};

        // Damage that the submit throttle refused, not damage that never
        // happened. Deliberately NOT pendingForcedCapture: that flag bypasses
        // the throttle (it exists for cases with no valid last frame at all),
        // and reusing it here would make every throttled frame re-enter on the
        // next tick, turning the fps cap into a no-op. This one only re-arms
        // the damage gate, so the change is picked up as soon as the throttle
        // window opens instead of being lost until the next scene damage --
        // which on a desktop that just went still can be a while.
        bool pendingDeferredCapture = false;

        // What has changed on this output since the last frame we actually
        // SENT -- not since the last composite. A throttled or loan-capped
        // tick returns without sending, and its damage has to survive to the
        // tick that does, or the damage rectangles sent with the frame would
        // not cover everything that changed.
        //
        // Empty is a real answer here: it means the next send is a keep-alive
        // with nothing new in it. "We do not know" is `damageUnknown`, which
        // is what a resize or a forced capture leaves behind.
        Region pendingDamage;
        bool damageUnknown = true;

        // Per-output stats
        struct SStreamStats {
            // Frames handed to the peer.
            uint64_t framesEncoded    = 0;
            uint64_t framesDropped    = 0;
            // Captures the submit throttle refused because they arrived
            // faster than the negotiated fps. Expected to be large and
            // harmless whenever the capture refresh rate runs above fps;
            // it is the count of composites that cost nothing downstream.
            uint64_t framesThrottled  = 0;
        } stats;
        uint64_t frameCounter = 0;
        // When the next frame is due to the peer (the submit throttle's
        // schedule, see captureFrame); default-constructed = no schedule yet.
        std::chrono::steady_clock::time_point nextSendDue{};

        uint32_t nextDmabufFrameId = 1;
        // Frames the loan cap refused. Distinct from framesDropped (bridge
        // send failures): a nonzero value here in steady state means
        // the peer is not returning buffers fast enough, which is a finding
        // about the peer, not about this process.
        uint64_t framesLoanCapped = 0;
        uint64_t loansExpired = 0;
        // Sampling state for the every-60-frames dmabuf trace.
        std::chrono::steady_clock::time_point dmabufTraceAt{};
        uint64_t dmabufTraceSent = 0;
        uint64_t dmabufTraceEnd = 0;
    };

    // ── Output info for client ────────────────────────────────────────────
    struct SOutputInfo {
        uint32_t idx;
        uint32_t width;
        uint32_t height;
        int32_t  x;     // logical position
        int32_t  y;
        uint32_t fps;
        std::string codec;
        uint32_t bitrateBps;
    };

    // The send half of captureFrame, after the throttle.
    bool sendDmabufFrame(SOutputPipeline *pipeline, const struct DmaBufAttributes *dma,
                         WebRTCEglLayer *layer, bool forced);

    // Fills the frame's damage tail from pipeline->pendingDamage and resets
    // it. See the definition for what the three possible answers mean.
    void fillDamagePayload(SOutputPipeline *pipeline, bool forced,
                           kwin_bridge::DmabufFramePayload &payload);

    bool initPipeline(const SWebRTCConfig &config);
    void shutdownPipeline();
    // True if at least one output has an active stream to the peer.
    bool isStreaming() const;

    // Per-output settings changes
    bool changeBitrate(uint32_t newBitrateBps, uint32_t outputIdx = 0);
    bool changeResolution(uint32_t width, uint32_t height, uint32_t outputIdx = 0);
    bool changeFps(uint32_t fps, uint32_t outputIdx = 0);

    // Called by WebRTCEglLayer::endFrame() to capture the buffer for a specific output.
    // `bypassThrottle` skips the min-interval submit gate; pass what
    // consumeForcedCapture() returned, never what consumeDeferredCapture() did.
    // Returns true when the frame was LOANED to the peer: the caller must
    // then NOT release the swapchain slot, because the BufferRelease handler
    // will. False means the frame was refused (throttled, loan cap, send
    // failure) and the slot is the caller's to release immediately.
    // `damage` is what the scene actually changed this frame, in device
    // coordinates -- the same region WebRTCEglLayer::endFrame gates on. It is
    // passed through to the peer with the frame; the compositor draws no
    // conclusion from it.
    bool captureFrame(BackendOutput *output, const struct DmaBufAttributes *dma,
                      bool bypassThrottle, WebRTCEglLayer *layer, const Region &damage);

    // Marshalled onto the compositor thread from the bridge's recv thread.
    // Takes ownership of fenceFd.
    void handleBufferRelease(uint16_t outputIdx, uint32_t bufferId, uint32_t frameId, int fenceFd);
    // The peer asked us to forget a bufferId (it cannot resolve it).
    void handleBufferForget(uint16_t outputIdx, uint32_t bufferId);
    // Tells the peer a buffer id is dead, so it drops its import-cache entry.
    void sendBufferDrop(uint32_t outputIdx, uint32_t bufferId);
    // Bridge output index for `output`, 0 if it has no pipeline.
    uint32_t outputIndexFor(BackendOutput *output);

    // Damage-gated capture (see WebRTCEglLayer::endFrame): true exactly once per
    // StreamStart/resolution-change, false otherwise -- callers should
    // capture unconditionally when this returns true, and gate on real
    // scene damage the rest of the time. Not idempotent: clears the flag.
    bool consumeForcedCapture(BackendOutput *output);

    // Same shape, for damage the submit throttle deferred (see
    // SOutputPipeline::pendingDeferredCapture). Re-arms the damage gate only:
    // the resulting capture still has to clear the throttle.
    bool consumeDeferredCapture(BackendOutput *output);

    // Arm the owed first frame: the flag, plus the deadline until which the
    // render loop keeps being woken to deliver it. Every caller that sets
    // pendingForcedCapture should go through here, so nobody arms a debt with
    // no way to collect it.
    void armForcedCapture(BackendOutput *output);

    // Is a forced capture still owed and still worth waking the loop for?
    // Asked by the layer after a composite that did not deliver one.
    bool owesForcedCapture(BackendOutput *output) const;

    // Put the flag back after a composite that could not deliver, WITHOUT
    // touching the deadline: extending it on every retry would turn the bound
    // into an unbounded spin, which is the failure this whole mechanism is
    // supposed to be safe from.
    void reArmForcedCapture(BackendOutput *output);

    // ── Multi-monitor management ─────────────────────────────────────────
    // Add a new virtual output alongside existing ones
    BackendOutput *addVirtualOutput(uint32_t width, uint32_t height,
                                     int32_t x, int32_t y, uint32_t fps = 60,
                                     const std::string &codec = "",
                                     uint32_t bitrateBps = 0);
    // Remove a virtual output by its index
    bool removeVirtualOutput(uint32_t outputIdx);
    // Reconfigure an existing output
    bool configureOutput(uint32_t outputIdx, uint32_t width, uint32_t height,
                         bool hasPos, int32_t x, int32_t y, uint32_t fps = 0,
                         const std::string &codec = "", uint32_t bitrateBps = 0);
    // Set an output's DPI scale factor (MsgType::OutputSetScale). Pixel size is
    // untouched: scale only changes the logical space clients are laid out in.
    bool setOutputScale(uint32_t outputIdx, double scale);
    // Get list of all output infos for client
    std::vector<SOutputInfo> getOutputInfos() const;
    // Get the output index for a BackendOutput*
    uint32_t getOutputIndex(BackendOutput *output) const;

    // ── Decoupled cursor (WebRTCCursorLayer) ─────────────────────────────
    void sendCursorImage(uint32_t outputIdx, uint32_t width, uint32_t height, int32_t hotspotX,
                          int32_t hotspotY, const uint8_t *rgba, size_t rgbaLen);
    void sendCursorPosition(uint32_t outputIdx, int32_t x, int32_t y, int32_t hotspotX,
                             int32_t hotspotY, bool visible);

    SWebRTCConfig &config() { return m_pipelineConfig; }

private:
    bool initializeEgl();
    void addOutput(BackendOutput *output);

    // Per-output pipeline management
    void initOutputPipeline(BackendOutput *output, uint32_t outputIdx, const SWebRTCConfig &config);
    void shutdownOutputPipeline(BackendOutput *output);
    SOutputPipeline *findPipeline(BackendOutput *output);
    SOutputPipeline *findPipelineByIdx(uint32_t outputIdx);
    BackendOutput *findOutputByIdx(uint32_t outputIdx);
    // The primary layer of the output with this index, or nullptr: shorthand
    // for findOutputByIdx() -> WebRTCOutput::outputLayer() as WebRTCEglLayer.
    WebRTCEglLayer *layerForIdx(uint32_t outputIdx);

    // Send the current output list to the peer over the bridge (also
    // resyncs the input backend's per-output geometry table).
    void broadcastOutputList();

    // ── CBeamBridge callbacks (invoked from the bridge's socket threads;
    // these all hop onto the main thread via QMetaObject::invokeMethod) ──
    void onBridgeStreamStart(uint16_t outputIdx, uint32_t codec, uint32_t fps,
                              uint32_t startKbps, uint32_t maxKbps, uint32_t mode);
    void onBridgeStreamStop(uint16_t outputIdx);
    void onBridgeSetFps(uint16_t outputIdx, uint32_t fps);
    // ForceIdr, SetBitrate and ResubmitLast: the message types stay in the
    // protocol enum, so existing values are never reused, and CBeamBridge
    // still parses them, but there is nothing here to act on.
    void onBridgeClientConnected();
    void onBridgeClientDisconnected();
    // Dynamic output layout, requested by the peer: thin wrappers around
    // addVirtualOutput/removeVirtualOutput/configureOutput.
    void onBridgeOutputAdd(uint32_t width, uint32_t height, int32_t x, int32_t y, uint32_t fps);
    void onBridgeOutputRemove(uint16_t outputIdx);
    void onBridgeOutputConfigure(uint16_t outputIdx, uint32_t width, uint32_t height, bool hasPos,
                                  int32_t x, int32_t y, uint32_t fps);
    void onBridgeOutputSetScale(uint16_t outputIdx, double scale);

    WebRTCBackend *m_backend;

    // Base pipeline config (default for first output, used as template for new outputs)
    SWebRTCConfig m_pipelineConfig;

    // ── Per-output pipelines ─────────────────────────────────────────────
    std::map<BackendOutput *, SOutputPipeline> m_pipelines;
    uint32_t m_nextOutputIdx = 0; // monotonically increasing output ID

    // ── Bridge to the peer ──────────────────────────────────────────────
    std::unique_ptr<NStreaming::CBeamBridge> m_bridge;

    // Deferred input wiring (input backend may not exist during initPipeline)
    bool m_inputWired = false;
    void tryWireInput();
};

} // namespace KWin
