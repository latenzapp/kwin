/*
    SPDX-FileCopyrightText: 2017 David Edmundson <davidedmundson@kde.org>

    Modified by Latenz (Simone Salgaro), Sep-Oct 2026: adds allowGlobal() and
    allowOutputDevice(), for per-seat filtering.

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#pragma once

#include "display.h"

struct wl_global;

namespace KWin
{

class BackendOutput;

class FilteredDisplayPrivate;

/**
 * Server Implementation that allows one to restrict which globals are available to which clients
 *
 * Users of this class must implement the virtual @method allowInterface method.
 */
class KWIN_EXPORT FilteredDisplay : public Display
{
    Q_OBJECT

public:
    FilteredDisplay(QObject *parent);
    ~FilteredDisplay() override;

    /**
     * Return whether the @arg client can see the interface with the given @arg interfaceName
     *
     * When false will not see these globals for a given interface in the registry,
     * and any manual attempts to bind will fail
     *
     * @return true if the client should be able to access the global with the following interfaceName
     */
    virtual bool allowInterface(ClientConnection *client, const QByteArray &interfaceName) = 0;

    /**
     * The same question, about ONE global rather than an interface.
     *
     * `wl_output` and `wl_seat` are one interface with several instances, so a
     * name is not enough to hide one screen from a client and show it another
     * (multiseat). libwayland's filter callback is handed the
     * `wl_global *`; this is where it stops being discarded.
     *
     * Defaults to the name-only answer, so existing subclasses need no
     * change.
     */
    virtual bool allowGlobal(ClientConnection *client, const QByteArray &interfaceName, wl_global *global)
    {
        Q_UNUSED(global)
        return allowInterface(client, interfaceName);
    }

    /**
     * Whether one output device is announced to a client.
     *
     * Since KWin 6.7 an output device is not a `wl_global` of its own: one
     * `kde_output_device_registry_v2` global announces every device to each
     * client that binds it, so allowGlobal() never sees them. The registry
     * asks here instead. Defaults to yes.
     */
    virtual bool allowOutputDevice(ClientConnection *client, BackendOutput *output)
    {
        Q_UNUSED(client)
        Q_UNUSED(output)
        return true;
    }

private:
    std::unique_ptr<FilteredDisplayPrivate> d;
};

}
