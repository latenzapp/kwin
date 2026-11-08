/*
    KWin WebRTC Backend - Main Backend
    Headless backend for WebRTC streaming; composited buffers are handed,
    without a copy, to a separate process (the peer).

    Based on KWin's VirtualBackend:
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include "core/outputbackend.h"
#include "utils/filedescriptor.h"

namespace KWin
{
class WebRTCBackend;
class WebRTCOutput;
class DrmDevice;
class RenderDevice;

class WebRTCInputBackend;

class KWIN_EXPORT WebRTCBackend : public OutputBackend
{
    Q_OBJECT

public:
    WebRTCBackend(QObject *parent = nullptr);
    ~WebRTCBackend() override;

    bool initialize() override;

    std::unique_ptr<InputBackend> createInputBackend() override;
    std::unique_ptr<EglBackend> createOpenGLBackend(RenderDevice *renderDevice) override;

    BackendOutput *createVirtualOutput(const QString &name, const QString &description, const QSize &size, qreal scale) override;
    void removeVirtualOutput(BackendOutput *output) override;

    struct OutputInfo
    {
        Rect geometry;
        double scale = 1;
        QList<OutputModeline> modes;
        std::optional<QString> connectorName;
    };
    BackendOutput *addOutput(const OutputInfo &info);

    QList<BackendOutput *> outputs() const override;
    QList<CompositingType> supportedCompositors() const override;

    // Input backend registration (called from createInputBackend)
    void setInputBackend(WebRTCInputBackend *input) { m_inputBackend = input; }
    WebRTCInputBackend *inputBackend() const { return m_inputBackend; }

private:
    WebRTCOutput *createOutput(const OutputInfo &info);
    void removeOutput(WebRTCOutput *output);

    QList<WebRTCOutput *> m_outputs;
    WebRTCInputBackend *m_inputBackend = nullptr;
};

} // namespace KWin
