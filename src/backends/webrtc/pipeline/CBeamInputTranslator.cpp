/*
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "CBeamInputTranslator.hpp"

#include "input_protocol.h"
#include "../webrtc_input_backend.h"

#include <cstring>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>
#include <optional>
#include <string_view>
#include <unordered_map>

// Linux evdev keycodes (linux/input-event-codes.h), not re-included here to
// avoid pulling a kernel header into the KWin build for a dozen constants.
namespace evdev {
constexpr uint32_t ESC = 1, MINUS = 12, EQUAL = 13, BACKSPACE = 14, TAB = 15;
constexpr uint32_t Q = 16, W = 17, E = 18, R = 19, T = 20, Y = 21, U = 22, I = 23, O = 24, P = 25;
constexpr uint32_t LEFTBRACE = 26, RIGHTBRACE = 27, ENTER = 28, LEFTCTRL = 29;
constexpr uint32_t A = 30, S = 31, D = 32, F = 33, G = 34, H = 35, J = 36, K = 37, L = 38;
constexpr uint32_t SEMICOLON = 39, APOSTROPHE = 40, GRAVE = 41, LEFTSHIFT = 42, BACKSLASH = 43;
constexpr uint32_t Z = 44, X = 45, C = 46, V = 47, B = 48, N = 49, M = 50;
constexpr uint32_t COMMA = 51, DOT = 52, SLASH = 53, RIGHTSHIFT = 54, KPASTERISK = 55;
constexpr uint32_t LEFTALT = 56, SPACE = 57, CAPSLOCK = 58;
constexpr uint32_t F1 = 59, F2 = 60, F3 = 61, F4 = 62, F5 = 63, F6 = 64, F7 = 65, F8 = 66, F9 = 67, F10 = 68;
constexpr uint32_t NUMLOCK = 69, SCROLLLOCK = 70;
constexpr uint32_t KP7 = 71, KP8 = 72, KP9 = 73, KPMINUS = 74, KP4 = 75, KP5 = 76, KP6 = 77, KPPLUS = 78;
constexpr uint32_t KP1 = 79, KP2 = 80, KP3 = 81, KP0 = 82, KPDOT = 83;
constexpr uint32_t ISO102ND = 86, F11 = 87, F12 = 88, RO = 89;
constexpr uint32_t KPENTER = 96, RIGHTCTRL = 97, KPSLASH = 98, SYSRQ = 99, RIGHTALT = 100;
constexpr uint32_t HOME = 102, UP = 103, PAGEUP = 104, LEFT = 105, RIGHT = 106, END = 107, DOWN = 108,
                    PAGEDOWN = 109, INSERT = 110, DELETE = 111;
constexpr uint32_t KPEQUAL = 117, PAUSE = 119, YEN = 124;
constexpr uint32_t LEFTMETA = 125, RIGHTMETA = 126, MENU = 127;
}  // namespace evdev

namespace {

// KeyboardEvent.code -> Linux evdev keycode. Targets evdev keycodes (raw, no +8
// offset) as expected by KWin's InputDevice::keyChanged().
const std::unordered_map<std::string_view, uint32_t> &keyTable() {
  static const std::unordered_map<std::string_view, uint32_t> table = {
      {"Escape", evdev::ESC},
      {"Digit1", 2}, {"Digit2", 3}, {"Digit3", 4}, {"Digit4", 5}, {"Digit5", 6},
      {"Digit6", 7}, {"Digit7", 8}, {"Digit8", 9}, {"Digit9", 10}, {"Digit0", 11},
      {"Minus", evdev::MINUS}, {"Equal", evdev::EQUAL}, {"Backspace", evdev::BACKSPACE},
      {"Tab", evdev::TAB},
      {"KeyQ", evdev::Q}, {"KeyW", evdev::W}, {"KeyE", evdev::E}, {"KeyR", evdev::R},
      {"KeyT", evdev::T}, {"KeyY", evdev::Y}, {"KeyU", evdev::U}, {"KeyI", evdev::I},
      {"KeyO", evdev::O}, {"KeyP", evdev::P},
      {"BracketLeft", evdev::LEFTBRACE}, {"BracketRight", evdev::RIGHTBRACE},
      {"Enter", evdev::ENTER}, {"ControlLeft", evdev::LEFTCTRL},
      {"KeyA", evdev::A}, {"KeyS", evdev::S}, {"KeyD", evdev::D}, {"KeyF", evdev::F},
      {"KeyG", evdev::G}, {"KeyH", evdev::H}, {"KeyJ", evdev::J}, {"KeyK", evdev::K},
      {"KeyL", evdev::L},
      {"Semicolon", evdev::SEMICOLON}, {"Quote", evdev::APOSTROPHE}, {"Backquote", evdev::GRAVE},
      {"ShiftLeft", evdev::LEFTSHIFT}, {"Backslash", evdev::BACKSLASH},
      {"KeyZ", evdev::Z}, {"KeyX", evdev::X}, {"KeyC", evdev::C}, {"KeyV", evdev::V},
      {"KeyB", evdev::B}, {"KeyN", evdev::N}, {"KeyM", evdev::M},
      {"Comma", evdev::COMMA}, {"Period", evdev::DOT}, {"Slash", evdev::SLASH},
      {"ShiftRight", evdev::RIGHTSHIFT}, {"NumpadMultiply", evdev::KPASTERISK},
      {"AltLeft", evdev::LEFTALT}, {"Space", evdev::SPACE}, {"CapsLock", evdev::CAPSLOCK},
      {"F1", evdev::F1}, {"F2", evdev::F2}, {"F3", evdev::F3}, {"F4", evdev::F4}, {"F5", evdev::F5},
      {"F6", evdev::F6}, {"F7", evdev::F7}, {"F8", evdev::F8}, {"F9", evdev::F9}, {"F10", evdev::F10},
      {"NumLock", evdev::NUMLOCK}, {"ScrollLock", evdev::SCROLLLOCK},
      {"Numpad7", evdev::KP7}, {"Numpad8", evdev::KP8}, {"Numpad9", evdev::KP9},
      {"NumpadSubtract", evdev::KPMINUS},
      {"Numpad4", evdev::KP4}, {"Numpad5", evdev::KP5}, {"Numpad6", evdev::KP6},
      {"NumpadAdd", evdev::KPPLUS},
      {"Numpad1", evdev::KP1}, {"Numpad2", evdev::KP2}, {"Numpad3", evdev::KP3},
      {"Numpad0", evdev::KP0}, {"NumpadDecimal", evdev::KPDOT},
      // The ISO 102nd key, left of Z: '<' '>' on Italian/German/French boards.
      {"IntlBackslash", evdev::ISO102ND},
      {"F11", evdev::F11}, {"F12", evdev::F12}, {"IntlRo", evdev::RO},
      {"NumpadEnter", evdev::KPENTER}, {"ControlRight", evdev::RIGHTCTRL},
      {"NumpadDivide", evdev::KPSLASH}, {"PrintScreen", evdev::SYSRQ}, {"AltRight", evdev::RIGHTALT},
      {"Home", evdev::HOME}, {"ArrowUp", evdev::UP}, {"PageUp", evdev::PAGEUP},
      {"ArrowLeft", evdev::LEFT}, {"ArrowRight", evdev::RIGHT}, {"End", evdev::END},
      {"ArrowDown", evdev::DOWN}, {"PageDown", evdev::PAGEDOWN},
      {"Insert", evdev::INSERT}, {"Delete", evdev::DELETE},
      {"NumpadEqual", evdev::KPEQUAL}, {"Pause", evdev::PAUSE}, {"IntlYen", evdev::YEN},
      {"MetaLeft", evdev::LEFTMETA}, {"MetaRight", evdev::RIGHTMETA}, {"ContextMenu", evdev::MENU},
  };
  return table;
}

// Linux input-event-codes.h: BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112.
//
// std::nullopt rather than a fallback: an unknown button (back/forward, extra
// mouse buttons) is dropped, never turned into a left click. Dropping an event
// the compositor cannot express is the only harmless answer.
std::optional<uint32_t> buttonToBtnCode(uint8_t button) {
  switch (button) {
    case 0: return 0x110;
    case 1: return 0x112;
    case 2: return 0x111;
    default: return std::nullopt;
  }
}

}  // namespace

namespace NStreaming {

void CBeamInputTranslator::translate(uint16_t outputIdx, const uint8_t *data, size_t len,
                                      KWin::WebRTCInputBackend *inputBackend) {
  if (!inputBackend || len < 1) return;

  using Kind = KWin::SRemoteInputEvent::Kind;
  KWin::SRemoteInputEvent ev;
  // The seat, before the switch, so an opcode that names its own output
  // overwrites it and one that does not inherits it. See the header.
  ev.outputIdx = outputIdx;

  switch (data[0]) {
    case input_proto::MoveAbs: {
      if (len < 10) return;
      ev.kind = Kind::MoveAbs;
      ev.outputIdx = data[1];
      ev.x = input_proto::readF32(data + 2);
      ev.y = input_proto::readF32(data + 6);
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    case input_proto::MoveRel: {
      if (len < 5) return;
      ev.kind = Kind::MoveRel;
      ev.x = static_cast<float>(input_proto::readI16(data + 1));
      ev.y = static_cast<float>(input_proto::readI16(data + 3));
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    case input_proto::ButtonDown:
    case input_proto::ButtonUp: {
      if (len < 3) return;
      const auto btnCode = buttonToBtnCode(data[2]);
      if (!btnCode) return;  // unmapped button: drop, never synthesise a click

      ev.kind = Kind::Button;
      ev.outputIdx = data[1];
      ev.code = *btnCode;
      ev.pressed = data[0] == input_proto::ButtonDown;
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    case input_proto::Wheel: {
      if (len < 3) return;
      ev.kind = Kind::Scroll;
      ev.x = 0.0f;
      // One notch == 1.0; the backend multiplies by 120 for the discrete axis
      // value, matching Qt's angleDelta eighths-of-a-degree convention where
      // 120 == one wheel click.
      ev.y = static_cast<float>(static_cast<int8_t>(data[2]));
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    case input_proto::Key: {
      if (len < 3) return;
      const uint8_t codeLen = data[2];
      if (len < static_cast<size_t>(3 + codeLen)) return;
      const std::string_view code(reinterpret_cast<const char *>(data + 3), codeLen);

      const auto &table = keyTable();
      auto it = table.find(code);
      if (it == table.end()) return;  // unmapped key: drop silently

      ev.kind = Kind::Key;
      ev.code = it->second;
      ev.pressed = data[1] != 0;
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    // ── Touch ────────────────────────────────────────────────────────────
    // input_protocol.h opcodes 17-19, delivered through WebRTCTouchDevice
    // (WebRTCInputBackend::dispatch).
    case input_proto::TouchDown:
    case input_proto::TouchMove: {
      input_proto::TouchPos t;
      if (!input_proto::parseTouchPos(data, len, &t)) return;
      ev.kind = data[0] == input_proto::TouchDown ? Kind::TouchDown : Kind::TouchMove;
      ev.outputIdx = t.mon;
      ev.code = t.id;
      ev.x = t.x;
      ev.y = t.y;
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    case input_proto::TouchUp: {
      uint8_t id = 0;
      if (!input_proto::parseTouchUp(data, len, &id)) return;
      ev.kind = Kind::TouchUp;
      ev.code = id;
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    // ── Tablet tool ──────────────────────────────────────────────────────
    // One wire opcode (27), five phases, three event kinds: proximity in/out
    // and tip down/up are each one kind with `pressed` giving the direction;
    // axis-only updates are their own kind.
    //
    // The button MASK rides through untouched. Turning it into presses and
    // releases needs the previous mask, and this translator is static and
    // stateless by design -- the backend, which is long-lived, keeps that
    // state. Same for the tip's state during an axis event.
    case input_proto::TabletTool: {
      input_proto::TabletToolEvent t;
      if (!input_proto::parseTabletTool(data, len, &t)) return;
      ev.outputIdx = t.mon;
      ev.x = t.x;
      ev.y = t.y;
      ev.pressure = t.pressure;
      ev.tiltX = t.tiltX;
      ev.tiltY = t.tiltY;
      ev.rotation = t.rotation;
      ev.distance = t.distance;
      ev.toolType = static_cast<uint8_t>(t.tool);
      ev.buttons = t.buttons;
      switch (t.phase) {
        case input_proto::TabletPhase::ProximityIn:
        case input_proto::TabletPhase::ProximityOut:
          ev.kind = Kind::TabletProximity;
          ev.pressed = t.phase == input_proto::TabletPhase::ProximityIn;
          break;
        case input_proto::TabletPhase::TipDown:
        case input_proto::TabletPhase::TipUp:
          ev.kind = Kind::TabletTip;
          ev.pressed = t.phase == input_proto::TabletPhase::TipDown;
          break;
        case input_proto::TabletPhase::Axis:
          ev.kind = Kind::TabletAxis;
          break;
      }
      inputBackend->queueEvents(&ev, 1);
      return;
    }
    // ── Text ─────────────────────────────────────────────────────────────
    // A committed string (e.g. from an IME), one Keysym event per character,
    // so the compositor types each in its own layout. Sent only when the
    // compositor advertises kFeatInputText.
    case input_proto::Text: {
      std::string text;
      if (!input_proto::parseText(data, len, text)) return;
      std::vector<KWin::SRemoteInputEvent> evs;
      for (size_t i = 0; i < text.size();) {
        const uint32_t cp = input_proto::nextUtf8Codepoint(text, i);
        const xkb_keysym_t sym = xkb_utf32_to_keysym(cp);
        if (sym == XKB_KEY_NoSymbol) continue;
        ev.kind = Kind::Keysym;
        ev.code = sym;
        evs.push_back(ev);
      }
      if (!evs.empty()) inputBackend->queueEvents(evs.data(), evs.size());
      return;
    }
    // Other opcodes are ignored.
    default:
      return;
  }
}

}  // namespace NStreaming
