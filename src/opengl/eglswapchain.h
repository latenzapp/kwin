/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2017 Martin Flöser <mgraesslin@kde.org>

    Modified by Latenz (Simone Salgaro), Aug 2026: splits release() into
    advanceAges() and setReleaseFence(), for buffers loaned to another process.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/drm_formats.h"
#include "core/graphicsbufferallocator.h"
#include "kwin_export.h"
#include "utils/filedescriptor.h"

#include <QList>
#include <QSize>

#include <cstdint>
#include <epoxy/egl.h>
#include <memory>

namespace KWin
{

class GLFramebuffer;
class GLTexture;
class EglContext;
class SyncReleasePoint;
class GraphicsBufferReleasePoint;
class RenderDevice;

class KWIN_EXPORT EglSwapchainSlot
{
public:
    EglSwapchainSlot(GraphicsBuffer *buffer, const std::shared_ptr<EglContext> &context, std::unique_ptr<GLFramebuffer> &&framebuffer, const std::shared_ptr<GLTexture> &texture);
    ~EglSwapchainSlot();

    GraphicsBuffer *buffer() const;
    std::shared_ptr<GLTexture> texture() const;
    GLFramebuffer *framebuffer() const;
    int age() const;
    const FileDescriptor &releaseFd() const;
    std::shared_ptr<SyncReleasePoint> releasePoint();

    static std::shared_ptr<EglSwapchainSlot> create(const std::shared_ptr<EglContext> &context, GraphicsBuffer *buffer);

private:
    bool isBusy() const;

    std::shared_ptr<EglContext> m_context;
    GraphicsBuffer *m_buffer;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    std::shared_ptr<GLTexture> m_texture;
    int m_age = 0;
    std::shared_ptr<GraphicsBufferReleasePoint> m_releasePoint;
    friend class EglSwapchain;
};

class KWIN_EXPORT EglSwapchain
{
public:
    EglSwapchain(GraphicsBufferAllocator *allocator, const std::shared_ptr<EglContext> &context, const GraphicsBufferOptions &options, const std::shared_ptr<EglSwapchainSlot> &seed);
    ~EglSwapchain();

    QSize size() const;
    uint32_t format() const;
    uint64_t modifier() const;
    bool scanout() const;
    const std::shared_ptr<EglContext> &context() const;

    std::shared_ptr<EglSwapchainSlot> acquire();
    void release(std::shared_ptr<EglSwapchainSlot> slot, FileDescriptor &&releaseFence);

    // release() does two unrelated things: it ages the slots (buffer-age
    // tracking, which drives DamageJournal) and it attaches the fence that
    // decides when the slot may be reused. For a compositor that hands the
    // buffer straight back those coincide, so one call is right.
    //
    // They stop coinciding when a buffer is LOANED to another process: the
    // ages must advance at end-of-frame like everywhere else, while the fence
    // is not known until the peer returns the buffer, possibly several
    // composites later. Calling release() late does both late -- the slot
    // acquired by the next beginFrame() reports an age that has not counted
    // the frame just rendered, DamageJournal::accumulate() then returns too
    // small a repaint region, and stale pixels survive in the buffer.
    //
    // So the two halves are separately callable. release() is still the
    // correct call for every non-loaning path and keeps its exact behaviour.
    void advanceAges(const std::shared_ptr<EglSwapchainSlot> &slot);
    void setReleaseFence(const std::shared_ptr<EglSwapchainSlot> &slot, FileDescriptor &&releaseFence);

    void resetBufferAge();

    static std::shared_ptr<EglSwapchain> create(RenderDevice *device, GraphicsBufferOptions options);
    static std::shared_ptr<EglSwapchain> create(GraphicsBufferAllocator *allocator, const std::shared_ptr<EglContext> &context, GraphicsBufferOptions options);

private:
    GraphicsBufferAllocator *m_allocator;
    std::shared_ptr<EglContext> m_context;
    GraphicsBufferOptions m_options;
    QList<std::shared_ptr<EglSwapchainSlot>> m_slots;
};

} // namespace KWin
