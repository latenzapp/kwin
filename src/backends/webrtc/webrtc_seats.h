/*
    KWin WebRTC Backend - Seats

    Several people on ONE compositor, each with an output and an input focus of
    their own.

    Seat handling for the WebRTC backend, kept here rather than in input.cpp
    and workspace.cpp wherever possible.

    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Sep-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <kwin_export.h>

#include <QObject>
#include <QPointF>
#include <QPointer>

struct wl_global;
struct xkb_state;

#include <cstdint>
#include <vector>

namespace KWin
{
class BackendOutput;
class ClientConnection;
class CursorSource;
class ShapeCursorSource;
class Display;
class WebRTCEglBackend;
class SeatInterface;
class SurfaceInterface;
class Window;


/**
 * How many seats this compositor runs, and which one an output belongs to.
 *
 * Seat N maps to output N, one to one.
 *
 * Set once from the command line before the backend comes up, and read from
 * both the bridge's receive thread and the compositor's main thread. It is
 * therefore written exactly once, at startup, and never mutated again --
 * which is what makes a plain int safe here rather than an atomic protected
 * by a promise nobody can check.
 */
// Exported for the same reason WebRTCBackend is: main_wayland.cpp lives in the
// kwin_wayland EXECUTABLE and this compiles into libkwin, which is built with
// hidden visibility -- without the macro the flag parses and the link fails.
class KWIN_EXPORT WebRTCSeats : public QObject
{
    Q_OBJECT
public:
    static WebRTCSeats &instance();

    /** From `kwin_wayland --seats N`. Values below 1 are read as 1. */
    void setCount(int count);
    int count() const
    {
        return m_count;
    }

    /**
     * Whether this compositor is running more than one seat.
     *
     * Everything seat-aware branches on this, so a single-seat compositor --
     * which is the default -- takes KWin's normal single-seat paths.
     */
    bool enabled() const
    {
        return m_count > 1;
    }

    /**
     * The seat an event addressed to `outputIdx` belongs to.
     *
     * Out-of-range indices fall back to seat 0, the compositor's normal input
     * path, rather than being dropped.
     */
    int seatForOutput(uint32_t outputIdx) const
    {
        return outputIdx < static_cast<uint32_t>(m_count) ? static_cast<int>(outputIdx) : 0;
    }

    // ── Seats above 0: a second seat, end to end ─────────────────────────────
    //
    // Everything below deliberately does NOT touch InputRedirection, Workspace
    // or the focus chain: seats above 0 are driven straight against their own
    // SeatInterface, which is per-seat by protocol, and seat 0 keeps KWin's
    // normal paths untouched.
    //
    // What that buys, and what it does not: an independent KEYBOARD focus per
    // seat is real (the protocol object owns it). An independent POINTER
    // POSITION is real for the extra seats too, because they never go through
    // PointerInputRedirection. What is NOT here is window management for those
    // seats -- no click-to-raise, no focus chain, no effects: one window per
    // output.

    /** Creates seats 1..N-1 on `display`. Seat 0 is KWin's own, untouched. */
    void createSeats(Display *display);

    /** The protocol seat for index > 0, or nullptr (0, out of range, not created). */
    SeatInterface *seatFor(int idx) const;

    /**
     * Give seat `idx` the keyboard, pointed at the topmost window opened by a
     * client of this seat (or the window chosen with the cycle key).
     *
     * Re-resolved on every event rather than cached: a seat's window may not
     * exist yet when the seat is created, and may later be replaced. Returns
     * the surface it focused, or nullptr when the seat has no window -- in
     * which case the event is dropped, because the alternative is delivering it
     * to somebody else's.
     */
    SurfaceInterface *focusFor(int idx);

    // ── Per-seat client binding ──────────────────────────────────────────────
    //
    // A client belongs to the seat whose SOCKET it connected through, and then
    // sees only that seat's screen and that seat's input. Which is what lets an
    // unmodified application -- Plasma included -- behave as if it were alone.

    /** Listens on $XDG_RUNTIME_DIR/wayland-seat<N> for each seat above 0. */
    void createSeatSockets(Display *display);

    /**
     * Where the cursor of a seat above 0 gets DRAWN.
     *
     * Nowhere, without this. The compositor's cursor layer draws from KWin's
     * own pointer, and a seat above 0 never moves it.
     *
     * The cursor is not composited into the frame: the compositor sends
     * CursorPos/CursorImage per output. So a seat needs those two sent for its
     * own output, from its own pointer -- and the shape has to go with it,
     * because seat 0's cursor never visits the other output and no shape is
     * ever sent for it otherwise.
     */
    void setEglBackend(WebRTCEglBackend *backend)
    {
        m_egl = backend;
    }

    /** Which seat this client belongs to; 0 for everything that came in normally. */
    int seatOfClient(ClientConnection *client) const;

    /**
     * Put every window on the output of the seat that OPENED it.
     *
     * Hiding the other screens from a client is not enough: KWin places by the
     * global active output, so an application that can only see its own screen
     * was still laid out on somebody else's -- the input went to the right
     * person and the pixels to the wrong one. Connected once, when the
     * workspace appears.
     */
    void placeWindowsBySeat();

    /**
     * Whether this client may see this global.
     *
     * Only `wl_output` and `wl_seat` are ever hidden, and only from a client
     * bound to a seat above 0: everything else, and every client on seat 0,
     * gets the normal answer. An unrecognised global of
     * either interface is ALLOWED -- the failure that matters is a client with
     * no screen at all, not one that can see a screen it will not use.
     */
    bool allowGlobalFor(ClientConnection *client, const QByteArray &interfaceName,
                        wl_global *global, Display *display);
    /**
     * The output-management half of allowGlobalFor: whether the device of
     * `output` is announced to `client` by kde_output_device_registry_v2,
     * which since KWin 6.7 carries every output device (they are not
     * globals of their own).
     */
    bool allowOutputDeviceFor(ClientConnection *client, BackendOutput *output);

    /** Returns true if the key was delivered to (or consumed by) seat `idx`. */
    bool routeKey(int idx, quint32 keyCode, bool pressed);
    /**
     * One character (an xkb keysym) typed on seat `idx`: the key and level
     * that produce it in the compositor's layout, or a temporary keymap sent
     * to this seat alone when the layout has no key for it. The Text opcode's
     * per-seat half; seat 0's is WebRTCInputBackend::typeKeysym.
     */
    bool routeKeysym(int idx, quint32 keysym);
    bool routePointerMotion(int idx, const QPointF &globalPos);
    bool routePointerButton(int idx, quint32 button, bool pressed);

    /**
     * The rest of the pointer, and the reason it is not optional.
     *
     * Without these a seat cannot scroll, and anything holding the pointer
     * gets nothing.
     */
    bool routeScroll(int idx, double dx, double dy);
    bool routePointerRelative(int idx, const QPointF &delta);
    bool routeTouch(int idx, int kind, qint32 id, const QPointF &globalPos);

private:
    WebRTCSeats() = default;
    int m_count = 1;
    // Index 0 is always nullptr: seat 0 IS waylandServer()->seat(), and owning
    // a second object for it would be two things claiming one name.
    std::vector<SeatInterface *> m_seats;
    // Whether each seat has been handed the keyboard map; see ensureKeymap.
    std::vector<bool> m_keymapSent;
    // Listening sockets, one per seat above 0, kept for their lifetime.
    std::vector<int> m_listenFds;

    /**
     * The window this seat is typing into, or nullptr.
     *
     * The WINDOW and not just its surface, because a pointer entering it needs
     * the window's input transformation to map a global position into surface
     * coordinates -- see routePointerMotion.
     */
    Window *focusWindowFor(int idx);
    void ensureKeymap(int idx, SeatInterface *seat);
    /**
     * Watch what this seat's client asks the pointer to look like.
     *
     * Seat 0 gets its cursor picture for free: the compositor DRAWS a cursor
     * layer for its output and the layer reads the pixels back. No such layer
     * exists for a seat, so the shape has to be resolved here, from what the
     * client set on its own wl_pointer -- the global cursor is seat 0's.
     */
    void ensureCursorWatch(int idx, SeatInterface *seat);
    /**
     * Keep this seat's modifier state, and tell its client when it changes.
     *
     * A wl_keyboard client does not derive Shift from the key events: it
     * applies the compositor's `modifiers` event and nothing else. Without
     * one, every seat types in lower case forever: a remote Shift reaches the
     * application as an unshifted letter.
     *
     * The state is per seat on purpose. Reusing the global Xkb would make one
     * person's held Shift capitalise somebody else's typing.
     */
    void updateModifiers(int idx, SeatInterface *seat, quint32 keyCode, bool pressed);
    // Re-announces the seat's real modifier state (from m_xkb), after a
    // routeKeysym tap announced a forced one.
    void restoreModifiers(int idx, SeatInterface *seat);
    /** Push this seat's cursor -- shape on change, then position -- to its output. */
    void sendCursor(int idx, const QPointF &globalPos);

    /**
     * Raise the next window of this seat, and keep it as the seat's choice.
     *
     * Global shortcuts are compositor-wide and cannot be scoped to a seat, so
     * one key (Left Meta), per seat, cycles that seat's OWN windows: a seat
     * with several windows needs to reach each of them. routeKey() consumes
     * the key in both directions.
     */
    void cycleWindow(int idx);

    WebRTCEglBackend *m_egl = nullptr;
    /**
     * The window each seat last chose with its cycle key.
     *
     * A guarded pointer: the chosen window can close while it is still the
     * seat's choice, and a raw pointer here would dangle.
     */
    std::vector<QPointer<Window>> m_chosen;
    /**
     * The cursor SHAPE each seat's output was last sent, as a hash.
     *
     * Re-sent whenever the shape changes -- a text caret over a field, a
     * resize arrow, a hand over a link -- and compared by hash, as the cursor
     * layer does for seat 0, so a shape crosses the bridge on a real change
     * and never on a repaint.
     */
    std::vector<quint64> m_shapeHash;
    /** Per-seat xkb state, built from the same keymap the seat was sent. */
    std::vector<xkb_state *> m_xkb;
    /** The theme sprite each seat's client last asked for, by shape name. */
    std::vector<ShapeCursorSource *> m_cursorShape;
    /** Whether ensureCursorWatch has connected this seat's pointer. */
    std::vector<bool> m_cursorWatched;
    /** Where each seat's pointer is, so a shape change can repaint in place. */
    std::vector<QPointF> m_lastPos;
};

} // namespace KWin
