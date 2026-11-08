/*
    KWin WebRTC Backend - Main Backend Implementation
    Headless backend for WebRTC streaming. Based on VirtualBackend.

    Based on KWin's VirtualBackend:
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_backend.h"

#include "core/drmdevice.h"
#include "core/renderdevice.h"
#include "webrtc_egl_backend.h"
#include "webrtc_input_backend.h"
#include "webrtc_output.h"
#include "webrtc_logging.h"

#include <QFile>

#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <ranges>
#include <xf86drm.h>

namespace KWin
{

static bool canOpen(const QString &path)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

/**
 * The DRM node the compositor renders on. Since KWin 6.8 the compositor opens
 * the device itself (GpuManager) and hands it to createOpenGLBackend(); this
 * only decides WHICH node, and says so through KWIN_RENDER_NODES, which
 * GpuManager reads when it is created -- after this backend.
 */
static QString findRenderNodePath()
{
#if !HAVE_LIBDRM_FAUX
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariableIsSet("CI")) {
        return QStringLiteral("/dev/dri/card1");
    }
#endif
#endif

    // KWIN_DRM_DEVICE pins the compositor to ONE card.
    //
    // Without it this function takes whatever drmGetDevices2 returns first,
    // which on a machine with several GPUs is an arbitrary answer the peer does
    // not share: the two halves could open different cards. The dmabuf import
    // then fails with an opaque EGL_BAD_ALLOC, because the GBM/EGL context has
    // to be built on the same GPU as the buffer whose fd is handed across the
    // bridge.
    //
    // A path that cannot be opened FALLS THROUGH to the scan below rather than
    // failing the compositor: the variable may be stale, and a stale hint must
    // cost a pin, never the desktop.
    const QString pinned = qEnvironmentVariable("KWIN_DRM_DEVICE");
    if (!pinned.isEmpty()) {
        if (canOpen(pinned)) {
            qCInfo(KWIN_WEBRTC) << "render device pinned by KWIN_DRM_DEVICE:" << pinned;
            return pinned;
        }
        qCWarning(KWIN_WEBRTC) << "KWIN_DRM_DEVICE" << pinned
                               << "could not be opened; falling back to the first render node";
    }

    const int deviceCount = drmGetDevices2(0, nullptr, 0);
    if (deviceCount <= 0) {
        return QString();
    }

    QList<drmDevice *> devices(deviceCount);
    if (drmGetDevices2(0, devices.data(), devices.size()) < 0) {
        return QString();
    }
    auto deviceCleanup = qScopeGuard([&devices]() {
        drmFreeDevices(devices.data(), devices.size());
    });

    for (drmDevice *device : std::as_const(devices)) {
        int nodeType = DRM_NODE_RENDER;
        if (device->bustype == DRM_BUS_PLATFORM) {
            if (strcmp(device->businfo.platform->fullname, "vgem") == 0) {
                nodeType = DRM_NODE_PRIMARY;
            }
        }
#if HAVE_LIBDRM_FAUX
        if (device->bustype == DRM_BUS_FAUX) {
            if (strcmp(device->businfo.faux->name, "vgem") == 0) {
                nodeType = DRM_NODE_PRIMARY;
            }
        }
#endif

        if (device->available_nodes & (1 << nodeType)) {
            const QString path = QString::fromLocal8Bit(device->nodes[nodeType]);
            if (canOpen(path)) {
                return path;
            }
        }
    }

    return QString();
}

WebRTCBackend::WebRTCBackend(QObject *parent)
    : OutputBackend(parent)
{
    if (qEnvironmentVariableIsSet("KWIN_RENDER_NODES")) {
        qCInfo(KWIN_WEBRTC) << "WebRTC backend created; render nodes from KWIN_RENDER_NODES:" << qEnvironmentVariable("KWIN_RENDER_NODES");
        return;
    }
    const QString node = findRenderNodePath();
    if (!node.isEmpty()) {
        qputenv("KWIN_RENDER_NODES", QFile::encodeName(node));
    }
    qCInfo(KWIN_WEBRTC) << "WebRTC backend created; render node" << (node.isEmpty() ? QStringLiteral("(none found)") : node);
}

WebRTCBackend::~WebRTCBackend()
{
    for (BackendOutput *output : std::as_const(m_outputs)) {
        output->unref();
    }
}

bool WebRTCBackend::initialize()
{
    return true;
}

QList<CompositingType> WebRTCBackend::supportedCompositors() const
{
    return {OpenGLCompositing};
}

std::unique_ptr<InputBackend> WebRTCBackend::createInputBackend()
{
    auto input = std::make_unique<WebRTCInputBackend>(this);
    m_inputBackend = input.get();
    return input;
}

std::unique_ptr<EglBackend> WebRTCBackend::createOpenGLBackend(RenderDevice *renderDevice)
{
    return std::make_unique<WebRTCEglBackend>(this, renderDevice);
}

BackendOutput *WebRTCBackend::createVirtualOutput(const QString &name, const QString &description, const QSize &size, qreal scale)
{
    return addOutput(OutputInfo{
        .geometry = Rect(QPoint(), size),
        .scale = scale,
        .connectorName = QStringLiteral("WEBRTC-") + name,
    });
}

void WebRTCBackend::removeVirtualOutput(BackendOutput *output)
{
    if (auto webrtcOutput = qobject_cast<WebRTCOutput *>(output)) {
        removeOutput(webrtcOutput);
    }
}

QList<BackendOutput *> WebRTCBackend::outputs() const
{
    return m_outputs | std::ranges::to<QList<BackendOutput *>>();
}

WebRTCOutput *WebRTCBackend::createOutput(const OutputInfo &info)
{
    WebRTCOutput *output = new WebRTCOutput(this, info.connectorName);
    output->init(info.geometry.topLeft(), info.geometry.size() * info.scale, info.scale, info.modes);
    m_outputs.append(output);
    Q_EMIT outputAdded(output);
    return output;
}

BackendOutput *WebRTCBackend::addOutput(const OutputInfo &info)
{
    WebRTCOutput *output = createOutput(info);
    Q_EMIT outputsQueried();
    return output;
}

void WebRTCBackend::removeOutput(WebRTCOutput *output)
{
    if (m_outputs.removeOne(output)) {
        Q_EMIT outputRemoved(output);
        Q_EMIT outputsQueried();
        output->unref();
    }
}

} // namespace KWin

#include "moc_webrtc_backend.cpp"
