/*
    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    Modified by Latenz (Simone Salgaro), Sep-Oct 2026: adds
    OutputInterface::globalUserData().

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "core/output.h"
#include "kwin_export.h"

#include <QObject>
#include <QPoint>
#include <QSize>

struct wl_resource;
struct wl_client;

namespace KWin
{

class LogicalOutput;

}

namespace KWin
{

class ClientConnection;
class Display;
class OutputInterfacePrivate;

/**
 * The OutputInterface class represents a screen. This class corresponds to the Wayland
 * interface @c wl_output.
 */
class KWIN_EXPORT OutputInterface : public QObject
{
    Q_OBJECT

public:
    explicit OutputInterface(Display *display, LogicalOutput *handle, QObject *parent = nullptr);
    ~OutputInterface() override;

    bool isRemoved() const;
    void remove();

    LogicalOutput *handle() const;

    /**
     * The user data of this output's `wl_global`, for a filter that has to tell
     * one instance of `wl_output` from another (multiseat).
     *
     * libwayland's global filter is handed a `wl_global *` and nothing else,
     * and the generated server class keeps its own `m_global` private with no
     * accessor -- so the way back is the other direction: `wl_global_get_user_data`
     * returns the generated object, which is this private, and comparing the
     * two identifies the output. The cast is what makes it exact rather than
     * an assumption about where a base sub-object sits.
     */
    void *globalUserData() const;

    /**
     * @returns all wl_resources bound for the @p client
     */
    QList<wl_resource *> clientResources(wl_client *client) const;

    /**
     * Submit changes to all clients.
     */
    void scheduleDone();

    /**
     * Submit changes to @p client.
     */
    void done(wl_client *client);

    static OutputInterface *get(wl_resource *native);

    Display *display() const;

Q_SIGNALS:
    void removed();

    /**
     * Emitted when a client binds to a given output
     * @internal
     */
    void bound(ClientConnection *client, wl_resource *boundResource);

private:
    std::unique_ptr<OutputInterfacePrivate> d;
};

} // namespace KWin
