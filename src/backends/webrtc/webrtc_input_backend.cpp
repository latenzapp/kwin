/*
    KWin WebRTC Backend - Input Backend Implementation
    Emits remote input through KWin's InputDevice signal system.

    Input reaches this process over the bridge from the peer, in the shared wire
    format of input_protocol.h; CBeamInputTranslator decodes that into
    SRemoteInputEvent (webrtc_input_backend.h) and queues them here.

    Events are stamped locally with steady_clock: nothing upstream carries a
    timestamp this compositor could trust.

    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "webrtc_input_backend.h"
#include "webrtc_backend.h"
#include "webrtc_seats.h"
#include "webrtc_logging.h"
#include "pipeline/WebRTCLog.hpp"

#include "input.h"
#include "keyboard_input.h"
#include "xkb.h"

#include <QCoreApplication>
#include <QTimer>

namespace KWin
{

// ── Virtual Input Devices ────────────────────────────────────────────────────

WebRTCKeyboardDevice::WebRTCKeyboardDevice(QObject *parent) : InputDevice(parent) {}
WebRTCPointerDevice::WebRTCPointerDevice(QObject *parent) : InputDevice(parent) {}
WebRTCTouchDevice::WebRTCTouchDevice(QObject *parent) : InputDevice(parent) {}

// ── WebRTCInputBackend ───────────────────────────────────────────────────────

WebRTCInputBackend::WebRTCInputBackend(WebRTCBackend *backend, QObject *parent)
    : InputBackend(parent)
    , m_backend(backend)
{
}

WebRTCInputBackend::~WebRTCInputBackend()
{
    if (m_keyboard) {
        Q_EMIT deviceRemoved(m_keyboard);
        delete m_keyboard;
    }
    if (m_pointer) {
        Q_EMIT deviceRemoved(m_pointer);
        delete m_pointer;
    }
    if (m_touch) {
        Q_EMIT deviceRemoved(m_touch);
        delete m_touch;
    }
    if (m_tablet) {
        Q_EMIT deviceRemoved(m_tablet);
        delete m_tablet;
    }
    // The tools are children of this object, so Qt deletes them; they are not
    // devices and were never announced, so nothing has to be told.
    m_tabletTools.clear();
}

WebRTCTabletTool::WebRTCTabletTool(Type type, QObject *parent)
    : InputDeviceTabletTool(parent)
    , m_type(type)
    // A stable, non-zero id per tool type. Applications treat serialId as
    // "which physical pen is this"; zero is what libinput reports for a tool
    // that has no serial, and two tools sharing one id would look like the same
    // pen changing shape.
    , m_serial(0xbea3'0000ull | static_cast<quint64>(type))
    // Exactly what input_protocol.h's TabletTool carries. Claiming Slider or
    // Wheel would make an application draw a control the wire can never move.
    , m_caps({Capability::Pressure, Capability::Tilt, Capability::Rotation, Capability::Distance})
{
}

WebRTCTabletDevice::WebRTCTabletDevice(QObject *parent)
    : InputDevice(parent)
{
}

void WebRTCInputBackend::initialize()
{
    m_keyboard = new WebRTCKeyboardDevice(this);
    m_pointer  = new WebRTCPointerDevice(this);
    m_touch    = new WebRTCTouchDevice(this);
    m_tablet   = new WebRTCTabletDevice(this);

    Q_EMIT deviceAdded(m_keyboard);
    Q_EMIT deviceAdded(m_pointer);
    Q_EMIT deviceAdded(m_touch);
    Q_EMIT deviceAdded(m_tablet);

    WEBRTC_LOG("Input backend initialized: keyboard=%p pointer=%p touch=%p tablet=%p",
               (void*)m_keyboard, (void*)m_pointer, (void*)m_touch, (void*)m_tablet);
}

void WebRTCInputBackend::setOutputSize(uint32_t width, uint32_t height)
{
    m_outputWidth  = width;
    m_outputHeight = height;
    // Also update default output geometry (idx 0)
    m_outputGeometries[0] = {0, 0, width, height};
}

void WebRTCInputBackend::setOutputGeometry(uint32_t outputIdx, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    m_outputGeometries[outputIdx] = {x, y, w, h};
    WEBRTC_LOG("Input: setOutputGeometry(%u) = %d,%d %ux%u", outputIdx, x, y, w, h);
}

void WebRTCInputBackend::removeOutputGeometry(uint32_t outputIdx)
{
    m_outputGeometries.erase(outputIdx);
}

QPointF WebRTCInputBackend::mapAbs(uint32_t outputIdx, float nx, float ny) const
{
    auto it = m_outputGeometries.find(outputIdx);
    if (it != m_outputGeometries.end()) {
        return QPointF(static_cast<double>(nx) * it->second.width + it->second.x,
                       static_cast<double>(ny) * it->second.height + it->second.y);
    }
    return QPointF(static_cast<double>(nx) * m_outputWidth,
                   static_cast<double>(ny) * m_outputHeight);
}

void WebRTCInputBackend::queueEvents(const SRemoteInputEvent *events, size_t count)
{
    if (!events || count == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queueBuf.insert(m_queueBuf.end(), events, events + count);
    }

    // One event-loop post per BATCH. Whoever flips the flag owns the post;
    // processQueuedInput clears it before draining, so an event queued in
    // between still gets a post of its own.
    if (!m_drainScheduled.exchange(true)) {
        QMetaObject::invokeMethod(this, &WebRTCInputBackend::processQueuedInput, Qt::QueuedConnection);
    }
}

void WebRTCInputBackend::processQueuedInput()
{
    m_drainScheduled.store(false);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::swap(m_queueBuf, m_drainBuf);
        m_queueBuf.clear();
    }

    for (const SRemoteInputEvent &ev : m_drainBuf) {
        dispatch(ev);
    }
    m_drainBuf.clear();
}

/**
 * The tool object for a wire tool type, created once and kept.
 *
 * The wire's enum (input_protocol.h's TabletToolType) has no Finger; KWin's
 * has Finger between Airbrush and Mouse. They agree for the first five and
 * diverge after, so the mapping is explicit rather than a cast -- a cast would
 * turn a Mouse into a Finger and a Lens into a Mouse, silently.
 */
WebRTCTabletTool *WebRTCInputBackend::tabletTool(uint8_t wireType)
{
    auto it = m_tabletTools.find(wireType);
    if (it != m_tabletTools.end())
        return it.value();

    InputDeviceTabletTool::Type type;
    switch (wireType) {
    case 0:  type = InputDeviceTabletTool::Pen;      break;
    case 1:  type = InputDeviceTabletTool::Eraser;   break;
    case 2:  type = InputDeviceTabletTool::Brush;    break;
    case 3:  type = InputDeviceTabletTool::Pencil;   break;
    case 4:  type = InputDeviceTabletTool::Airbrush; break;
    case 5:  type = InputDeviceTabletTool::Mouse;    break;
    case 6:  type = InputDeviceTabletTool::Lens;     break;
    // The wire parser rejects anything else, so this is unreachable from a
    // conforming peer; a pen is the safe answer for the one that is not.
    default: type = InputDeviceTabletTool::Pen;      break;
    }
    auto *tool = new WebRTCTabletTool(type, this);
    m_tabletTools.insert(wireType, tool);
    return tool;
}

// One character on seat 0, typed similarly to how KWin's fake-input backend
// types a keysym (backends/fakeinput): the key and level that produce it in the
// ACTIVE layout, pressed under exactly that level's modifiers, then the real
// modifier state back. A character the layout has no key for gets a temporary
// keymap with that one keysym on a spare keycode, and the real keymap back
// after. The sender knows the character, not this layout.
void WebRTCInputBackend::typeKeysym(uint32_t keysym, std::chrono::microseconds now)
{
    if (!m_keyboard || !input() || !input()->keyboard() || !input()->keyboard()->xkb())
        return;
    Xkb *xkb = input()->keyboard()->xkb();
    if (const std::optional<Xkb::KeyCode> kc = xkb->keycodeFromKeysym(keysym)) {
        xkb_state *state = xkb->state();
        const xkb_mod_mask_t depressed = xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
        const xkb_mod_mask_t latched = xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
        const xkb_mod_mask_t locked = xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
        const xkb_layout_index_t layout = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE);
        // The level's own modifiers and nothing else -- a locked Caps Lock
        // would turn the 'a' it was asked for into an 'A' -- in the layout the
        // key was looked up in.
        xkb->updateModifiers(kc->modifiers, 0, 0, layout);
        WEBRTC_TRACE("Input: Keysym 0x%x -> keycode=%u mods=0x%x", keysym, kc->keyCode, kc->modifiers);
        Q_EMIT m_keyboard->keyChanged(kc->keyCode, KeyboardKeyState::Pressed, now, m_keyboard);
        Q_EMIT m_keyboard->keyChanged(kc->keyCode, KeyboardKeyState::Released, now, m_keyboard);
        xkb->updateModifiers(depressed, latched, locked, layout);
        xkb->forwardModifiers();
        return;
    }
    static constexpr xkb_keycode_t kUnmappedKey = 247;  // a keycode no keyboard sends
    if (!xkb->updateToKeymapForKeySym(kUnmappedKey, keysym)) {
        WEBRTC_LOG("Input: no keymap can carry keysym 0x%x; character dropped", keysym);
        return;
    }
    WEBRTC_TRACE("Input: Keysym 0x%x -> temporary keymap", keysym);
    Q_EMIT m_keyboard->keyChanged(kUnmappedKey, KeyboardKeyState::Pressed, now, m_keyboard);
    Q_EMIT m_keyboard->keyChanged(kUnmappedKey, KeyboardKeyState::Released, now, m_keyboard);
    xkb->reconfigure();
}

void WebRTCInputBackend::dispatch(const SRemoteInputEvent &ev)
{
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    using Kind = SRemoteInputEvent::Kind;

    // Seats above 0 do not go through KWin's input redirection at all: they are
    // driven against their own SeatInterface, which owns its focus and its
    // pointer position by protocol (see webrtc_seats.h for what that does and
    // does not buy). Seat 0 falls through to KWin's normal input path below;
    // with a single seat WebRTCSeats::enabled() is false and this block is
    // skipped.
    if (WebRTCSeats::instance().enabled()) {
        auto &seats = WebRTCSeats::instance();
        const int seat = seats.seatForOutput(ev.outputIdx);
        if (seat > 0) {
            switch (ev.kind) {
            case Kind::Key:
                seats.routeKey(seat, ev.code, ev.pressed);
                return;
            case Kind::Keysym:
                seats.routeKeysym(seat, ev.code);
                return;
            case Kind::MoveAbs:
                seats.routePointerMotion(seat, mapAbs(ev.outputIdx, ev.x, ev.y));
                return;
            case Kind::Button:
                seats.routePointerButton(seat, ev.code, ev.pressed);
                return;
            case Kind::Scroll:
                // Without this a person at a seat cannot read anything longer
                // than a screen.
                seats.routeScroll(seat, ev.x, ev.y);
                return;
            case Kind::MoveRel:
                seats.routePointerRelative(seat, QPointF(ev.x, ev.y));
                return;
            case Kind::TouchDown:
                seats.routeTouch(seat, 0, static_cast<qint32>(ev.code),
                                 mapAbs(ev.outputIdx, ev.x, ev.y));
                return;
            case Kind::TouchMove:
                seats.routeTouch(seat, 1, static_cast<qint32>(ev.code),
                                 mapAbs(ev.outputIdx, ev.x, ev.y));
                return;
            case Kind::TouchUp:
                seats.routeTouch(seat, 2, static_cast<qint32>(ev.code), QPointF());
                return;
            default:
                // Tablet events are not routed to seats above 0: tablet_v2
                // hangs tools and pads off a per-seat TabletSeatV2Interface,
                // which seats above 0 do not create. Dropped rather than sent
                // to seat 0, which is somebody else's screen.
                return;
            }
        }
    }

    switch (ev.kind) {

    // ── Relative mouse move ──────────────────────────────────────────────
    case Kind::MoveRel: {
        if (!m_pointer)
            return;
        const QPointF delta(static_cast<double>(ev.x), static_cast<double>(ev.y));
        Q_EMIT m_pointer->pointerMotion(delta, delta, now, m_pointer);
        Q_EMIT m_pointer->pointerFrame(m_pointer);
        return;
    }

    // ── Absolute mouse move ──────────────────────────────────────────────
    case Kind::MoveAbs: {
        if (!m_pointer)
            return;
        const QPointF pos = mapAbs(ev.outputIdx, ev.x, ev.y);
        // TRACE, not LOG: this fires on every absolute mouse move, hundreds
        // of times a second while dragging.
        WEBRTC_TRACE("Input: MoveAbs raw=(%f,%f) output=%u -> global=(%f,%f)",
                     ev.x, ev.y, ev.outputIdx, pos.x(), pos.y());
        Q_EMIT m_pointer->pointerMotionAbsolute(pos, now, m_pointer);
        Q_EMIT m_pointer->pointerFrame(m_pointer);
        return;
    }

    // ── Mouse button ─────────────────────────────────────────────────────
    case Kind::Button: {
        if (!m_pointer)
            return;
        const auto state = ev.pressed ? PointerButtonState::Pressed
                                      : PointerButtonState::Released;
        Q_EMIT m_pointer->pointerButtonChanged(ev.code, state, now, m_pointer);
        Q_EMIT m_pointer->pointerFrame(m_pointer);
        return;
    }

    // ── Scroll ───────────────────────────────────────────────────────────
    case Kind::Scroll: {
        if (!m_pointer)
            return;
        const double deltaX = static_cast<double>(ev.x);
        const double deltaY = static_cast<double>(ev.y);
        // 120 per notch is Qt's angleDelta convention (eighths of a degree),
        // which is what one wheel click means to everything downstream.
        if (deltaY != 0.0) {
            Q_EMIT m_pointer->pointerAxisChanged(
                PointerAxis::Vertical, deltaY, static_cast<qint32>(deltaY * 120),
                PointerAxisSource::Wheel, false, false, now, m_pointer);
        }
        if (deltaX != 0.0) {
            Q_EMIT m_pointer->pointerAxisChanged(
                PointerAxis::Horizontal, deltaX, static_cast<qint32>(deltaX * 120),
                PointerAxisSource::Wheel, false, false, now, m_pointer);
        }
        Q_EMIT m_pointer->pointerFrame(m_pointer);
        return;
    }

    // ── Keyboard ─────────────────────────────────────────────────────────
    case Kind::Key: {
        if (!m_keyboard)
            return;
        const auto state = ev.pressed ? KeyboardKeyState::Pressed
                                      : KeyboardKeyState::Released;
        WEBRTC_TRACE("Input: Key keycode=%u pressed=%d", ev.code, ev.pressed ? 1 : 0);
        Q_EMIT m_keyboard->keyChanged(ev.code, state, now, m_keyboard);
        return;
    }
    case Kind::Keysym:
        typeKeysym(ev.code, now);
        return;

    // ── Touch ────────────────────────────────────────────────────────────
    case Kind::TouchDown: {
        if (!m_touch)
            return;
        // TRACE like MoveAbs, and for the same reason: a dragging finger fires
        // this at pointer rate. It is also the only way to see from outside
        // that a touch reached the compositor at all.
        WEBRTC_TRACE("Input: TouchDown slot=%u output=%u raw=(%f,%f)", ev.code, ev.outputIdx, ev.x, ev.y);
        Q_EMIT m_touch->touchDown(static_cast<qint32>(ev.code),
                                  mapAbs(ev.outputIdx, ev.x, ev.y), now, m_touch);
        Q_EMIT m_touch->touchFrame(m_touch);
        return;
    }
    case Kind::TouchUp: {
        if (!m_touch)
            return;
        WEBRTC_TRACE("Input: TouchUp slot=%u", ev.code);
        Q_EMIT m_touch->touchUp(static_cast<qint32>(ev.code), now, m_touch);
        Q_EMIT m_touch->touchFrame(m_touch);
        return;
    }
    // ── Tablet tool ──────────────────────────────────────────────────────
    //
    // The four axes travel on every phase (see SRemoteInputEvent's comment),
    // so the emits differ only in which signal they are. `sliderPosition` is
    // passed as 0: the tool does not advertise Capability::Slider, so nothing
    // downstream reads it.
    case Kind::TabletProximity:
    case Kind::TabletTip:
    case Kind::TabletAxis: {
        if (!m_tablet)
            return;
        auto *tool = tabletTool(ev.toolType);
        const QPointF pos = mapAbs(ev.outputIdx, ev.x, ev.y);
        // The tip's state: authoritative on a Tip event, remembered on an
        // Axis one (the wire does not repeat it), and forced up when the tool
        // leaves proximity, so a lost TipUp cannot leave a stroke open.
        if (ev.kind == Kind::TabletTip)
            m_tabletTipDown = ev.pressed;
        else if (ev.kind == Kind::TabletProximity && !ev.pressed)
            m_tabletTipDown = false;

        // TRACE for MoveAbs's reason: a stroke fires this at pointer rate.
        WEBRTC_TRACE("Input: Tablet kind=%d tip=%d pressure=%f tool=%u output=%u raw=(%f,%f)",
                     (int)ev.kind, (int)m_tabletTipDown, ev.pressure, ev.toolType,
                     ev.outputIdx, ev.x, ev.y);

        if (ev.kind == Kind::TabletProximity) {
            Q_EMIT m_tablet->tabletToolProximityEvent(pos, ev.tiltX, ev.tiltY, ev.rotation,
                                                      ev.distance, ev.pressed, 0.0, tool, now,
                                                      m_tablet);
        } else if (ev.kind == Kind::TabletTip) {
            Q_EMIT m_tablet->tabletToolTipEvent(pos, ev.pressure, ev.tiltX, ev.tiltY,
                                                ev.rotation, ev.distance, ev.pressed, 0.0,
                                                tool, now, m_tablet);
        } else {
            Q_EMIT m_tablet->tabletToolAxisEvent(pos, ev.pressure, ev.tiltX, ev.tiltY,
                                                 ev.rotation, ev.distance, m_tabletTipDown,
                                                 0.0, tool, now, m_tablet);
        }

        // Buttons as edges. The wire carries what is HELD -- so a lost event
        // cannot leave a button stuck down -- and the compositor wants presses
        // and releases, so the difference is taken here.
        const uint16_t changed = static_cast<uint16_t>(ev.buttons ^ m_tabletButtons);
        for (int bit = 0; bit < 16; ++bit) {
            if (!(changed & (1u << bit)))
                continue;
            const bool pressed = (ev.buttons & (1u << bit)) != 0;
            WEBRTC_TRACE("Input: TabletButton button=%d pressed=%d", bit, (int)pressed);
            Q_EMIT m_tablet->tabletToolButtonEvent(static_cast<uint>(bit), pressed, tool, now,
                                                   m_tablet);
        }
        m_tabletButtons = ev.buttons;
        // Leaving proximity ends every stroke and every button: nothing else
        // will tell us they ended, and a pen that left the tablet with a
        // button "held" would keep an application in a drag for ever.
        if (ev.kind == Kind::TabletProximity && !ev.pressed)
            m_tabletButtons = 0;
        return;
    }

    case Kind::TouchMove: {
        if (!m_touch)
            return;
        WEBRTC_TRACE("Input: TouchMove slot=%u output=%u raw=(%f,%f)", ev.code, ev.outputIdx, ev.x, ev.y);
        Q_EMIT m_touch->touchMotion(static_cast<qint32>(ev.code),
                                    mapAbs(ev.outputIdx, ev.x, ev.y), now, m_touch);
        Q_EMIT m_touch->touchFrame(m_touch);
        return;
    }
    }
}

} // namespace KWin

#include "moc_webrtc_input_backend.cpp"
