/*
    KWin WebRTC Backend - EGL Backend Implementation

    The layer that loans each composed frame to the peer is WebRTCEglLayer,
    in webrtc_egl_layer.cpp. This file is the backend around
    it: EGL/GBM setup, the per-output pipelines, output topology, and the frame
    submission path that reads a layer's current buffer. The bridge's callback
    surface is webrtc_bridge_handlers.cpp.

    Nothing is encoded in this process: the peer imports the loaned buffer
    and hands it back through BufferRelease. See
    webrtc_egl_layer.h for the two mechanisms that keep a loaned swapchain slot
    out of the compositor's hands until then.

    Multi-monitor: each output has its own SOutputPipeline (stream lifecycle,
    throttle state, damage accumulation and loan table), so outputs can run at
    different resolutions and rates. Transport is one shared CBeamBridge
    socket; a frame names its output in the bridge protocol's own header
    (kwin_bridge_protocol.h), not in-band, so outputs share no wire format.

    Based on KWin's VirtualEglBackend:
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_egl_backend.h"
#include "core/drmdevice.h"
#include "core/gbmgraphicsbufferallocator.h"
#include "core/graphicsbuffer.h"
#include "core/outputconfiguration.h"
#include "opengl/eglnativefence.h"
#include "opengl/eglswapchain.h"
#include "opengl/glrendertimequery.h"
#include "opengl/glutils.h"
#include "utils/softwarevsyncmonitor.h"
#include "webrtc_backend.h"
#include "webrtc_seats.h"
#include "webrtc_cursor_layer.h"
#include "webrtc_input_backend.h"
#include "webrtc_logging.h"
#include "webrtc_output.h"
#include "workspace.h"

#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QMetaObject>

#include "pipeline/WebRTCEnv.hpp"

// Pipeline modules
#include "pipeline/CBeamBridge.hpp"
#include "pipeline/CBeamInputTranslator.hpp"
#include "pipeline/WebRTCLog.hpp"
#include "kwin_bridge_protocol.h"

#include <drm_fourcc.h>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace NStreaming;

static void segfaultHandler(int sig) {
    void *array[30];
    int size = backtrace(array, 30);
    fprintf(stderr, "\n[WebRTC:CRASH] Signal %d — backtrace (%d frames):\n", sig, size);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

namespace KWin
{

static const bool s_seghandlerInstalled = [] {
    signal(SIGSEGV, segfaultHandler);
    signal(SIGABRT, segfaultHandler);
    return true;
}();

// ═══════════════════════════════════════════════════════════════════════════════
// WebRTCEglBackend — OpenGL backend + WebRTC pipeline (multi-monitor)
// ═══════════════════════════════════════════════════════════════════════════════

WebRTCEglBackend::WebRTCEglBackend(WebRTCBackend *b, RenderDevice *renderDevice)
    : EglBackend(renderDevice)
    , m_backend(b)
{
    // The seats need somewhere to send their own cursors: the layer that draws
    // seat 0's is fed by KWin's pointer, which they never touch.
    WebRTCSeats::instance().setEglBackend(this);
}

WebRTCEglBackend::~WebRTCEglBackend()
{
    shutdownPipeline();

    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
        webrtcOutput->setOutputLayer(nullptr);
        webrtcOutput->setCursorLayer(nullptr);
    }
    cleanup();
}

WebRTCBackend *WebRTCEglBackend::backend() const
{
    return m_backend;
}

bool WebRTCEglBackend::initializeEgl()
{
    if (!initClientExtensions()) {
        WEBRTC_ERR("initializeEgl: EGL_EXT_platform_base / EGL_KHR_platform_gbm not supported");
        return false;
    }
    if (!renderDevice()) {
        // Almost always the GBM device: EGL_PLATFORM_GBM_KHR on a node whose
        // vendor GBM backend is missing gives a null display with no other
        // trace, and RenderDevice::open then returns nothing at all.
        WEBRTC_ERR("initializeEgl: no render device (no EGL display for any gbm device)");
        return false;
    }
    return true;
}

bool WebRTCEglBackend::init()
{
    // Log these explicitly, as initPipeline does: otherwise a compositor
    // that comes up without a streaming pipeline leaves no trace of it beyond
    // KWin's own failure handling.
    if (!initializeEgl()) {
        WEBRTC_ERR("init: initializeEgl failed -- no streaming pipeline will start");
        return false;
    }
    if (!createContext()) {
        WEBRTC_ERR("init: createContext failed -- no streaming pipeline will start");
        return false;
    }

    initWayland();

    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        addOutput(output);
    }

    connect(m_backend, &WebRTCBackend::outputAdded, this, &WebRTCEglBackend::addOutput);

    // ── Initialize WebRTC pipeline ───────────────────────────────────────
    SWebRTCConfig config;
    
    // Read configuration from environment variables
    if (const char *env = std::getenv("KWIN_WEBRTC_WIDTH"))
        config.width = std::atoi(env);
    if (const char *env = std::getenv("KWIN_WEBRTC_HEIGHT"))
        config.height = std::atoi(env);
    // The frame rate is not configured here: it arrives as StreamStart's fps.
    // The pipeline starts at the output's own 60 Hz and follows from there.
    if (const char *env = std::getenv("KWIN_WEBRTC_BITRATE"))
        config.bitrateBps = std::atoi(env);
    if (const char *env = std::getenv("KWIN_WEBRTC_CODEC"))
        config.codec = env;

    if (!initPipeline(config)) {
        WEBRTC_ERR("init: initPipeline failed -- streaming disabled");
    }
    return true;
}

// BEAM_KWIN_DMABUF_INFLIGHT: how many buffers may be on loan to the peer at
// once. ONE reader, because this number is both advertised (Capabilities, at
// init) and enforced (sendDmabufFrame, per frame) -- two separate statics
// parsing the same variable is two chances for the peer to be told a ceiling
// this process does not apply.
static size_t dmabufMaxInflight()
{
    static const size_t value = [] {
        const char *e = NWebRTCPipeline::envValue("BEAM_KWIN_DMABUF_INFLIGHT");
        const int n = e ? std::atoi(e) : 2;
        return static_cast<size_t>(n > 0 ? n : 2);
    }();
    return value;
}

// KWIN_WEBRTC_CAPTURE_HZ: how fast the render loop composes, independent of
// the fps advertised to clients and of the stream fps. See
// WebRTCOutput::setCaptureRefreshRate() for why the two are different things.
// 0/unset composes at the mode's rate.
static uint32_t captureRefreshHzEnv()
{
    static const uint32_t hz = [] {
        const char *env = std::getenv("KWIN_WEBRTC_CAPTURE_HZ");
        return env ? static_cast<uint32_t>(std::max(0, std::atoi(env))) : 0u;
    }();
    return hz;
}

void WebRTCEglBackend::addOutput(BackendOutput *output)
{
    (void)openglContext()->makeCurrent();
    auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
    webrtcOutput->setOutputLayer(std::make_unique<WebRTCEglLayer>(output, this));
    webrtcOutput->setCursorLayer(std::make_unique<WebRTCCursorLayer>(output, this));
    // Set here rather than at init(): outputs created later (addVirtualOutput,
    // the outputAdded signal) must get the same treatment, and the override
    // survives every later changeMode/changeRefreshRate on its own.
    if (const uint32_t hz = captureRefreshHzEnv())
        webrtcOutput->setCaptureRefreshRate(hz);
}

QList<OutputLayer *> WebRTCEglBackend::compatibleOutputLayers(BackendOutput *output)
{
    auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
    return {webrtcOutput->outputLayer(), webrtcOutput->cursorLayer()};
}

// ═══════════════════════════════════════════════════════════════════════════════
// Per-Output Pipeline Management
// ═══════════════════════════════════════════════════════════════════════════════

// Bookkeeping only -- an output index, its config, and an empty loan table;
// nothing can fail.
void WebRTCEglBackend::initOutputPipeline(BackendOutput *output, uint32_t outputIdx, const SWebRTCConfig &config)
{
    SOutputPipeline pipeline;
    pipeline.outputIdx = outputIdx;
    pipeline.config = config;

    WEBRTC_LOG("initOutputPipeline: output %u %ux%u@%uHz codec=%s (encoded by beam-host)",
               outputIdx, config.width, config.height, config.fps, config.codec.c_str());

    m_pipelines[output] = std::move(pipeline);
}

void WebRTCEglBackend::shutdownOutputPipeline(BackendOutput *output)
{
    auto it = m_pipelines.find(output);
    if (it != m_pipelines.end()) {
        WEBRTC_LOG("shutdown: destroying pipeline for output %u", it->second.outputIdx);
        m_pipelines.erase(it);
    }
}

WebRTCEglBackend::SOutputPipeline *WebRTCEglBackend::findPipeline(BackendOutput *output)
{
    auto it = m_pipelines.find(output);
    return it != m_pipelines.end() ? &it->second : nullptr;
}

WebRTCEglBackend::SOutputPipeline *WebRTCEglBackend::findPipelineByIdx(uint32_t outputIdx)
{
    for (auto &[output, pipeline] : m_pipelines) {
        if (pipeline.outputIdx == outputIdx)
            return &pipeline;
    }
    return nullptr;
}

BackendOutput *WebRTCEglBackend::findOutputByIdx(uint32_t outputIdx)
{
    for (auto &[output, pipeline] : m_pipelines) {
        if (pipeline.outputIdx == outputIdx)
            return output;
    }
    return nullptr;
}

uint32_t WebRTCEglBackend::getOutputIndex(BackendOutput *output) const
{
    auto it = m_pipelines.find(output);
    return it != m_pipelines.end() ? it->second.outputIdx : 0;
}

WebRTCEglLayer *WebRTCEglBackend::layerForIdx(uint32_t outputIdx)
{
    BackendOutput *output = findOutputByIdx(outputIdx);
    if (!output) return nullptr;
    return static_cast<WebRTCEglLayer *>(static_cast<WebRTCOutput *>(output)->outputLayer());
}

// ═══════════════════════════════════════════════════════════════════════════════
// WebRTC Pipeline Integration (Multi-Monitor)
// ═══════════════════════════════════════════════════════════════════════════════

bool WebRTCEglBackend::initPipeline(const SWebRTCConfig &config)
{
    m_pipelineConfig = config;
    // Everything on this path logs through WEBRTC_ERR/WEBRTC_LOG rather than
    // the kwin_platform_webrtc Qt category. The category may be filtered out,
    // and then a pipeline that never started is
    // indistinguishable from one that was never asked to start. WEBRTC_ERR is
    // always-on and lands in the compositor's log.
    WEBRTC_LOG("initPipeline: entry %ux%u@%u codec=%s",
               config.width, config.height, config.fps, config.codec.c_str());
    // No encoder to select: this process does not encode.
    WEBRTC_LOG("initPipeline: %ux%u@%uHz codec=%s (encode is beam-host's)",
               config.width, config.height, config.fps, config.codec.c_str());

    // ── Initialize per-output pipelines for all existing outputs
    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        uint32_t idx = m_nextOutputIdx++;
        SWebRTCConfig outputConfig = config;
        QSize modeSize = output->modeSize();
        if (modeSize.width() > 0 && modeSize.height() > 0) {
            outputConfig.width = modeSize.width();
            outputConfig.height = modeSize.height();
        }
        initOutputPipeline(output, idx, outputConfig);
    }

    // ── Bridge to the peer ──────────────────────────────────────────────

    std::string socketPath;
    if (const char *env = std::getenv(kwin_bridge::kDefaultSocketEnv)) {
        socketPath = env;
    } else {
        const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
        socketPath = std::string(runtimeDir ? runtimeDir : "/tmp") + "/" +
                     kwin_bridge::kDefaultSocketRelPath;
    }
    // Ensure the socket's parent directory exists.
    if (auto slash = socketPath.rfind('/'); slash != std::string::npos) {
        std::string dir = socketPath.substr(0, slash);
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            WEBRTC_ERR("could not create bridge socket directory %s: %s", dir.c_str(), strerror(errno));
        }
        chmod(dir.c_str(), 0700);
    }

    m_bridge = std::make_unique<CBeamBridge>();
    // Advertised to every client in Capabilities. Whether this EGL stack can
    // export a sync_file is knowable here and only here -- the display exists
    // long before any client connects -- so the peer is told rather than
    // left to guess.
    {
        const size_t maxInflight = dmabufMaxInflight();
        const bool nativeFence = eglDisplayObject() && eglDisplayObject()->supportsNativeFence();
        m_bridge->setDmabufCapabilities(nativeFence, static_cast<uint32_t>(maxInflight));
        WEBRTC_LOG("initPipeline: dmabuf transport advertised (nativeFence=%d maxInflight=%zu)",
                   nativeFence ? 1 : 0, maxInflight);
    }
    if (!m_bridge->init(socketPath)) {
        WEBRTC_ERR("initPipeline: CBeamBridge init failed on %s", socketPath.c_str());
        return false;
    }
    WEBRTC_LOG("initPipeline: CBeamBridge listening on %s", socketPath.c_str());

    // Same descriptor-building + input-geometry sync as every later output
    // change; no client is connected yet, so this only seeds the bridge's
    // list for the Hello it will send on connect.
    broadcastOutputList();

    m_bridge->setOnStreamStart([this](uint16_t idx, uint32_t codec, uint32_t fps,
                                       uint32_t startKbps, uint32_t maxKbps, uint32_t mode) {
        onBridgeStreamStart(idx, codec, fps, startKbps, maxKbps, mode);
    });
    // Both arrive on the bridge's recv thread; the loan table and the
    // swapchain are compositor-thread-only, so they have to hop.
    m_bridge->setOnBufferForget([this](uint16_t idx, uint32_t bufferId) {
        QMetaObject::invokeMethod(this, [this, idx, bufferId]() {
            handleBufferForget(idx, bufferId);
        }, Qt::QueuedConnection);
    });
    // The fence fd rides along by value -- the lambda owns it from here.
    m_bridge->setOnBufferRelease([this](uint16_t idx, uint32_t bufferId, uint32_t frameId,
                                         int fenceFd) {
        QMetaObject::invokeMethod(this, [this, idx, bufferId, frameId, fenceFd]() {
            handleBufferRelease(idx, bufferId, frameId, fenceFd);
        }, Qt::QueuedConnection);
    });
    m_bridge->setOnStreamStop([this](uint16_t idx) { onBridgeStreamStop(idx); });
    m_bridge->setOnSetFps([this](uint16_t idx, uint32_t fps) { onBridgeSetFps(idx, fps); });
    // No handler for ForceIdr, SetBitrate or ResubmitLast: this compositor
    // does not act on them. The bridge still parses the message types (their
    // values stay reserved in the protocol enum) and drops them.
    m_bridge->setOnInput([this](uint16_t outputIdx, const uint8_t *data, size_t len) {
        if (auto *inputBackend = m_backend->inputBackend())
            CBeamInputTranslator::translate(outputIdx, data, len, inputBackend);
    });
    m_bridge->setOnClientConnected([this] { onBridgeClientConnected(); });
    m_bridge->setOnClientDisconnected([this] { onBridgeClientDisconnected(); });
    m_bridge->setOnOutputAdd([this](uint32_t width, uint32_t height, int32_t x, int32_t y, uint32_t fps) {
        onBridgeOutputAdd(width, height, x, y, fps);
    });
    m_bridge->setOnOutputRemove([this](uint16_t idx) { onBridgeOutputRemove(idx); });
    m_bridge->setOnOutputConfigure([this](uint16_t idx, uint32_t width, uint32_t height, bool hasPos,
                                          int32_t x, int32_t y, uint32_t fps) {
        onBridgeOutputConfigure(idx, width, height, hasPos, x, y, fps);
    });

    m_bridge->setOnOutputSetScale([this](uint16_t idx, double scale) {
        onBridgeOutputSetScale(idx, scale);
    });
    m_bridge->setOnOutputSetHdr([this](uint16_t idx, bool on, uint32_t ref, uint32_t max, uint32_t minMilli) {
        onBridgeOutputSetHdr(idx, on, ref, max, minMilli);
    });

    // Clipboard is not carried over the bridge.

    tryWireInput();

    WEBRTC_LOG("Multi-output pipeline initialized: %zu output(s), inputWired=%d",
               m_pipelines.size(), m_inputWired ? 1 : 0);
    return true;
}

void WebRTCEglBackend::shutdownPipeline()
{
    m_inputWired = false;
    if (m_bridge) m_bridge->shutdown();
    m_bridge.reset();
    // Make the context current before tearing down per-output state.
    if (openglContext()) {
        (void)openglContext()->makeCurrent();
    }
    m_pipelines.clear();
}

void WebRTCEglBackend::tryWireInput()
{
    if (m_inputWired)
        return;

    auto *inputBackend = m_backend->inputBackend();
    if (!inputBackend) {
        WEBRTC_LOG("tryWireInput: input backend not yet available (will retry on client connect)");
        return;
    }

    inputBackend->setOutputSize(m_pipelineConfig.width, m_pipelineConfig.height);
    m_inputWired = true;
    WEBRTC_LOG("Remote input wired: beam-host Input messages -> CBeamInputTranslator");
}

// The bridge's callback surface -- onBridgeClientConnected through
// sendCursorPosition -- lives in webrtc_bridge_handlers.cpp.

bool WebRTCEglBackend::isStreaming() const
{
    for (const auto &[output, pipeline] : m_pipelines) {
        if (pipeline.streamActive) return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Multi-Monitor Management
// ═══════════════════════════════════════════════════════════════════════════════

BackendOutput *WebRTCEglBackend::addVirtualOutput(uint32_t width, uint32_t height,
                                                    int32_t x, int32_t y, uint32_t fps,
                                                    const std::string &codec,
                                                    uint32_t bitrateBps)
{
    width  = std::max(320u, std::min(7680u, width));
    height = std::max(240u, std::min(4320u, height));
    width  = (width  + 1) & ~1u;
    height = (height + 1) & ~1u;
    if (fps == 0) fps = m_pipelineConfig.fps;
    if (fps < 15) fps = 15;
    if (fps > 240) fps = 240;

    WebRTCBackend::OutputInfo info;
    info.geometry = Rect(QPoint(x, y), QSize(width, height));
    info.scale = 1.0;
    info.connectorName = QStringLiteral("WEBRTC-%1").arg(m_nextOutputIdx);

    BackendOutput *output = m_backend->addOutput(info);
    if (!output)
        return nullptr;

    SWebRTCConfig outputConfig;
    outputConfig.width      = width;
    outputConfig.height     = height;
    outputConfig.fps        = fps;
    outputConfig.bitrateBps = bitrateBps > 0 ? bitrateBps : m_pipelineConfig.bitrateBps;
    outputConfig.codec      = codec.empty() ? m_pipelineConfig.codec : codec;

    uint32_t idx = m_nextOutputIdx++;
    initOutputPipeline(output, idx, outputConfig);
    // New outputs start inactive; the peer must send StreamStart for this
    // outputIdx (after seeing it in OutputsChanged) before it streams.

    WEBRTC_LOG("added virtual output %u: %ux%u at %d,%d @%uHz codec=%s",
               idx, width, height, x, y, fps, outputConfig.codec.c_str());

    broadcastOutputList();
    return output;
}

bool WebRTCEglBackend::removeVirtualOutput(uint32_t outputIdx)
{
    if (outputIdx == 0) {
        WEBRTC_ERR("cannot remove primary output (index 0)");
        return false;
    }

    BackendOutput *output = findOutputByIdx(outputIdx);
    if (!output) {
        WEBRTC_ERR("output %u not found", outputIdx);
        return false;
    }

    shutdownOutputPipeline(output);
    // The input backend keeps its own geometry table, and broadcastOutputList
    // below only ADDS to it, so remove this output's entry here: otherwise an
    // input event still naming it (sent before the peer saw the new layout)
    // would map absolute coordinates onto an output that no longer exists.
    if (auto *inputBackend = m_backend->inputBackend())
        inputBackend->removeOutputGeometry(outputIdx);
    m_backend->removeVirtualOutput(output);

    WEBRTC_LOG("removed virtual output %u", outputIdx);
    broadcastOutputList();
    return true;
}

bool WebRTCEglBackend::configureOutput(uint32_t outputIdx, uint32_t width, uint32_t height,
                                         bool hasPos, int32_t x, int32_t y, uint32_t fps,
                                         const std::string &codec, uint32_t bitrateBps)
{
    if (width > 0 && height > 0) {
        changeResolution(width, height, outputIdx);
    }
    if (fps > 0) {
        changeFps(fps, outputIdx);
    }
    // Codec is chosen by the peer's StreamStart, not reconfigured here.
    Q_UNUSED(codec);
    if (bitrateBps > 0) {
        changeBitrate(bitrateBps, outputIdx);
    }

    BackendOutput *output = findOutputByIdx(outputIdx);
    if (output && hasPos) {
        auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
        QPoint currentPos = webrtcOutput->position();
        QPoint newPos(x, y);

        if (newPos != currentPos) {
            OutputConfiguration config;
            config.changeSet(output)->pos = newPos;
            auto *ws = Workspace::self();
            if (ws) {
                ws->applyOutputConfiguration(config);
            } else {
                webrtcOutput->setPosition(newPos);
            }
        }
    }

    broadcastOutputList();
    return true;
}

// Per-output DPI scale (MsgType::OutputSetScale). The pixel size is untouched:
// scale changes only the logical coordinate space clients are laid out in, so
// a 3840x2160 output at scale 2 keeps sending 3840x2160 frames and gives its
// clients a 1920x1080 logical desktop.
//
// Routed through OutputConfiguration rather than WebRTCOutput::setScale for
// the same reason the position change at configureOutput() is: Workspace has
// to see the change to relayout clients, move panels and re-run window
// placement. Setting it behind Workspace's back leaves plasmashell drawing at
// the old scale.
bool WebRTCEglBackend::setOutputScale(uint32_t outputIdx, double scale)
{
    BackendOutput *output = findOutputByIdx(outputIdx);
    if (!output) {
        WEBRTC_LOG("setOutputScale: unknown output %u", outputIdx);
        return false;
    }
    auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
    if (qFuzzyCompare(webrtcOutput->scale(), scale))
        return true;

    WEBRTC_LOG("output %u: changing scale %.3f -> %.3f", outputIdx, webrtcOutput->scale(), scale);

    OutputConfiguration config;
    const auto changeSet = config.changeSet(output);
    changeSet->scale = scale;
    // scaleSetting is what output management reads back; leaving it behind
    // would let the next kscreen round trip revert the change.
    changeSet->scaleSetting = scale;
    auto *ws = Workspace::self();
    if (ws) {
        ws->applyOutputConfiguration(config);
    } else {
        // Pre-Workspace (startup): apply it directly so the very first frame
        // is already at the requested scale. Workspace picks the state up when
        // it comes online.
        webrtcOutput->setScale(scale);
    }

    // A scale change relayouts every window on this output, so what the peer
    // holds is stale everywhere -- not in the rectangles the scene happens to
    // report. Force one whole frame, exactly as changeResolution does, and
    // schedule the composite that consumes the flag: the mode is unchanged
    // here, so nothing else guarantees one arrives.
    if (SOutputPipeline *pipeline = findPipelineByIdx(outputIdx)) {
        // Through armForcedCapture so this gets the same debt-with-a-deadline
        // as StreamStart: one scheduled composite is one chance, and a tick
        // that cannot deliver would otherwise leave the frame waiting for real
        // damage.
        if (pipeline->streamActive) armForcedCapture(webrtcOutput);
        else pipeline->pendingForcedCapture = true;
    }

    broadcastOutputList();
    return true;
}

bool WebRTCEglBackend::setOutputHdr(uint32_t outputIdx, bool on, uint32_t referenceNits, uint32_t maxNits,
                                    uint32_t minMilliNits)
{
    BackendOutput *output = findOutputByIdx(outputIdx);
    if (!output) {
        WEBRTC_LOG("setOutputHdr: unknown output %u", outputIdx);
        return false;
    }
    auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
    if (!webrtcOutput->setHdr(on, referenceNits, maxNits, minMilliNits / 1000.0)) {
        return true;  // already so
    }
    WEBRTC_LOG("output %u: HDR %s (SDR white %u nits, peak %u, black %.3f)", outputIdx, on ? "on" : "off",
               referenceNits, maxNits, minMilliNits / 1000.0);
    // Every pixel changes encoding: one whole frame, as a scale change does.
    if (SOutputPipeline *pipeline = findPipelineByIdx(outputIdx)) {
        if (pipeline->streamActive) armForcedCapture(webrtcOutput);
        else pipeline->pendingForcedCapture = true;
    }
    return true;
}

std::vector<WebRTCEglBackend::SOutputInfo> WebRTCEglBackend::getOutputInfos() const
{
    std::vector<SOutputInfo> infos;
    for (const auto &[output, pipeline] : m_pipelines) {
        SOutputInfo info;
        info.idx = pipeline.outputIdx;
        info.width = pipeline.config.width;
        info.height = pipeline.config.height;
        auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
        QPoint pos = webrtcOutput->position();
        info.x = pos.x();
        info.y = pos.y();
        info.fps = pipeline.config.fps;
        info.codec = pipeline.config.codec;
        info.bitrateBps = pipeline.config.bitrateBps;
        infos.push_back(info);
    }
    std::sort(infos.begin(), infos.end(), [](const SOutputInfo &a, const SOutputInfo &b) {
        return a.idx < b.idx;
    });
    return infos;
}

void WebRTCEglBackend::broadcastOutputList()
{
    auto infos = getOutputInfos();

    if (m_bridge) {
        std::vector<CBeamBridge::OutputDescriptor> descs;
        for (const auto &info : infos) {
            CBeamBridge::OutputDescriptor d;
            d.idx = static_cast<uint16_t>(info.idx);
            d.x = info.x;
            d.y = info.y;
            d.nativeW = info.width;
            d.nativeH = info.height;
            d.fps = info.fps;
            d.name = QStringLiteral("WEBRTC-%1").arg(info.idx).toStdString();
            descs.push_back(d);
        }
        m_bridge->setOutputs(descs);
    }

    // Sync input backend geometries for multi-output coordinate mapping
    auto *inputBackend = m_backend->inputBackend();
    if (inputBackend) {
        for (const auto &info : infos) {
            inputBackend->setOutputGeometry(info.idx, info.x, info.y, info.width, info.height);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Per-Output Settings Changes
// ═══════════════════════════════════════════════════════════════════════════════

bool WebRTCEglBackend::changeBitrate(uint32_t newBitrateBps, uint32_t outputIdx)
{
    SOutputPipeline *pipeline = findPipelineByIdx(outputIdx);
    if (!pipeline) return false;

    if (newBitrateBps == 0 || newBitrateBps == pipeline->config.bitrateBps)
        return true;

    WEBRTC_LOG("output %u: changing bitrate %u kbps -> %u kbps",
               outputIdx, pipeline->config.bitrateBps / 1000, newBitrateBps / 1000);

    // Recorded only; nothing in this process uses it.
    pipeline->config.bitrateBps = newBitrateBps;
    return true;
}

bool WebRTCEglBackend::changeResolution(uint32_t width, uint32_t height, uint32_t outputIdx)
{
    SOutputPipeline *pipeline = findPipelineByIdx(outputIdx);
    if (!pipeline) return false;

    if (width == 0 || height == 0 || width > 7680 || height > 4320)
        return false;

    width  = (width  + 1) & ~1u;
    height = (height + 1) & ~1u;

    if (width == pipeline->config.width && height == pipeline->config.height)
        return true;

    WEBRTC_LOG("output %u: changing resolution %ux%u -> %ux%u",
               outputIdx, pipeline->config.width, pipeline->config.height, width, height);

    const bool wasActive = pipeline->streamActive;
    pipeline->streamActive = false;

    BackendOutput *output = findOutputByIdx(outputIdx);
    if (output) {
        auto *webrtcOutput = static_cast<WebRTCOutput *>(output);
        webrtcOutput->changeMode(QSize(width, height));
    }
    Q_EMIT m_backend->outputsQueried();

    // Each DmabufFramePayload carries width/height, so a resize needs no
    // message of its own.
    pipeline->config.width  = width;
    pipeline->config.height = height;

    if (outputIdx == 0) {
        if (auto *inputBackend = m_backend->inputBackend())
            inputBackend->setOutputSize(width, height);
    }

    pipeline->streamActive = wasActive;

    // Damage-driven render loop: the mode change alone does not guarantee a
    // composite reaches WebRTCEglLayer::endFrame (the layer may have nothing
    // to repaint), and the forced capture is only consumed there.
    // armForcedCapture also keeps waking the loop until the frame is actually
    // out, because one scheduled composite is one chance.
    if (output && pipeline->streamActive) armForcedCapture(output);
    else pipeline->pendingForcedCapture = true;

    broadcastOutputList();
    return true;
}

bool WebRTCEglBackend::changeFps(uint32_t fps, uint32_t outputIdx)
{
    if (fps < 15 || fps > 240)
        return false;

    SOutputPipeline *pipeline = findPipelineByIdx(outputIdx);
    if (!pipeline) return false;

    // Compared against the output's own rate too, as StreamStart does
    // (webrtc_bridge_handlers.cpp): config.fps can match while the output's
    // refresh rate does not.
    auto *webrtcOutput = static_cast<WebRTCOutput *>(findOutputByIdx(outputIdx));
    const bool outputRateDiffers = webrtcOutput && webrtcOutput->refreshRate() != fps * 1000u;
    if (fps == pipeline->config.fps && !outputRateDiffers)
        return true;

    WEBRTC_LOG("output %u: changing fps %u -> %u", outputIdx, pipeline->config.fps, fps);

    pipeline->config.fps = fps;

    if (webrtcOutput) {
        webrtcOutput->changeRefreshRate(fps);
    }

    broadcastOutputList();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Frame Capture (Per-Output)
// ═══════════════════════════════════════════════════════════════════════════════

bool WebRTCEglBackend::consumeForcedCapture(BackendOutput *output)
{
    SOutputPipeline *pipeline = findPipeline(output);
    if (!pipeline) return false;
    const bool forced = pipeline->pendingForcedCapture;
    pipeline->pendingForcedCapture = false;
    return forced;
}

// How long a stream keeps waking an idle render loop to deliver its first
// frame. Generous against a slow first composite (EGL, swapchain, the first
// import), short enough that an output which genuinely cannot compose stops
// spinning.
static constexpr auto kForcedCaptureGrace = std::chrono::seconds(3);

void WebRTCEglBackend::armForcedCapture(BackendOutput *output)
{
    SOutputPipeline *pipeline = findPipeline(output);
    if (!pipeline) return;
    pipeline->pendingForcedCapture = true;
    pipeline->forcedCaptureDeadline = std::chrono::steady_clock::now() + kForcedCaptureGrace;
    if (auto *webrtcOutput = static_cast<WebRTCOutput *>(output)) webrtcOutput->kickRepaint();
}

void WebRTCEglBackend::reArmForcedCapture(BackendOutput *output)
{
    if (SOutputPipeline *pipeline = findPipeline(output)) pipeline->pendingForcedCapture = true;
}

bool WebRTCEglBackend::owesForcedCapture(BackendOutput *output) const
{
    auto it = m_pipelines.find(output);
    if (it == m_pipelines.end()) return false;
    const SOutputPipeline &pipeline = it->second;
    if (!pipeline.streamActive) return false;
    if (pipeline.forcedCaptureDeadline.time_since_epoch().count() == 0) return false;
    return std::chrono::steady_clock::now() < pipeline.forcedCaptureDeadline;
}

bool WebRTCEglBackend::consumeDeferredCapture(BackendOutput *output)
{
    SOutputPipeline *pipeline = findPipeline(output);
    if (!pipeline) return false;
    const bool deferred = pipeline->pendingDeferredCapture;
    pipeline->pendingDeferredCapture = false;
    return deferred;
}

uint32_t WebRTCEglBackend::outputIndexFor(BackendOutput *output)
{
    SOutputPipeline *pipeline = findPipeline(output);
    return pipeline ? pipeline->outputIdx : 0;
}

void WebRTCEglBackend::sendBufferDrop(uint32_t outputIdx, uint32_t bufferId)
{
    if (m_bridge) m_bridge->sendBufferDrop(static_cast<uint16_t>(outputIdx), bufferId);
}

void WebRTCEglBackend::handleBufferRelease(uint16_t outputIdx, uint32_t bufferId, uint32_t frameId,
                                            int fenceFd)
{
    auto *layer = layerForIdx(outputIdx);
    if (!layer) {
        if (fenceFd >= 0) ::close(fenceFd);
        return;
    }
    (void)bufferId;  // frameId identifies the loan; bufferId is for diagnostics
    layer->releaseLoan(frameId, fenceFd);
}


void WebRTCEglBackend::handleBufferForget(uint16_t outputIdx, uint32_t bufferId)
{
    auto *layer = layerForIdx(outputIdx);
    if (!layer) return;
    WEBRTC_LOG("peer could not resolve buffer %u -- resending its descriptors now", bufferId);
    layer->forgetBufferId(bufferId);
    // Now, not with the next frame that happens to be captured. The peer is
    // waiting on descriptors it cannot resolve, so whatever it was shown last
    // is all it has; on a still desktop the next capture is whatever damages
    // the screen next, and nothing new reaches the peer until then.
    if (auto *output = findOutputByIdx(outputIdx)) armForcedCapture(output);
}

bool WebRTCEglBackend::captureFrame(BackendOutput *output, const DmaBufAttributes *dma,
                                    bool bypassThrottle, WebRTCEglLayer *layer,
                                    const Region &damage)
{
    SOutputPipeline *pipeline = findPipeline(output);
    if (!pipeline || !dma || !pipeline->streamActive) {
        return false;
    }
    // Accumulate FIRST, send later: this function has three ways to return
    // without sending (throttle, loan cap, send failure) and the damage of
    // those ticks belongs to the frame that eventually goes out. A forced
    // capture is the compositor saying it bypassed its own damage gate, so
    // what it carries describes nothing -- mark the accumulation unusable
    // instead of pretending it is empty.
    if (bypassThrottle) {
        pipeline->damageUnknown = true;
    } else {
        pipeline->pendingDamage |= damage;
    }
    pipeline->frameCounter++;

    if ((pipeline->frameCounter % 60) == 1) {
        WEBRTC_TRACE("captureFrame output[%u]: frame #%lu (sent=%lu dropped=%lu throttled=%lu)",
                   pipeline->outputIdx, (unsigned long)pipeline->frameCounter,
                   (unsigned long)pipeline->stats.framesEncoded,
                   (unsigned long)pipeline->stats.framesDropped,
                   (unsigned long)pipeline->stats.framesThrottled);
    }

    // Submit throttle: decouples how often the scene composes from how many
    // frames reach the peer. Composing faster shrinks the wait between a
    // scene change and the frame that contains it, while this gate keeps the
    // number of frames handed to the peer at the negotiated fps.
    //
    // A SCHEDULE, not a minimum spacing from the last frame sent: the due
    // time advances by exactly one interval per frame that goes out, and a
    // frame is refused only when it arrives more than a tenth of an interval
    // before it is due. A minimum spacing leaves almost no slack at high
    // frame rates, so GPU contention pushes composites under it and they are
    // thrown away. A throttled
    // tick is not a lost change -- it re-arms the damage gate via
    // pendingDeferredCapture.
    const auto now = std::chrono::steady_clock::now();
    const auto interval = pipeline->config.fps > 0
        ? std::chrono::nanoseconds(1'000'000'000ull / pipeline->config.fps)
        : std::chrono::nanoseconds(0);
    if (!bypassThrottle && interval.count() > 0
        && pipeline->nextSendDue.time_since_epoch().count() != 0) {
        if (now + interval / 10 < pipeline->nextSendDue) {
            pipeline->pendingDeferredCapture = true;
            pipeline->stats.framesThrottled++;
            // With the damage-driven render loop a throttled change must
            // arrange its own next composite, or it is not sent until the
            // next scene damage. scheduleRepaint(nullptr) wakes the render
            // loop AND keeps needsRepaint() true without injecting fake
            // damage, so the next tick reaches WebRTCEglLayer::endFrame,
            // consumeDeferredCapture() fires and the throttle is tried again.
            if (auto *webrtcOutput = static_cast<WebRTCOutput *>(output)) {
                if (OutputLayer *l = webrtcOutput->outputLayer()) {
                    l->scheduleRepaint(nullptr);
                }
            }
            return false;
        }
    }

    // The buffer itself goes to the peer and comes back through
    // BufferRelease.
    //
    // Stamp the throttle only for a frame actually sent, so a frame refused
    // by the loan cap does not use up the window and the retry tick is not
    // refused by the throttle for a frame that was never sent.
    const bool sent = sendDmabufFrame(pipeline, dma, layer, bypassThrottle);
    if (sent && interval.count() > 0) {
        // One interval on from the previous due time, so the average rate is
        // the negotiated one; restarted from now when this frame is a whole
        // interval late (an idle desktop, a stall); and held within
        // [now + T/4, now + 1.1 T] so a late frame cannot be followed by a
        // burst, nor a schedule that ran ahead starve the stream.
        auto &due = pipeline->nextSendDue;
        if (due.time_since_epoch().count() == 0 || now >= due + interval) {
            due = now + interval;
        } else {
            due += interval;
            due = std::clamp(due, now + interval / 4, now + interval + interval / 10);
        }
    }
    return sent;
}

// Fills the frame's damage rectangles from what this output has accumulated
// since the last frame it sent, and resets the accumulation.
//
// Three answers, and the difference between them matters to the peer:
//   kDamageFull            we cannot describe it (a forced capture, a resize,
//                          or a region so large that describing it saves
//                          nothing)
//   count > 0              exactly these rectangles changed
//   count == 0, not full   NOTHING changed -- a keep-alive.
//
// More rectangles than the payload carries are coalesced to their bounding
// box rather than dropped: a bounding box always covers what changed, it just
// covers more.
void WebRTCEglBackend::fillDamagePayload(SOutputPipeline *pipeline, bool forced,
                                         kwin_bridge::DmabufFramePayload &payload)
{
    const Region damage = pipeline->pendingDamage;
    const bool unknown = pipeline->damageUnknown || forced;
    pipeline->pendingDamage = Region();
    pipeline->damageUnknown = false;

    payload.damageRectCount = 0;
    payload.damageFlags = 0;
    if (unknown) {
        payload.damageFlags |= kwin_bridge::kDamageFull;
        return;
    }
    if (damage.isEmpty()) {
        return; // count 0, not full: nothing changed
    }

    const auto rects = damage.rects();
    const int64_t surface = static_cast<int64_t>(payload.width) * payload.height;
    int64_t covered = 0;
    for (const auto &r : rects) {
        covered += static_cast<int64_t>(r.width()) * r.height();
    }
    if (surface > 0 && covered * 10 >= surface * 6) {
        payload.damageFlags |= kwin_bridge::kDamageFull;
        return;
    }

    if (rects.size() > kwin_bridge::kMaxDamageRects) {
        const auto bb = damage.boundingRect();
        payload.damageRectCount = 1;
        payload.damageRect[0] = {static_cast<uint16_t>(std::max(0, bb.x())),
                                 static_cast<uint16_t>(std::max(0, bb.y())),
                                 static_cast<uint16_t>(std::max(0, bb.width())),
                                 static_cast<uint16_t>(std::max(0, bb.height()))};
        return;
    }

    uint8_t n = 0;
    for (const auto &r : rects) {
        if (r.width() <= 0 || r.height() <= 0) {
            continue;
        }
        payload.damageRect[n++] = {static_cast<uint16_t>(std::max(0, r.x())),
                                   static_cast<uint16_t>(std::max(0, r.y())),
                                   static_cast<uint16_t>(r.width()),
                                   static_cast<uint16_t>(r.height())};
    }
    payload.damageRectCount = n;
    if (n == 0) {
        payload.damageFlags |= kwin_bridge::kDamageFull;
    }
}

// Hands one composited buffer to the peer by file descriptor.
//
// Ordering matters and is not obvious: the loan has to be recorded BEFORE the
// send, because the reply can arrive on the bridge's recv thread while this
// function is still running. Recording afterwards leaves a window where a
// BufferRelease finds no loan, silently drops it, and the slot is pinned
// forever.
bool WebRTCEglBackend::sendDmabufFrame(SOutputPipeline *pipeline, const DmaBufAttributes *dma,
                                        WebRTCEglLayer *layer, bool forced)
{
    if (!layer || !m_bridge) return false;

    // Retire loans a wedged peer never returned, before testing the cap --
    // otherwise one dead peer permanently pins the output at the ceiling.
    static const auto loanTimeout = [] {
        const char *e = NWebRTCPipeline::envValue("BEAM_KWIN_DMABUF_LOAN_TIMEOUT_MS");
        const int ms = e ? std::atoi(e) : 250;
        return std::chrono::milliseconds(ms > 0 ? ms : 250);
    }();
    for (uint32_t staleId : layer->expireStaleLoans(loanTimeout)) {
        pipeline->loansExpired++;
        m_bridge->sendBufferDrop(static_cast<uint16_t>(pipeline->outputIdx), staleId);
    }

    if (layer->loanCount() >= dmabufMaxInflight()) {
        // Drop the NEW frame, not an outstanding loan: the peer is behind, and
        // sending it more only adds latency to what it eventually shows. Re-arm
        // the damage gate so the change is not lost -- same mechanism a
        // throttled tick uses, deliberately not pendingForcedCapture (which
        // bypasses the fps cap).
        pipeline->framesLoanCapped++;
        pipeline->pendingDeferredCapture = true;
        return false;
    }

    GraphicsBuffer *buffer = nullptr;
    if (auto slotBuffer = layer->currentBuffer()) buffer = slotBuffer;
    if (!buffer) return false;

    bool isNew = false;
    const uint32_t bufferId = layer->bufferIdFor(buffer, &isNew);
    const uint32_t frameId = pipeline->nextDmabufFrameId++;

    kwin_bridge::DmabufFramePayload payload{};
    payload.ptsUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    payload.frameId = frameId;
    payload.bufferId = bufferId;
    payload.width = static_cast<uint32_t>(dma->width);
    payload.height = static_cast<uint32_t>(dma->height);
    payload.drmFormat = dma->format;
    payload.modifier = dma->modifier;
    payload.planeCount = static_cast<uint8_t>(dma->planeCount);
    payload.fdCount = isNew ? static_cast<uint8_t>(dma->planeCount) : 0;
    payload.fenceFdIndex = kwin_bridge::kNoFenceFd;
    fillDamagePayload(pipeline, forced, payload);
    for (int i = 0; i < dma->planeCount && i < 4; ++i) {
        payload.offset[i] = dma->offset[i];
        payload.pitch[i] = dma->pitch[i];
    }

    int fds[kwin_bridge::kMaxFdsPerMessage];
    int fdCount = 0;
    if (isNew) {
        for (int i = 0; i < dma->planeCount && i < 4; ++i) {
            fds[fdCount++] = dma->fd[i].get();
        }
    }

    uint8_t flags = 0;
    if (isNew) flags |= kwin_bridge::kDmabufNewBuffer;
    if (forced) flags |= kwin_bridge::kDmabufForced;
    if (layer->currentBufferHdr()) flags |= kwin_bridge::kDmabufHdr;

    // Explicit cross-process sync. EGLNativeFence does the glFlush and hands
    // back a sync_file the peer can wait on GPU-side; without it (driver
    // without EGL_ANDROID_native_fence_sync, or the peer said it cannot
    // import one) we owe the peer a CPU-side guarantee instead, which is what
    // glFinish is doing below. There is no third option: NVIDIA provides no
    // implicit sync across DMABUF.
    //
    // Created only when the peer can import a fence: EGLNativeFence's
    // constructor (eglCreateSyncKHR, a glFlush, eglDupNativeFenceFDANDROID)
    // would otherwise be wasted on every frame. Without a fence, the glFinish
    // below is required for correctness.
    FileDescriptor fenceFd;
    const bool peerWantsFence = (m_bridge->peerFeatures() & kwin_bridge::kFeatNativeFence) != 0;
    bool sentFence = false;
    if (peerWantsFence) {
        EGLNativeFence fence(eglDisplayObject());
        if (fence.isValid()) {
            fenceFd = fence.takeFileDescriptor();
            payload.fenceFdIndex = static_cast<uint8_t>(fdCount);
            fds[fdCount++] = fenceFd.get();
            flags |= kwin_bridge::kDmabufHasFence;
            sentFence = true;
        }
    }
    if (!sentFence) {
        glFinish();
    }

    if (!layer->loanCurrentSlot(frameId, bufferId)) return false;

    if (!m_bridge->sendDmabufFrame(static_cast<uint16_t>(pipeline->outputIdx), payload, flags, fds,
                                    fdCount)) {
        // Nothing will ever release it, so take it straight back.
        layer->releaseLoan(frameId, -1);
        // And if this send was the one carrying the descriptors, the id must
        // not stay marked as announced: the peer never received them, and
        // every later frame on this buffer would arrive without any way to
        // resolve it. Forgetting the id makes the next one carry them again.
        if (isNew) layer->forgetBufferId(bufferId);
        pipeline->stats.framesDropped++;
        // The damage that went out with this frame died with it. The
        // accumulation was already consumed by fillDamagePayload, and
        // reconstructing it is not worth it for a path that only fires when
        // the socket is broken: say "unknown" and let the next frame that
        // gets through be a whole one.
        pipeline->damageUnknown = true;
        return false;
    }

    pipeline->stats.framesEncoded++;

    // Half-rate diagnosis, dmabuf path. `endFrames` is how often the
    // compositor actually composited; `sent` is how many of those reached the
    // peer. If the two track each other and both sit at half the refresh
    // rate, the render loop is the thing running slow and nothing here can
    // fix it. If endFrames runs at the refresh rate and sent is half of it,
    // one of the gates refused the difference: capped= and throttled= below
    // say which, and send failures are counted in framesDropped. Cheap enough
    // to leave in: one printf per 60 frames.
    if ((pipeline->stats.framesEncoded % 60) == 1) {
        const auto now = std::chrono::steady_clock::now();
        const double sinceMs = pipeline->dmabufTraceAt.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double, std::milli>(now - pipeline->dmabufTraceAt).count();
        WEBRTC_LOG("dmabuf output[%u]: sent=%llu endFrames=%llu (+%llu/+%llu in %.0fms) "
                   "capped=%llu throttled=%llu expired=%llu loansNow=%zu",
                   pipeline->outputIdx,
                   (unsigned long long)pipeline->stats.framesEncoded,
                   (unsigned long long)layer->endFrameCount(),
                   (unsigned long long)(pipeline->stats.framesEncoded - pipeline->dmabufTraceSent),
                   (unsigned long long)(layer->endFrameCount() - pipeline->dmabufTraceEnd),
                   sinceMs,
                   (unsigned long long)pipeline->framesLoanCapped,
                   (unsigned long long)pipeline->stats.framesThrottled,
                   (unsigned long long)pipeline->loansExpired,
                   layer->loanCount());
        pipeline->dmabufTraceAt = now;
        pipeline->dmabufTraceSent = pipeline->stats.framesEncoded;
        pipeline->dmabufTraceEnd = layer->endFrameCount();
    }
    return true;
}

} // namespace KWin

#include "moc_webrtc_egl_backend.cpp"
