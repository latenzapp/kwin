/*
    KWin WebRTC Backend - WebRTCEglLayer

    The per-output compositing layer. It intercepts endFrame() to take the
    composed frame's GraphicsBuffer and LOAN it to the peer by file
    descriptor: the peer imports the loaned buffer and returns it through
    BufferRelease.

    A separate class with state of its own -- the loan table, the buffer-id
    map, the damage journal -- reaching the backend only through m_backend.

    Based on KWin's VirtualEglLayer:
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Aug-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "utils/damagejournal.h"

#include <chrono>
#include <map>
#include <memory>
#include <vector>

namespace KWin
{

class ColorDescription;
class EglSwapchain;
class EglSwapchainSlot;
class GLFramebuffer;
class GLRenderTimeQuery;
class GLTexture;
class GraphicsBuffer;
class WebRTCEglBackend;

class KWIN_EXPORT WebRTCEglLayer : public OutputLayer
{
public:
    WebRTCEglLayer(BackendOutput *output, WebRTCEglBackend *backend);
    ~WebRTCEglLayer() override;

    std::optional<OutputLayerBeginFrameInfo> beginFrame(OutputFrame *frame) override;
    bool endFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;

    FormatModifierMap supportedDrmFormats() const override;
    void releaseBuffers() override;

    GLTexture *texture() const;

    // ── DMABUF loans (kStreamModeDmabuf) ─────────────────────────────────
    //
    // On the dmabuf transport we do not copy the composited frame, we LEND
    // the swapchain slot to the peer and wait to be told it is finished.
    // Until then the slot must not be recomposed into.
    //
    // Two mechanisms are needed and they cover disjoint windows:
    //
    //   GraphicsBuffer::ref()   from the moment we send until BufferRelease
    //                           arrives. Holding the shared_ptr alone is not
    //                           enough -- EglSwapchain::acquire() picks any
    //                           slot whose isBusy() is false, and an
    //                           unreleased slot has neither a referenced
    //                           buffer nor a valid release fence.
    //   release fence           from BufferRelease until the peer's GPU reads
    //                           actually retire. EglSwapchainSlot::isBusy()
    //                           already consults it; we just have to pass it.
    //
    // Called on the compositor thread only. releaseLoan() is marshalled there
    // by the backend, since BufferRelease arrives on the bridge recv thread.
    bool loanCurrentSlot(uint32_t frameId, uint32_t bufferId);
    // The GraphicsBuffer behind the slot currently being ended, or nullptr.
    GraphicsBuffer *currentBuffer() const;
    // That buffer holds HDR (PQ / BT.2020, 10 bits): the frame's kDmabufHdr.
    bool currentBufferHdr() const { return m_currentHdr; }
    void releaseLoan(uint32_t frameId, int fenceFd);
    size_t loanCount() const { return m_loans.size(); }

    // Stable per-GraphicsBuffer id, and whether this is the first time we
    // have seen it (in which case the plane fds must ride along).
    uint32_t bufferIdFor(GraphicsBuffer *buffer, bool *isNew);

    // Drops EVERY id without telling the peer. For a client change: a newly
    // connected peer starts with an empty import cache, so any id we
    // still remember announcing to its predecessor would reach it as a frame
    // with no descriptors and nothing to resolve them against.
    void resetBufferIds() { m_bufferIds.clear(); }

    // Drops one id from the map, so the next frame on that buffer is announced
    // as new and carries its plane descriptors again. Called when the peer
    // says it cannot resolve the id, and when a send failed after the id was
    // already marked as announced.
    void forgetBufferId(uint32_t bufferId);

    // Returns every loan older than `timeout`, having already released
    // it. A wedged or crashed peer must not freeze the user's desktop; the
    // buffer ids come back so the backend can tell it to forget them.
    std::vector<uint32_t> expireStaleLoans(std::chrono::milliseconds timeout);

    // Force-reclaims everything. Disconnect, StreamStop, resize.
    void reclaimAllLoans();

private:
    // Drops every buffer-id mapping and tells the peer, because the buffers
    // they referred to are about to stop existing.
    void forgetAllBufferIds();

    // Composites/second against the rate the render loop was configured at.
    // Diagnostic for whether KWIN_WEBRTC_CAPTURE_HZ actually caps the
    // composite rate; BEAM_KWIN_CADENCE_LOG=1. See the definition.
    void reportCadence();
    std::chrono::steady_clock::time_point m_cadenceSince{};
    uint64_t m_cadenceBase = 0;

    // Loan-expiry reporting, throttled by time. See expireStaleLoans: still
    // ERROR, because an expiry in steady state is a bug, but a peer that
    // stopped reading is a survivable state and must not flood the log.
    uint64_t m_expiredSinceLog = 0;
    uint64_t m_expiredTotal = 0;
    uint32_t m_expiredFirstFrameId = 0;
    uint32_t m_expiredFirstBufferId = 0;
    std::chrono::steady_clock::time_point m_expiredLastLog{};

    WebRTCEglBackend *const m_backend;
    std::shared_ptr<EglSwapchain> m_swapchain;
    std::shared_ptr<EglSwapchainSlot> m_current;
    bool m_currentHdr = false;  // m_current was composed for an HDR output
    bool m_swapchainForHdr = false;  // m_swapchain was made for an HDR output (maybe 8-bit, see beginFrame)
    std::unique_ptr<GLRenderTimeQuery> m_query;
    DamageJournal m_damageJournal;

    // ── HDR blending (see WebRTCOutput::setHdr) ─────────────────────────
    //
    // When the output's blending colour differs from its encoding (HDR:
    // gamma 2.2 vs PQ) the scene composes into this shadow texture instead
    // of the swapchain slot, and endFrame converts the repainted region into
    // the slot in one pass. One texture, not a swapchain: it never leaves
    // this process, so nothing can hold it while the next frame composes,
    // and with buffer age always 1 the scene repaints only this frame's
    // damage. m_damageJournal keeps tracking the SLOTS (their ages decide
    // how much of each slot the conversion pass rewrites);
    // m_shadowDamageJournal tracks the texture.
    bool beginShadow(const QSize &size, OutputTransform transform);
    void convertShadow(const Region &damagedDeviceRegion);
    void releaseShadow();
    std::unique_ptr<GLTexture> m_shadowTexture;
    std::unique_ptr<GLFramebuffer> m_shadowFramebuffer;
    DamageJournal m_shadowDamageJournal;
    // The texture holds the previous frame, complete (an endFrame ran).
    bool m_shadowValid = false;
    // This frame composes into the shadow texture.
    bool m_shadowActive = false;
    // A size no blending buffer could be made at: not retried every frame.
    QSize m_shadowFailedSize;
    // The conversion the shadow was composed for, (blending, encoding): a
    // change repaints everything, in the texture and in every slot.
    std::shared_ptr<ColorDescription> m_shadowBlend;
    std::shared_ptr<ColorDescription> m_shadowEncoding;
    // Per layer, so each output's every-60th trace samples its own
    // composites.
    uint64_t m_endFrameCount = 0;

public:
    // Composite ticks reaching endFrame(). Distinguishes "the compositor is
    // running at half rate" from "the compositor runs at full rate and our
    // own gates refuse half the frames" -- the two have opposite fixes.
    uint64_t endFrameCount() const { return m_endFrameCount; }

private:

    struct SLoan {
        uint32_t frameId = 0;
        uint32_t bufferId = 0;
        std::shared_ptr<EglSwapchainSlot> slot;
        GraphicsBuffer *buffer = nullptr;
        // When the loan was made, NOT when it expires: expireStaleLoans
        // compares `now - startedAt` against its timeout argument, so the
        // watchdog window is the caller's to choose, not the loan's.
        std::chrono::steady_clock::time_point startedAt;
    };
    std::vector<SLoan> m_loans;
    std::map<GraphicsBuffer *, uint32_t> m_bufferIds;
    uint32_t m_nextBufferId = 1;
};

} // namespace KWin
