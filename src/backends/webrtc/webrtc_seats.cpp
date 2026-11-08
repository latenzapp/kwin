/*
    KWin WebRTC Backend - Seats

    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Sep-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_seats.h"
#include "webrtc_logging.h"
#include "pipeline/WebRTCLog.hpp"

#include "core/inputdevice.h"
#include "wayland/clientconnection.h"
#include "wayland/output.h"
#include "wayland/outputdevice_v2.h"
#include "wayland/surface.h"
#include "cursor.h"
#include "cursorsource.h"
#include "pointer_input.h"
#include "wayland/pointer.h"
#include "webrtc_egl_backend.h"
#include "input.h"
#include "keyboard_input.h"
#include "wayland/keyboard.h"
#include "xkb.h"
#include "core/output.h"
#include "core/outputbackend.h"
#include "wayland/display.h"
#include "wayland/seat.h"
#include "wayland/surface.h"
#include "main.h"
#include "window.h"
#include "workspace.h"

#include <QSocketNotifier>

#include <wayland-server-core.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>

#include <xkbcommon/xkbcommon.h>

namespace KWin
{

WebRTCSeats &WebRTCSeats::instance()
{
    static WebRTCSeats s;
    return s;
}

void WebRTCSeats::setCount(int count)
{
    m_count = count < 1 ? 1 : count;
    if (m_count > 1) {
        WEBRTC_LOG("Seats: running %d seats (seat N == output N)", m_count);
    }
}

void WebRTCSeats::createSeats(Display *display)
{
    if (!enabled() || !m_seats.empty()) {
        return;
    }
    m_seats.resize(m_count, nullptr);
    m_keymapSent.assign(m_count, false);
    m_shapeHash.assign(m_count, 0);
    m_chosen.assign(m_count, nullptr);
    m_xkb.assign(m_count, nullptr);
    m_cursorShape.assign(m_count, nullptr);
    m_cursorWatched.assign(m_count, false);
    m_lastPos.assign(m_count, QPointF());
    for (int i = 1; i < m_count; ++i) {
        // Named seat1, seat2... beside KWin's own, which takes its name from
        // the logind seat (seat0 when headless). A client sees them
        // as ordinary wl_seat globals and binds whichever it is given.
        auto *seat = new SeatInterface(display, QStringLiteral("seat%1").arg(i), display);
        seat->setHasKeyboard(true);
        seat->setHasPointer(true);
        // Without the capability routeTouch is dead code: a client never binds
        // a wl_touch it was not told about.
        seat->setHasTouch(true);
        m_seats[i] = seat;
        WEBRTC_LOG("Seats: created wl_seat 'seat%d'", i);
    }
}

void WebRTCSeats::createSeatSockets(Display *display)
{
    if (!enabled() || !m_listenFds.empty()) {
        return;
    }
    const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
    if (runtimeDir.isEmpty()) {
        WEBRTC_ERR("Seats: no XDG_RUNTIME_DIR, cannot open per-seat sockets");
        return;
    }
    for (int i = 1; i < m_count; ++i) {
        const QByteArray path = runtimeDir + "/wayland-seat" + QByteArray::number(i);
        ::unlink(path.constData());  // a socket left by a previous run

        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            WEBRTC_ERR("Seats: socket() for seat%d failed: %s", i, strerror(errno));
            continue;
        }
        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        if (static_cast<size_t>(path.size()) >= sizeof(addr.sun_path)) {
            WEBRTC_ERR("Seats: %s is too long for a unix socket", path.constData());
            ::close(fd);
            continue;
        }
        memcpy(addr.sun_path, path.constData(), path.size());
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0
            || ::listen(fd, 8) < 0) {
            WEBRTC_ERR("Seats: bind/listen on %s failed: %s", path.constData(), strerror(errno));
            ::close(fd);
            continue;
        }
        m_listenFds.push_back(fd);

        // Same shape as SecurityContext::onListenFdActivated (wayland/display.cpp),
        // which is what this borrows: accept, hand the fd to the display, and tag
        // the client that comes out of it. The tag is the only thing that
        // survives into the filter.
        auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        const int seatIdx = i;
        connect(notifier, &QSocketNotifier::activated, this, [this, display, seatIdx](QSocketDescriptor sd) {
            const int clientFd = ::accept4(sd, nullptr, nullptr, SOCK_CLOEXEC);
            if (clientFd < 0) {
                return;
            }
            ClientConnection *client = display->createClient(clientFd);
            if (!client) {
                ::close(clientFd);
                return;
            }
            client->setSecurityContextAppId(QStringLiteral("beam-seat%1").arg(seatIdx));
            WEBRTC_LOG("Seats: a client connected on seat%d's socket", seatIdx);
        });
        WEBRTC_LOG("Seats: listening for seat%d clients on %s", i, path.constData());
    }
}

void WebRTCSeats::placeWindowsBySeat()
{
    if (!enabled()) {
        return;
    }
    connect(kwinApp(), &Application::workspaceCreated, this, [this]() {
        connect(workspace(), &Workspace::windowAdded, this, [this](Window *w) {
            if (!w || !w->surface()) {
                return;
            }
            const int seatIdx = seatOfClient(w->surface()->client());
            if (seatIdx <= 0) {
                return;  // seat 0
            }
            const QList<LogicalOutput *> outs = workspace()->outputs();
            if (seatIdx >= outs.size()) {
                return;
            }
            LogicalOutput *want = outs.at(seatIdx);
            if (w->output() == want) {
                return;
            }
            w->sendToOutput(want);
            WEBRTC_LOG("Seats: moved %s to seat%d's output %s", qPrintable(w->resourceClass()),
                       seatIdx, qPrintable(want->name()));
        });
    });
}

int WebRTCSeats::seatOfClient(ClientConnection *client) const
{
    if (!client || !enabled()) {
        return 0;
    }
    const QString appId = client->securityContextAppId();
    if (!appId.startsWith(QLatin1String("beam-seat"))) {
        return 0;  // came in through the normal socket, or is a real sandbox
    }
    bool ok = false;
    const int idx = QStringView(appId).mid(9).toInt(&ok);
    return (ok && idx > 0 && idx < m_count) ? idx : 0;
}

bool WebRTCSeats::allowGlobalFor(ClientConnection *client, const QByteArray &interfaceName,
                                 wl_global *global, Display *display)
{
    const int seatIdx = seatOfClient(client);
    if (seatIdx <= 0 || !display || !global) {
        return true;  // seat 0
    }
    void *ud = wl_global_get_user_data(global);

    if (interfaceName == QByteArrayLiteral("wl_output")) {
        const QList<OutputInterface *> outs = display->outputs();
        for (int i = 0; i < outs.size(); ++i) {
            if (outs.at(i)->globalUserData() != ud) {
                continue;
            }
            // Seat N is output N. A client on seat 1 sees WEBRTC-1 and nothing
            // else, which is what makes an unmodified shell put its panel on
            // one screen instead of on everybody's.
            return i == seatIdx;
        }
        return true;  // not one of ours: leave it alone
    }

    if (interfaceName == QByteArrayLiteral("wl_seat")) {
        SeatInterface *mine = seatFor(seatIdx);
        if (mine && mine->globalUserData() == ud) {
            return true;
        }
        for (SeatInterface *s : display->seats()) {
            if (s->globalUserData() == ud) {
                return false;  // a seat this client does not sit at
            }
        }
        return true;
    }

    return true;
}

bool WebRTCSeats::allowOutputDeviceFor(ClientConnection *client, BackendOutput *output)
{
    // The SAME question as wl_output's, for the output-management side, and
    // it is not optional. Plasma's shell does not build its screens from
    // wl_output: it goes through KScreen, which reads kde_output_device_v2.
    // Hiding one and not the other hands the shell a world that does not
    // agree with itself -- a seat that sees one wl_output and two devices
    // gets a shell that reports ZERO screens and draws nothing.
    const int seatIdx = seatOfClient(client);
    if (seatIdx <= 0 || !output || !kwinApp()->outputBackend()) {
        return true;  // seat 0
    }
    const int i = kwinApp()->outputBackend()->outputs().indexOf(output);
    if (i < 0) {
        return true;  // not one of ours: leave it alone
    }
    return i == seatIdx;
}

SeatInterface *WebRTCSeats::seatFor(int idx) const
{
    if (idx <= 0 || idx >= static_cast<int>(m_seats.size())) {
        return nullptr;
    }
    return m_seats[idx];
}

Window *WebRTCSeats::focusWindowFor(int idx)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat || !workspace()) {
        return nullptr;
    }
    const QList<LogicalOutput *> outs = workspace()->outputs();
    if (idx >= outs.size()) {
        return nullptr;
    }
    Q_UNUSED(outs)
    // A window belongs to the seat whose CLIENT opened it, and to nothing else.
    //
    // There is no fallback to the topmost window on this seat's OUTPUT: seat
    // 0's shell manages EVERY screen, so it would hand another seat the
    // keyboard on seat 0's shell.
    //
    // So a seat with no window of its own focuses nothing, and the key is
    // dropped. That is intended, and it is the safe direction:
    // a key that goes nowhere is a key that did not go to somebody else.
    //
    // The window this seat CHOSE with its cycle key wins, while it lives: a
    // person who switched to their terminal did not stop meaning it because
    // something else opened on top.
    if (idx < static_cast<int>(m_chosen.size())) {
        if (Window *chosen = m_chosen[idx]) {
            if (!chosen->isDeleted() && chosen->surface()
                && seatOfClient(chosen->surface()->client()) == idx) {
                if (seat->focusedKeyboardSurface() != chosen->surface()) {
                    seat->setFocusedKeyboardSurface(chosen->surface());
                }
                return chosen;
            }
            m_chosen[idx] = nullptr;
        }
    }
    // Topmost first: stackingOrder is bottom-to-top.
    const QList<Window *> &stack = workspace()->stackingOrder();
    {
    for (auto it = stack.crbegin(); it != stack.crend(); ++it) {
        Window *w = *it;
        if (!w || w->isDeleted() || !w->isClient() || !w->surface()) {
            continue;
        }
        if (seatOfClient(w->surface()->client()) != idx) {
            continue;
        }
        if (seat->focusedKeyboardSurface() != w->surface()) {
            seat->setFocusedKeyboardSurface(w->surface());
            WEBRTC_LOG("Seats: seat%d focus -> %s on %s", idx,
                       qPrintable(w->resourceClass()),
                       qPrintable(w->output() ? w->output()->name() : QStringLiteral("?")));
        }
        return w;
    }
    }
    return nullptr;
}

SurfaceInterface *WebRTCSeats::focusFor(int idx)
{
    Window *w = focusWindowFor(idx);
    return w ? w->surface() : nullptr;
}

/**
 * Hand this seat the keyboard MAP, once.
 *
 * Only seat 0 gets one for free: Xkb::createKeymapFile() pushes it to
 * waylandServer()->seat() and to nothing else. A wl_keyboard with no keymap is
 * one every toolkit ignores -- the client binds it, receives the key, and has
 * no way to know what it means -- so without this the seat would look wired up
 * and do nothing, which is the failure mode hardest to tell from "seats do not
 * work".
 *
 * Lazily, on first key, and not at creation: the seats are created while the
 * Wayland server comes up, and the input stack that owns the keymap does not
 * exist yet.
 */
void WebRTCSeats::ensureKeymap(int idx, SeatInterface *seat)
{
    if (idx <= 0 || idx >= static_cast<int>(m_keymapSent.size()) || m_keymapSent[idx]) {
        return;
    }
    if (!input() || !input()->keyboard() || !input()->keyboard()->xkb()) {
        return;
    }
    const QByteArray keymap = input()->keyboard()->xkb()->keymapContents();
    if (keymap.isEmpty() || !seat->keyboard()) {
        return;
    }
    seat->keyboard()->setKeymap(keymap);
    m_keymapSent[idx] = true;
    WEBRTC_LOG("Seats: seat%d got the keymap (%lld bytes)", idx, (long long) keymap.size());
}

/** Every window this seat opened, bottom to top. */
static QList<Window *> windowsOfSeat(const WebRTCSeats *seats, int idx)
{
    QList<Window *> out;
    if (!workspace()) {
        return out;
    }
    for (Window *w : workspace()->stackingOrder()) {
        if (!w || w->isDeleted() || !w->isClient() || !w->surface()) {
            continue;
        }
        if (seats->seatOfClient(w->surface()->client()) == idx) {
            out.append(w);
        }
    }
    return out;
}

void WebRTCSeats::cycleWindow(int idx)
{
    const QList<Window *> mine = windowsOfSeat(this, idx);
    if (mine.isEmpty() || idx >= static_cast<int>(m_chosen.size())) {
        return;
    }
    // The one after the current choice, wrapping. With a single window this is
    // a raise of that window, which brings back a window lost behind
    // another.
    int next = 0;
    if (Window *current = m_chosen[idx]) {
        const int at = mine.indexOf(current);
        next = at < 0 ? 0 : (at + 1) % mine.size();
    }
    Window *w = mine.at(next);
    m_chosen[idx] = w;
    workspace()->raiseWindow(w);
    if (SeatInterface *seat = seatFor(idx)) {
        seat->setFocusedKeyboardSurface(w->surface());
    }
    WEBRTC_LOG("Seats: seat%d switched to %s (%d window(s))", idx,
               qPrintable(w->resourceClass()), static_cast<int>(mine.size()));
}

void WebRTCSeats::updateModifiers(int idx, SeatInterface *seat, quint32 keyCode, bool pressed)
{
    if (idx < 0 || idx >= static_cast<int>(m_xkb.size())) {
        return;
    }
    if (!m_xkb[idx]) {
        if (!input() || !input()->keyboard() || !input()->keyboard()->xkb()
            || !input()->keyboard()->xkb()->keymap()) {
            return;
        }
        // The seat was handed this very keymap by ensureKeymap; a state built
        // from any other one would resolve the same keycode differently.
        m_xkb[idx] = xkb_state_new(input()->keyboard()->xkb()->keymap());
        if (!m_xkb[idx]) {
            return;
        }
    }
    // evdev + 8: the offset between kernel keycodes and X/xkb ones.
    xkb_state_update_key(m_xkb[idx], keyCode + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    const quint32 depressed = xkb_state_serialize_mods(m_xkb[idx], XKB_STATE_MODS_DEPRESSED);
    const quint32 latched = xkb_state_serialize_mods(m_xkb[idx], XKB_STATE_MODS_LATCHED);
    const quint32 locked = xkb_state_serialize_mods(m_xkb[idx], XKB_STATE_MODS_LOCKED);
    const quint32 group = xkb_state_serialize_layout(m_xkb[idx], XKB_STATE_LAYOUT_EFFECTIVE);
    seat->notifyKeyboardModifiers(depressed, latched, locked, group);
}

bool WebRTCSeats::routeKey(int idx, quint32 keyCode, bool pressed)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat) {
        return false;
    }
    // The seat's own switcher, on Meta: global shortcuts belong to the whole
    // compositor and cannot be scoped to a seat. Consumed in both directions --
    // forwarding the release alone would leave an application believing Meta
    // is held down.
    if (keyCode == 125 /* KEY_LEFTMETA */) {
        if (pressed) {
            cycleWindow(idx);
        }
        return true;
    }
    ensureKeymap(idx, seat);
    SurfaceInterface *focus = focusFor(idx);
    if (!focus) {
        WEBRTC_LOG("Seats: seat%d has no window on its output; key dropped", idx);
        return false;
    }
    seat->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()));
    // Modifiers BEFORE the key: a client applies the state it was last told,
    // so a Shift announced after the letter capitalises the letter after it.
    updateModifiers(idx, seat, keyCode, pressed);
    seat->notifyKeyboardKey(keyCode,
                            pressed ? KeyboardKeyState::Pressed : KeyboardKeyState::Released,
                            seat->display()->nextSerial());
    return true;
}

void WebRTCSeats::restoreModifiers(int idx, SeatInterface *seat)
{
    xkb_state *st = idx >= 0 && idx < static_cast<int>(m_xkb.size()) ? m_xkb[idx] : nullptr;
    if (!st) {
        seat->notifyKeyboardModifiers(0, 0, 0, 0);
        return;
    }
    seat->notifyKeyboardModifiers(xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED),
                                  xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED),
                                  xkb_state_serialize_mods(st, XKB_STATE_MODS_LOCKED),
                                  xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE));
}

bool WebRTCSeats::routeKeysym(int idx, quint32 keysym)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat || !seat->keyboard() || !input() || !input()->keyboard() || !input()->keyboard()->xkb()) {
        return false;
    }
    ensureKeymap(idx, seat);
    if (!focusFor(idx)) {
        WEBRTC_LOG("Seats: seat%d has no window on its output; text dropped", idx);
        return false;
    }
    Xkb *xkb = input()->keyboard()->xkb();
    seat->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()));
    auto tap = [&](quint32 keyCode) {
        seat->notifyKeyboardKey(keyCode, KeyboardKeyState::Pressed, seat->display()->nextSerial());
        seat->notifyKeyboardKey(keyCode, KeyboardKeyState::Released, seat->display()->nextSerial());
    };
    // Every seat was handed the same keymap (ensureKeymap), so the key that
    // types the character there is the one the compositor's layout names.
    if (const std::optional<Xkb::KeyCode> kc = xkb->keycodeFromKeysym(keysym)) {
        xkb_state *st = idx < static_cast<int>(m_xkb.size()) ? m_xkb[idx] : nullptr;
        const quint32 group = st ? xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE) : 0;
        // The level's own modifiers and nothing else: a locked Caps Lock
        // would turn the 'a' it was asked for into an 'A'.
        seat->notifyKeyboardModifiers(kc->modifiers, 0, 0, group);
        tap(kc->keyCode);
        restoreModifiers(idx, seat);
        return true;
    }
    // No key for it in this layout (an emoji, a symbol from another script):
    // a keymap with that one keysym, to this seat only, then the real one back
    // -- the per-seat form of what KWin's fake-input backend does globally.
    static constexpr quint32 kUnmappedKey = 247;  // a keycode no keyboard sends
    const QByteArray custom = xkb->keymapContentsForKeysym(kUnmappedKey, keysym);
    if (custom.isEmpty()) {
        return false;
    }
    seat->keyboard()->setKeymap(custom);
    seat->notifyKeyboardModifiers(0, 0, 0, 0);
    tap(kUnmappedKey);
    seat->keyboard()->setKeymap(xkb->keymapContents());
    restoreModifiers(idx, seat);
    return true;
}

void WebRTCSeats::ensureCursorWatch(int idx, SeatInterface *seat)
{
    if (idx <= 0 || idx >= static_cast<int>(m_cursorWatched.size()) || m_cursorWatched[idx]) {
        return;
    }
    PointerInterface *pointer = seat->pointer();
    if (!pointer || !input() || !input()->pointer()) {
        return;
    }
    auto *shape = new ShapeCursorSource(seat);
    shape->setTheme(input()->pointer()->cursorTheme());
    m_cursorShape[idx] = shape;
    m_cursorWatched[idx] = true;

    // A shape change repaints IN PLACE: the pointer has not moved, so nothing
    // else would push the new picture and the seat would keep the old one
    // until the next motion.
    QObject::connect(shape, &CursorSource::changed, seat, [this, idx]() {
        sendCursor(idx, m_lastPos[idx]);
    });
    QObject::connect(pointer, &PointerInterface::cursorChanged, seat,
                     [this, idx, shape](const PointerCursor &cursor) {
        if (auto *name = std::get_if<QByteArray>(&cursor)) {
            shape->setShape(*name);
            sendCursor(idx, m_lastPos[idx]);
        } else {
            // A client that draws its own pointer into a wl_surface. Seat 0
            // gets these because the compositor composites that surface into
            // its cursor layer; a seat has no layer to composite into, so
            // surface cursors are not supported for seats above 0. Logged.
            WEBRTC_LOG("Seats: seat%d client set a SURFACE cursor; not carried", idx);
        }
    });
}

void WebRTCSeats::sendCursor(int idx, const QPointF &globalPos)
{
    if (!m_egl || !workspace()) {
        return;
    }
    const QList<LogicalOutput *> outs = workspace()->outputs();
    if (idx < 0 || idx >= outs.size()) {
        return;
    }
    if (idx < static_cast<int>(m_lastPos.size())) {
        m_lastPos[idx] = globalPos;
    }
    if (SeatInterface *seat = seatFor(idx)) {
        ensureCursorWatch(idx, seat);
    }
    // The seat's OWN cursor first, and the global one only until its client has
    // asked for something: Cursors::self() is seat 0's pointer.
    ShapeCursorSource *seatShape = idx < static_cast<int>(m_cursorShape.size()) ? m_cursorShape[idx] : nullptr;
    if (seatShape && seatShape->shape().isEmpty()) {
        seatShape = nullptr;
    }
    Cursor *cursor = Cursors::self() ? Cursors::self()->currentCursor() : nullptr;
    if (!seatShape && !cursor) {
        return;
    }
    const QPointF hotF = seatShape ? seatShape->hotspot() : cursor->hotspot();
    const QPoint hot(static_cast<int>(hotF.x()), static_cast<int>(hotF.y()));

    // Hash-and-compare below, so a shape crosses the bridge on a real change
    // and never on a repaint.
    if (idx < static_cast<int>(m_shapeHash.size())) {
        ShapeCursorSource *shapeSource = seatShape
            ? seatShape
            : (cursor ? qobject_cast<ShapeCursorSource *>(cursor->source()) : nullptr);
        if (shapeSource) {
            const QImage img = shapeSource->image().convertToFormat(QImage::Format_RGBA8888);
            const quint64 hash = img.isNull()
                ? 0
                : qHashBits(img.constBits(), static_cast<size_t>(img.sizeInBytes()));
            if (hash && hash != m_shapeHash[idx]) {
                m_egl->sendCursorImage(static_cast<uint32_t>(idx),
                                       static_cast<uint32_t>(img.width()),
                                       static_cast<uint32_t>(img.height()),
                                       hot.x(), hot.y(), img.constBits(),
                                       static_cast<size_t>(img.sizeInBytes()));
                m_shapeHash[idx] = hash;
                WEBRTC_LOG("Seats: seat%d cursor shape sent (%dx%d)", idx, img.width(), img.height());
            }
        }
    }

    // Output-LOCAL device pixels, and the image's TOP-LEFT rather than the
    // pointer: that is what CursorPosPayload carries and what the cursor layer
    // sends for seat 0 (targetRect().topLeft()).
    const QPointF local = globalPos - outs.at(idx)->geometry().topLeft();
    m_egl->sendCursorPosition(static_cast<uint32_t>(idx),
                              static_cast<int32_t>(local.x()) - hot.x(),
                              static_cast<int32_t>(local.y()) - hot.y(),
                              hot.x(), hot.y(), true);
}

bool WebRTCSeats::routePointerMotion(int idx, const QPointF &globalPos)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat) {
        return false;
    }
    Window *window = focusWindowFor(idx);
    SurfaceInterface *surface = window ? window->surface() : nullptr;
    if (!surface) {
        return false;
    }
    seat->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()));
    // Enter once per surface, then motion. The seat tracks its own focused
    // pointer surface, so this is where the extra seats get a pointer position
    // of their own -- they never touch PointerInputRedirection, which is the
    // single position seat 0 uses.
    //
    // The TRANSFORMATION is not optional. Positions here are global (a second
    // output starts where the first one ends), and the default enter treats
    // them as surface-local, so a toolkit places the pointer far outside the
    // window it is entering, sets no cursor for it and drops its wheel. Same
    // call KWin makes for seat 0 (pointer_input.cpp), for the same reason.
    if (seat->focusedPointerSurface() != surface) {
        seat->notifyPointerEnter(surface, globalPos, window->inputTransformation());
    }
    seat->notifyPointerMotion(globalPos);
    seat->notifyPointerFrame();
    sendCursor(idx, globalPos);
    return true;
}

bool WebRTCSeats::routeScroll(int idx, double dx, double dy)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat || !focusFor(idx)) {
        return false;
    }
    seat->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()));
    // 120 per notch is what one wheel click means to everything downstream --
    // the same convention seat 0's path uses.
    if (dy != 0.0) {
        seat->notifyPointerAxis(Qt::Vertical, dy, static_cast<qint32>(dy * 120),
                                PointerAxisSource::Wheel);
    }
    if (dx != 0.0) {
        seat->notifyPointerAxis(Qt::Horizontal, dx, static_cast<qint32>(dx * 120),
                                PointerAxisSource::Wheel);
    }
    seat->notifyPointerFrame();
    return true;
}

bool WebRTCSeats::routePointerRelative(int idx, const QPointF &delta)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat || !focusFor(idx)) {
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    seat->setTimestamp(now);
    // The relative-pointer protocol only: a seat has no cursor position of its
    // own to advance here, and an application that asked for relative motion
    // has taken the pointer precisely so that it does not move one.
    seat->relativePointerMotion(delta, delta, now);
    seat->notifyPointerFrame();
    return true;
}

bool WebRTCSeats::routeTouch(int idx, int kind, qint32 id, const QPointF &globalPos)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat) {
        return false;
    }
    SurfaceInterface *surface = focusFor(idx);
    if (!surface) {
        return false;
    }
    seat->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()));
    switch (kind) {
    case 0:  // down
        seat->notifyTouchDown(surface, QPointF(), id, globalPos);
        break;
    case 1:  // motion
        seat->notifyTouchMotion(id, globalPos);
        break;
    default: // up
        seat->notifyTouchUp(id);
        break;
    }
    seat->notifyTouchFrame();
    return true;
}

bool WebRTCSeats::routePointerButton(int idx, quint32 button, bool pressed)
{
    SeatInterface *seat = seatFor(idx);
    if (!seat || !focusFor(idx)) {
        return false;
    }
    seat->setTimestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()));
    seat->notifyPointerButton(button,
                              pressed ? PointerButtonState::Pressed : PointerButtonState::Released);
    seat->notifyPointerFrame();
    return true;
}

} // namespace KWin
