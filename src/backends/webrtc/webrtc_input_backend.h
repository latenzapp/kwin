/*
    KWin WebRTC Backend - Input Backend
    Handles remote input (mouse, keyboard, touch, tablet) forwarded by the peer.
    Creates virtual InputDevice objects that emit events to KWin's input system.

    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/inputbackend.h"
#include "core/inputdevice.h"

#include <QObject>
#include <QHash>
#include <QList>
#include <QPointF>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <map>
#include <cstdint>

namespace KWin
{

class WebRTCBackend;
class WebRTCEglBackend;

// ── Virtual Input Devices ────────────────────────────────────────────────────

class WebRTCKeyboardDevice : public InputDevice
{
    Q_OBJECT
public:
    explicit WebRTCKeyboardDevice(QObject *parent = nullptr);

    QString name() const override { return QStringLiteral("webrtc-keyboard"); }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override {}
    bool isKeyboard() const override { return true; }
    bool isPointer() const override { return false; }
    bool isTouchpad() const override { return false; }
    bool isTouch() const override { return false; }
    bool isTabletTool() const override { return false; }
    bool isTabletPad() const override { return false; }
    bool isTabletModeSwitch() const override { return false; }
    bool isLidSwitch() const override { return false; }
};

class WebRTCPointerDevice : public InputDevice
{
    Q_OBJECT
public:
    explicit WebRTCPointerDevice(QObject *parent = nullptr);

    QString name() const override { return QStringLiteral("webrtc-pointer"); }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override {}
    bool isKeyboard() const override { return false; }
    bool isPointer() const override { return true; }
    bool isTouchpad() const override { return false; }
    bool isTouch() const override { return false; }
    bool isTabletTool() const override { return false; }
    bool isTabletPad() const override { return false; }
    bool isTabletModeSwitch() const override { return false; }
    bool isLidSwitch() const override { return false; }
};

class WebRTCTouchDevice : public InputDevice
{
    Q_OBJECT
public:
    explicit WebRTCTouchDevice(QObject *parent = nullptr);

    QString name() const override { return QStringLiteral("webrtc-touch"); }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override {}
    bool isKeyboard() const override { return false; }
    bool isPointer() const override { return false; }
    bool isTouchpad() const override { return false; }
    bool isTouch() const override { return true; }
    bool isTabletTool() const override { return false; }
    bool isTabletPad() const override { return false; }
    bool isTabletModeSwitch() const override { return false; }
    bool isLidSwitch() const override { return false; }
};

/**
 * The pen behind the tablet device. KWin's InputDeviceTabletTool is abstract
 * and libinput's backend answers it from a real tool's serial; there is no
 * physical tool here, so the ids are fixed and the capability list is what the
 * wire can actually carry (input_protocol.h's TabletTool: pressure, tilt,
 * rotation, distance -- no slider and no wheel).
 *
 * One instance per tool TYPE rather than one per stroke: applications key their
 * "which pen is this" state off the tool object, and a fresh one per proximity
 * would look like a new pen every time the user lifted the stylus.
 */
class WebRTCTabletTool : public InputDeviceTabletTool
{
    Q_OBJECT
public:
    explicit WebRTCTabletTool(Type type, QObject *parent = nullptr);

    quint64 serialId() const override { return m_serial; }
    quint64 uniqueId() const override { return m_serial; }
    Type type() const override { return m_type; }
    QList<Capability> capabilities() const override { return m_caps; }

private:
    Type m_type;
    quint64 m_serial;
    QList<Capability> m_caps;
};

class WebRTCTabletDevice : public InputDevice
{
    Q_OBJECT
public:
    explicit WebRTCTabletDevice(QObject *parent = nullptr);

    QString name() const override { return QStringLiteral("webrtc-tablet"); }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override {}
    bool isKeyboard() const override { return false; }
    bool isPointer() const override { return false; }
    bool isTouchpad() const override { return false; }
    bool isTouch() const override { return false; }
    bool isTabletTool() const override { return true; }
    bool isTabletPad() const override { return false; }
    bool isTabletModeSwitch() const override { return false; }
    bool isLidSwitch() const override { return false; }
};

// ── Remote input events ──────────────────────────────────────────────────────
//
// One decoded event, as CBeamInputTranslator produces it from the bridge's
// wire format (input_protocol.h).
//
// `outputIdx` is the output the event applies to: absolute kinds map their
// coordinates within it, and every kind uses it to select the seat.
struct SRemoteInputEvent {
    enum class Kind : uint8_t {
        MoveRel,     // dx, dy in pixels
        MoveAbs,     // x, y normalised [0..1] within outputIdx
        Button,      // button is a Linux BTN_* code, pressed
        Scroll,      // dx, dy in notches (1.0 == one wheel click)
        Key,         // keycode is a raw evdev code (no +8 offset), pressed
        // input_protocol.h opcodes 17-19, decoded by CBeamInputTranslator.
        //
        // `code` is the touch slot (0..kMaxTouchSlots-1); the wire parser
        // rejects an out-of-range one.
        TouchDown,   // code (slot), x, y normalised
        TouchUp,     // code (slot)
        TouchMove,   // code (slot), x, y normalised
        // input_protocol.h opcode 27, decoded by CBeamInputTranslator.
        //
        // Three kinds and not five: proximity in/out and tip down/up are each
        // one kind with `pressed` giving the direction, and the axis-only
        // update is its own kind. Buttons are not a fourth kind -- they ride
        // every event as a held mask, and the backend turns that into presses
        // and releases.
        TabletProximity,  // pressed = entering; x, y, tilt/rotation/distance
        TabletTip,        // pressed = tip touched down; the full axis bundle
        TabletAxis,       // motion, with or without the tip down
        // input_protocol.h opcode 15 (Text), one event per character: `code`
        // is the character's xkb keysym. Typed in the compositor's OWN layout
        // (WebRTCInputBackend::typeKeysym), which is the whole point -- the
        // sender knows a character, not the compositor's layout, and only this
        // side knows which key and level produce it.
        Keysym,
    };

    Kind     kind = Kind::MoveRel;
    uint32_t outputIdx = 0;
    float    x = 0.0f;      // MoveRel/Scroll: delta. MoveAbs/Touch*: normalised
    float    y = 0.0f;
    uint32_t code = 0;      // Button: BTN_*. Key: evdev keycode. Touch*: id.
                            // Keysym: an xkb keysym.
    bool     pressed = false;
    // Tablet-only. Zero on every other kind, and deliberately flat fields
    // rather than a union: this struct crosses a queue by value, an extra
    // twenty bytes on a pointer move costs nothing measurable, and a union
    // would need a tag that `kind` already is.
    float    pressure = 0.0f;   // 0..1
    float    tiltX = 0.0f;      // degrees
    float    tiltY = 0.0f;
    float    rotation = 0.0f;
    float    distance = 0.0f;   // 0..1
    uint8_t  toolType = 0;      // input_proto::TabletToolType
    uint16_t buttons = 0;       // tool buttons HELD, as a mask (bit N = button N)
};

// ── WebRTC Input Backend ─────────────────────────────────────────────────────

class WebRTCInputBackend : public InputBackend
{
    Q_OBJECT

public:
    explicit WebRTCInputBackend(WebRTCBackend *backend, QObject *parent = nullptr);
    ~WebRTCInputBackend() override;

    void initialize() override;

    // Thread-safe: queue decoded events from the bridge's recv thread. They
    // are emitted on the main thread by processQueuedInput.
    void queueEvents(const SRemoteInputEvent *events, size_t count);

    // Process queued input on the main thread (called via event loop)
    Q_INVOKABLE void processQueuedInput();

    // Set the output dimensions for absolute coordinate mapping
    void setOutputSize(uint32_t width, uint32_t height);

    // Multi-monitor: register per-output geometry for abs coordinate mapping
    struct SOutputGeometry {
        int32_t  x = 0, y = 0;
        uint32_t width = 1920, height = 1080;
    };
    void setOutputGeometry(uint32_t outputIdx, int32_t x, int32_t y, uint32_t w, uint32_t h);
    void removeOutputGeometry(uint32_t outputIdx);

private:
    void dispatch(const SRemoteInputEvent &ev);
    // Normalised [0..1] within `outputIdx` -> global pixel coordinates, from
    // that output's geometry (falling back to the primary size).
    QPointF mapAbs(uint32_t outputIdx, float nx, float ny) const;
    WebRTCTabletTool *tabletTool(uint8_t wireType);
    // Types one character on seat 0 in the active layout (see the .cpp).
    void typeKeysym(uint32_t keysym, std::chrono::microseconds now);

    WebRTCBackend *m_backend;
    WebRTCKeyboardDevice *m_keyboard = nullptr;
    WebRTCPointerDevice  *m_pointer  = nullptr;
    WebRTCTouchDevice    *m_touch    = nullptr;
    WebRTCTabletDevice   *m_tablet   = nullptr;
    // One tool per type, created on first use: an application keys its pen
    // state off the tool object, so handing it a new one per stroke would read
    // as a different pen every time.
    QHash<int, WebRTCTabletTool *> m_tabletTools;
    // The pen's state between events. The wire carries a button MASK and no
    // tip state on an axis event, while the compositor wants edges and a tip
    // flag on every one -- so the difference is taken here, in the backend,
    // which is long-lived. The translator is static.
    uint16_t m_tabletButtons = 0;
    bool m_tabletTipDown = false;

    uint32_t m_outputWidth  = 1920;
    uint32_t m_outputHeight = 1080;

    // Multi-monitor: per-output geometry. Each event names the output it
    // applies to, so a move and the output it lands on cannot be separated by
    // the queue.
    std::map<uint32_t, SOutputGeometry> m_outputGeometries;

    // Thread-safe input queue
    std::mutex                      m_queueMutex;
    std::vector<SRemoteInputEvent>  m_queueBuf;
    std::vector<SRemoteInputEvent>  m_drainBuf;
    // Whether a drain is already on the main thread's event queue, so a burst
    // of events costs one post instead of one each (see queueEvents).
    std::atomic<bool>               m_drainScheduled{false};
};

} // namespace KWin
