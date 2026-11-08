// SPDX-FileCopyrightText: 2026 Simone Salgaro
// SPDX-License-Identifier: MIT
//
// Written for Latenz. Opcodes and constants of the remote input messages. Needed to build src/backends/webrtc.
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace input_proto {

enum Op : uint8_t {
  MoveAbs = 1,
  MoveRel = 2,
  ButtonDown = 3,
  ButtonUp = 4,
  Wheel = 5,
  Key = 6,
  Cursor = 11,
  Text = 15,
  TouchDown = 17,
  TouchMove = 18,
  TouchUp = 19,
  TabletTool = 27,
};

enum class TabletPhase : uint8_t {

  ProximityIn = 0,

  Axis = 1,

  TipDown = 2,

  TipUp = 3,

  ProximityOut = 4,
};

enum class TabletToolType : uint8_t {
  Pen = 0,
  Eraser = 1,
  Brush = 2,
  Pencil = 3,
  Airbrush = 4,
  Mouse = 5,
  Lens = 6,
};

constexpr uint8_t kMaxTabletToolType = 6;

constexpr uint8_t kMaxTabletPhase = 4;

constexpr size_t kTabletToolBytes = 34;

constexpr uint8_t kMaxTouchSlots = 10;

constexpr size_t kTouchPosBytes = 11;

constexpr size_t kTouchUpBytes = 2;

constexpr size_t kTextHeaderBytes = 3;

constexpr size_t kMaxTextBytes = 4096;

}
