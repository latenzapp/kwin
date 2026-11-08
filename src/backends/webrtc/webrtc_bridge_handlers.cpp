/*
    KWin WebRTC Backend - the bridge's callback surface

    The methods CBeamBridge calls into the backend with -- a client arriving or
    going, a stream starting or stopping, an output added, removed or
    reconfigured -- and the two the backend calls back out with, which carry
    the decoupled cursor.

    They live in their own file but belong to the same class as
    webrtc_egl_backend.cpp, deliberately: they touch the per-output pipelines
    directly and making them free functions would mean passing that state
    around. What they are not is the rest of the backend -- EGL setup, output
    topology, frame submission.

    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Aug-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_egl_backend.h"

#include "webrtc_backend.h"
#include "webrtc_cursor_layer.h"
#include "webrtc_egl_layer.h"
#include "webrtc_input_backend.h"
#include "webrtc_logging.h"
#include "webrtc_output.h"

#include "pipeline/CBeamBridge.hpp"
#include "pipeline/WebRTCLog.hpp"
#include "kwin_bridge_protocol.h"

#include <QMetaObject>

#include <algorithm>
#include <string>

using namespace NStreaming;

namespace KWin
{

void WebRTCEglBackend::onBridgeClientConnected()
{
    WEBRTC_INFO("beam-host connected");
    QMetaObject::invokeMethod(this, [this]() {
        // A fresh client starts with an empty DMABUF import cache, so every
        // buffer must be introduced to it again. Any id still marked as
        // announced -- to the client that just went away -- would otherwise
        // reach this one as a frame carrying no descriptors and nothing to
        // resolve them against, and it would take a round trip through
        // BufferDrop to recover each buffer. Cheap to get right here.
        for (auto &[output, pipeline] : m_pipelines) {
            auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
            if (auto *layer = static_cast<WebRTCEglLayer *>(webrtcOutput->outputLayer())) {
                layer->reclaimAllLoans();
                layer->resetBufferIds();
            }
        }

        broadcastOutputList();
        tryWireInput();

        // Clipboard is not carried over the bridge.
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeClientDisconnected()
{
    WEBRTC_INFO("beam-host disconnected -- stopping all outputs");
    QMetaObject::invokeMethod(this, [this]() {
        for (auto &[output, pipeline] : m_pipelines) {
            pipeline.streamActive = false;
            auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
            // The peer that owed us every outstanding buffer is gone. Take
            // them all back, or the swapchain slots stay pinned forever and
            // the desktop stops updating.
            if (auto *layer = static_cast<WebRTCEglLayer *>(webrtcOutput->outputLayer()))
                layer->reclaimAllLoans();
            if (webrtcOutput->isContinuousMode())
                webrtcOutput->setContinuousMode(false);
        }
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeStreamStart(uint16_t outputIdx, uint32_t codecBit, uint32_t fps,
                                            uint32_t startKbps, uint32_t maxKbps, uint32_t mode)
{
    Q_UNUSED(maxKbps);  // no rate control in this process
    QMetaObject::invokeMethod(this, [this, outputIdx, codecBit, fps, startKbps, mode]() {
        SOutputPipeline *pipeline = findPipelineByIdx(outputIdx);
        if (!pipeline) {
            WEBRTC_ERR("StreamStart for unknown output %u", outputIdx);
            return;
        }

        // dmabuf is the only transport this compositor implements; a bitstream
        // request gets no frames. Say so once, as an error, because the
        // alternative is a black stream with an empty log.
        if (mode != kwin_bridge::kStreamModeDmabuf) {
            WEBRTC_ERR("StreamStart output=%u requested the bitstream transport, which this "
                       "compositor no longer implements (the encoder moved to beam-host). "
                       "Update beam-host; this stream will produce no frames.",
                       outputIdx);
            return;
        }
        // A restart must not inherit the previous session's loans: nobody is
        // left to return them and they would pin swapchain slots.
        //
        // Nor its buffer IDS: the peer drops its DMABUF import cache when a
        // stream stops, so every buffer must be re-announced with its
        // descriptors on the next stream -- between two streams of one client
        // nothing reconnects, so the reset on connect does not cover it.
        if (auto *layer = layerForIdx(outputIdx)) {
            layer->reclaimAllLoans();
            layer->resetBufferIds();
        }

        std::string codec = codecBit == kwin_bridge::kCodecH265 ? "hevc"
                           : codecBit == kwin_bridge::kCodecAv1  ? "av1"
                                                                  : "h264";
        uint32_t bitrateBps = startKbps > 0 ? startKbps * 1000 : pipeline->config.bitrateBps;

        // Codec and bitrate are recorded, not acted on: this process does not
        // encode. They are kept because the trace lines and getOutputInfos()
        // report them.
        pipeline->config.codec = codec;
        pipeline->config.bitrateBps = bitrateBps;

        auto *webrtcOutput = static_cast<WebRTCOutput *>(findOutputByIdx(outputIdx));

        // Compared against the OUTPUT's own rate too, not only config.fps: the
        // output starts at a fixed 60 Hz mode, and the two are not guaranteed
        // to match.
        const bool outputRateDiffers =
            webrtcOutput && webrtcOutput->refreshRate() != fps * 1000u;
        if (fps > 0 && (fps != pipeline->config.fps || outputRateDiffers)) {
            pipeline->config.fps = fps;
            if (webrtcOutput) webrtcOutput->changeRefreshRate(fps);
        }

        pipeline->streamActive = true;
        // A stream that stopped and restarted must not be throttled against
        // the timestamp of the previous session's last frame, and must not
        // inherit its deferred-damage flag either.
        pipeline->pendingDeferredCapture = false;
        pipeline->nextSendDue = {};
        if (webrtcOutput && !webrtcOutput->isContinuousMode())
            webrtcOutput->setContinuousMode(true);
        // The owed first frame, armed AFTER streamActive is set and
        // unconditionally -- not as a side effect of the continuous-mode
        // transition above -- so the first frame is sent even on a static
        // desktop and even if continuous mode was already on.
        if (webrtcOutput) armForcedCapture(webrtcOutput);
        WEBRTC_LOG("StreamStart output=%u codec=%s fps=%u bitrateKbps=%u", outputIdx,
                   pipeline->config.codec.c_str(), pipeline->config.fps, bitrateBps / 1000);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeStreamStop(uint16_t outputIdx)
{
    QMetaObject::invokeMethod(this, [this, outputIdx]() {
        SOutputPipeline *pipeline = findPipelineByIdx(outputIdx);
        if (!pipeline) return;
        // Nobody is going to return outstanding loans once the stream is
        // down, and a pinned swapchain slot would freeze the output.
        if (auto *layer = layerForIdx(outputIdx))
            layer->reclaimAllLoans();
        pipeline->streamActive = false;
        auto *webrtcOutput = static_cast<WebRTCOutput *>(findOutputByIdx(outputIdx));
        if (webrtcOutput && webrtcOutput->isContinuousMode())
            webrtcOutput->setContinuousMode(false);
        WEBRTC_LOG("StreamStop output=%u", outputIdx);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeSetFps(uint16_t outputIdx, uint32_t fps)
{
    QMetaObject::invokeMethod(this, [this, outputIdx, fps]() {
        changeFps(fps, outputIdx);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeOutputAdd(uint32_t width, uint32_t height, int32_t x, int32_t y,
                                          uint32_t fps)
{
    QMetaObject::invokeMethod(this, [this, width, height, x, y, fps]() {
        addVirtualOutput(width, height, x, y, fps);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeOutputRemove(uint16_t outputIdx)
{
    QMetaObject::invokeMethod(this, [this, outputIdx]() {
        removeVirtualOutput(outputIdx);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeOutputConfigure(uint16_t outputIdx, uint32_t width, uint32_t height,
                                                bool hasPos, int32_t x, int32_t y, uint32_t fps)
{
    QMetaObject::invokeMethod(this, [this, outputIdx, width, height, hasPos, x, y, fps]() {
        configureOutput(outputIdx, width, height, hasPos, x, y, fps);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::onBridgeOutputSetScale(uint16_t outputIdx, double scale)
{
    QMetaObject::invokeMethod(this, [this, outputIdx, scale]() {
        setOutputScale(outputIdx, scale);
    }, Qt::QueuedConnection);
}

void WebRTCEglBackend::sendCursorImage(uint32_t outputIdx, uint32_t width, uint32_t height,
                                        int32_t hotspotX, int32_t hotspotY, const uint8_t *rgba,
                                        size_t rgbaLen)
{
    if (m_bridge) {
        m_bridge->sendCursorImage(static_cast<uint16_t>(outputIdx), width, height, hotspotX,
                                   hotspotY, rgba, rgbaLen);
    }
}

void WebRTCEglBackend::sendCursorPosition(uint32_t outputIdx, int32_t x, int32_t y,
                                           int32_t hotspotX, int32_t hotspotY, bool visible)
{
    if (m_bridge) {
        m_bridge->sendCursorPos(static_cast<uint16_t>(outputIdx), x, y, hotspotX, hotspotY, visible);
    }
}

} // namespace KWin
