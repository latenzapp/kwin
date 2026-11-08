/*
    KWin WebRTC Backend - WebRTCEglLayer implementation

    See webrtc_egl_layer.h.

    Based on KWin's VirtualEglLayer:
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Aug-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_egl_layer.h"

#include "webrtc_egl_backend.h"
#include "webrtc_logging.h"

// Not just forward-declared: beginFrame creates the swapchain on the render device.
#include "core/drmdevice.h"
#include "core/renderloop.h"
#include "core/graphicsbuffer.h"
#include "opengl/eglswapchain.h"
#include "opengl/glrendertimequery.h"
#include "opengl/glutils.h"

#include "pipeline/CBeamBridge.hpp"
#include "pipeline/WebRTCLog.hpp"
#include "kwin_bridge_protocol.h"

#include <drm_fourcc.h>

#include <algorithm>

using namespace NStreaming;

namespace KWin
{

// Buffer age is what lets beginFrame() hand back a partial repaint region
// instead of the whole screen. Kept here with its only reader.
static const bool s_bufferAgeEnabled = qEnvironmentVariable("KWIN_USE_BUFFER_AGE") != QStringLiteral("0");

WebRTCEglLayer::WebRTCEglLayer(BackendOutput *output, WebRTCEglBackend *backend)
    : OutputLayer(output, OutputLayerType::Primary)
    , m_backend(backend)
{
}

WebRTCEglLayer::~WebRTCEglLayer()
{
    (void)m_backend->openglContext()->makeCurrent();
}

std::optional<OutputLayerBeginFrameInfo> WebRTCEglLayer::beginFrame(OutputFrame *frame)
{
    if (!m_backend->openglContext()->makeCurrent()) {
        return std::nullopt;
    }

    const QSize nativeSize = m_output->modeSize();
    if (!m_swapchain || m_swapchain->size() != nativeSize) {
        // Every buffer the old swapchain owned is about to stop existing, so
        // every loan on one is unreturnable and every id the peer has cached
        // is stale. Both have to go before the new swapchain hands out
        // pointers that could reuse the same addresses.
        reclaimAllLoans();
        forgetAllBufferIds();
        GraphicsBufferOptions options{
            .size = nativeSize,
            .format = DRM_FORMAT_XRGB8888,
            .modifiers = m_backend->supportedFormats()[DRM_FORMAT_XRGB8888],
            .software = false,
            .scanout = false,
        };
        m_swapchain = EglSwapchain::create(m_backend->renderDevice(), options);
        if (!m_swapchain) {
            return std::nullopt;
        }
    }

    m_current = m_swapchain->acquire();
    if (!m_current) {
        return std::nullopt;
    }

    m_current->framebuffer()->colorAttachment()->setContentTransform(bufferTransform().combine(OutputTransform::FlipY));

    m_query = GLRenderTimeQuery::begin(m_backend->openglContextRef());

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_current->framebuffer()),
        .repaint = s_bufferAgeEnabled ? m_damageJournal.accumulate(m_current->age(), Region::infinite()) : Region::infinite(),
    };
}

/**
 * Once a second, logs how many composites this layer ran against the render
 * loop's configured rate. Printing them together is the point: "composites
 * per second" alone cannot separate "the loop was configured at 60" from "the
 * loop was configured at 30 and ran at 60 anyway", and those have opposite
 * fixes.
 *
 * BEAM_KWIN_CADENCE_LOG=1 to enable; off by default, and off is one relaxed
 * bool test per composite.
 */
void WebRTCEglLayer::reportCadence()
{
    static const bool enabled = qEnvironmentVariableIntValue("BEAM_KWIN_CADENCE_LOG") != 0;
    if (!enabled) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_cadenceSince.time_since_epoch().count() == 0) {
        m_cadenceSince = now;
        m_cadenceBase = m_endFrameCount;
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_cadenceSince);
    if (elapsed < std::chrono::seconds(1)) return;

    const uint64_t composites = m_endFrameCount - m_cadenceBase;
    RenderLoop *loop = m_output->renderLoop();
    WEBRTC_INFO("[cadence] output=%s composites=%llu in %lldms (%.1f/s) renderLoopHz=%d",
                qPrintable(m_output->name()),
                static_cast<unsigned long long>(composites),
                static_cast<long long>(elapsed.count()),
                composites * 1000.0 / static_cast<double>(elapsed.count()),
                loop ? loop->refreshRate() / 1000 : -1);

    m_cadenceSince = now;
    m_cadenceBase = m_endFrameCount;
}

bool WebRTCEglLayer::endFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    if (frame && m_query) {
        m_query->end();
        frame->addRenderTimeQuery(std::move(m_query));
    }
    glFlush();

    // ── WebRTC frame capture: extract DMABUF and feed to per-output pipeline ──
    bool loaned = false;
    bool streaming = m_backend->isStreaming();
    bool hasCurrent = (m_current != nullptr);
    m_endFrameCount++;
    if ((m_endFrameCount % 60) == 1) {
        WEBRTC_TRACE("doEndFrame #%lu: streaming=%d hasCurrent=%d", (unsigned long)m_endFrameCount, streaming ? 1 : 0, hasCurrent ? 1 : 0);
    }
    reportCadence();
    if (streaming && hasCurrent) {
        // Damage-gated capture: skip the loan and the peer's import entirely
        // when the scene reports no real damage. damagedDeviceRegion is KWin's
        // own actual-content-changed signal (compositor.cpp computes it as
        // `surfaceDamage`). Repaints requested with scheduleRepaint(nullptr)
        // add none, so a static desktop reports empty damage here even when
        // the layer is re-rendered.
        // m_backend->consumeForcedCapture() covers the one real gap that
        // leaves: the very first frame after StreamStart/resize, when
        // the peer has nothing imported yet to refresh from.
        const bool forced = m_backend->consumeForcedCapture(m_output);
        // Damage the submit throttle refused on an earlier tick: the scene may
        // report nothing new now (the change already happened), but it has not
        // reached the peer yet, so the gate has to let this tick through.
        const bool deferred = m_backend->consumeDeferredCapture(m_output);
        if (forced || deferred || !damagedDeviceRegion.isEmpty()) {
            GraphicsBuffer *buffer = m_current->buffer();
            if (buffer) {
                const DmaBufAttributes *dma = buffer->dmabufAttributes();
                if (dma && dma->planeCount > 0) {
                    loaned = m_backend->captureFrame(m_output, dma, forced, this,
                                                     damagedDeviceRegion);
                }
            }
        }
    }

    // A loaned slot stays busy until the peer returns it (releaseLoan
    // attaches the peer's fence). Releasing it outright here would hand the
    // compositor a buffer another process is still reading.
    //
    // The AGES still have to advance now, though. They are buffer-age
    // bookkeeping, not a lifetime signal: beginFrame() reads m_current->age()
    // and feeds it to DamageJournal::accumulate(), so an age that has not
    // counted the frame just rendered yields too small a repaint region and
    // leaves stale pixels in the buffer. Deferring both halves to releaseLoan
    // would leave every age one composite behind, and on a loan returned after
    // the next beginFrame() would also age the slot being rendered into.
    if (loaned) {
        m_swapchain->advanceAges(m_current);
    } else {
        m_swapchain->release(m_current, FileDescriptor{});
    }

    // The owed first frame is a DEBT, not one attempt.
    //
    // StreamStart wakes the loop for a single composite, and on a still desktop
    // that is the only composite there will be: the loop is damage-driven and
    // goes back to sleep. If this tick could not deliver -- the stream flag not
    // yet visible here, no current buffer, a capture that returned no loan --
    // the frame would wait for the next unrelated damage, which on a static
    // desktop can be arbitrarily late.
    //
    // So: while the debt stands and its deadline has not passed, wake the loop
    // again. Re-arming the flag matters as much as the repaint -- consuming it
    // above cleared it, and a wake with nothing to consume would compose once
    // and skip the capture on the damage gate.
    if (streaming && !loaned && m_backend->owesForcedCapture(m_output)) {
        m_backend->reArmForcedCapture(m_output);
        scheduleRepaint(nullptr);
        if (RenderLoop *loop = m_output->renderLoop()) loop->scheduleRepaint();
    }
    // m_current is deliberately NOT cleared -- beginFrame() reassigns it and
    // texture() reads it, exactly as VirtualEglLayer does. A loan holds its
    // own reference to the same slot, so shared ownership is the point.
    m_damageJournal.add(damagedDeviceRegion);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// DMABUF loans
// ═══════════════════════════════════════════════════════════════════════════

GraphicsBuffer *WebRTCEglLayer::currentBuffer() const
{
    return m_current ? m_current->buffer() : nullptr;
}

uint32_t WebRTCEglLayer::bufferIdFor(GraphicsBuffer *buffer, bool *isNew)
{
    auto it = m_bufferIds.find(buffer);
    if (it != m_bufferIds.end()) {
        if (isNew) *isNew = false;
        return it->second;
    }
    const uint32_t id = m_nextBufferId++;
    m_bufferIds[buffer] = id;
    if (isNew) *isNew = true;
    return id;
}

bool WebRTCEglLayer::loanCurrentSlot(uint32_t frameId, uint32_t bufferId)
{
    if (!m_current) return false;
    GraphicsBuffer *buffer = m_current->buffer();
    if (!buffer) return false;

    // The reference is what keeps EglSwapchain::acquire() from picking this
    // slot again -- see isBusy(). Without it the compositor would happily
    // composite into a buffer the peer is mid-import on.
    buffer->ref();
    m_loans.push_back(SLoan{
        frameId,
        bufferId,
        m_current,
        buffer,
        std::chrono::steady_clock::now(),
    });
    return true;
}

void WebRTCEglLayer::releaseLoan(uint32_t frameId, int fenceFd)
{
    for (auto it = m_loans.begin(); it != m_loans.end(); ++it) {
        if (it->frameId != frameId) continue;

        // Fence first, THEN unref: afterwards isBusy() is governed purely by
        // the fence, which is the state we want, and there is no window in
        // which the slot looks free because neither condition holds yet.
        // Ages are not touched here -- endFrame() already advanced them for
        // this frame (see the comment there).
        if (m_swapchain) {
            m_swapchain->setReleaseFence(it->slot, FileDescriptor{fenceFd});
        } else if (fenceFd >= 0) {
            ::close(fenceFd);
        }
        it->buffer->unref();
        m_loans.erase(it);
        return;
    }
    // No matching loan: a release that arrived after we force-reclaimed it
    // (watchdog, StreamStop, resize). Normal, but the fd is still ours.
    if (fenceFd >= 0) ::close(fenceFd);
}

std::vector<uint32_t> WebRTCEglLayer::expireStaleLoans(std::chrono::milliseconds timeout)
{
    std::vector<uint32_t> expired;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_loans.begin(); it != m_loans.end();) {
        if (now - it->startedAt < timeout) {
            ++it;
            continue;
        }
        // The peer may still be reading this buffer. Taking it back anyway
        // is a deliberate trade: the alternative is the user's desktop frozen
        // behind a peer that is never going to answer.
        //
        // The ids the log line names are the FIRST of the reporting window,
        // not of this call: a window spans several sweeps, and naming the last
        // one would point at whichever loan happened to expire nearest the
        // line rather than at where the trouble started.
        if (m_expiredSinceLog == 0) {
            m_expiredFirstFrameId = it->frameId;
            m_expiredFirstBufferId = it->bufferId;
        }
        ++m_expiredSinceLog;
        ++m_expiredTotal;
        if (m_swapchain) m_swapchain->setReleaseFence(it->slot, FileDescriptor{});
        it->buffer->unref();
        expired.push_back(it->bufferId);
        it = m_loans.erase(it);
    }

    // ERROR, because an expiry in steady state is a bug, and one every few
    // minutes has to keep producing a line. But a peer that stops reading
    // (survivable thanks to SendPolicy::DropOnFull) can expire several loans
    // a second, which would make a real incident unreadable, so the line is
    // throttled.
    //
    // Throttled by TIME rather than by count, deliberately. A count throttle
    // ("every 240th") under-reports exactly the case the ERROR exists for: a
    // slow steady-state leak would go quiet after its first burst. A time
    // throttle keeps one line per window whatever the rate, and the window's
    // own count says which regime it is in -- 1 since last, or hundreds.
    //
    // What this does cost: a burst that ENDS inside a window has its tail
    // reported at the next line rather than when it happened. The running
    // total is cumulative and never resets, so no expiry is lost from the
    // record -- only its timing is coarse, which is the point of a throttle.
    if (m_expiredSinceLog > 0
        && (m_expiredLastLog.time_since_epoch().count() == 0
            || now - m_expiredLastLog >= std::chrono::seconds(5))) {
        WEBRTC_ERR("WebRTCEglLayer: loan frameId=%u buffer=%u timed out, force-reclaiming "
                   "(%llu in this window, %llu total)",
                   m_expiredFirstFrameId, m_expiredFirstBufferId,
                   static_cast<unsigned long long>(m_expiredSinceLog),
                   static_cast<unsigned long long>(m_expiredTotal));
        m_expiredSinceLog = 0;
        m_expiredLastLog = now;
    }
    return expired;
}

void WebRTCEglLayer::reclaimAllLoans()
{
    for (auto &loan : m_loans) {
        if (m_swapchain) m_swapchain->setReleaseFence(loan.slot, FileDescriptor{});
        loan.buffer->unref();
    }
    m_loans.clear();
}

void WebRTCEglLayer::forgetBufferId(uint32_t bufferId)
{
    for (auto it = m_bufferIds.begin(); it != m_bufferIds.end(); ++it) {
        if (it->second == bufferId) {
            m_bufferIds.erase(it);
            return;
        }
    }
}

void WebRTCEglLayer::forgetAllBufferIds()
{
    if (m_bufferIds.empty()) return;
    for (const auto &[buffer, id] : m_bufferIds) {
        m_backend->sendBufferDrop(m_backend->outputIndexFor(m_output), id);
    }
    m_bufferIds.clear();
}

FormatModifierMap WebRTCEglLayer::supportedDrmFormats() const
{
    return m_backend->supportedFormats();
}

void WebRTCEglLayer::releaseBuffers()
{
    m_current.reset();
    m_swapchain.reset();
}

GLTexture *WebRTCEglLayer::texture() const
{
    return m_current ? m_current->texture().get() : nullptr;
}

} // namespace KWin
