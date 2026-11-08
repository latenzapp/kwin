// SPDX-FileCopyrightText: 2026 Simone Salgaro
// SPDX-License-Identifier: MIT
//
// Written for Latenz. Parsers for the remote input messages the bridge forwards. Needed to build src/backends/webrtc.
//
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "generated/wire_input.h"

namespace input_proto {

struct TabletToolEvent {
  uint8_t mon = 0;
  TabletPhase phase = TabletPhase::Axis;
  TabletToolType tool = TabletToolType::Pen;
  uint16_t buttons = 0;
  float x = 0.0f;
  float y = 0.0f;
  float pressure = 0.0f;
  float tiltX = 0.0f;
  float tiltY = 0.0f;
  float rotation = 0.0f;
  float distance = 0.0f;
};

inline bool parseTabletTool(const uint8_t *data, size_t len, TabletToolEvent *out) {
  if (len < kTabletToolBytes || data[0] != TabletTool) return false;
  if (data[2] > kMaxTabletPhase || data[3] > kMaxTabletToolType) return false;
  TabletToolEvent e;
  e.mon = data[1];
  e.phase = static_cast<TabletPhase>(data[2]);
  e.tool = static_cast<TabletToolType>(data[3]);
  std::memcpy(&e.buttons, data + 4, 2);
  float axes[7];
  std::memcpy(axes, data + 6, sizeof(axes));
  e.x = axes[0]; e.y = axes[1]; e.pressure = axes[2];
  e.tiltX = axes[3]; e.tiltY = axes[4]; e.rotation = axes[5]; e.distance = axes[6];
  *out = e;
  return true;
}

struct TouchPos {
  uint8_t mon = 0;
  uint8_t id = 0;
  float x = 0.0f;
  float y = 0.0f;
};

inline bool parseTouchPos(const uint8_t *data, size_t len, TouchPos *out) {
  if (len < kTouchPosBytes) return false;
  if (data[2] >= kMaxTouchSlots) return false;
  TouchPos t;
  t.mon = data[1];
  t.id = data[2];
  std::memcpy(&t.x, data + 3, 4);
  std::memcpy(&t.y, data + 7, 4);
  *out = t;
  return true;
}

inline bool parseTouchUp(const uint8_t *data, size_t len, uint8_t *outId) {
  if (len < kTouchUpBytes) return false;
  if (data[1] >= kMaxTouchSlots) return false;
  *outId = data[1];
  return true;
}

inline bool parseText(const uint8_t *data, size_t size, std::string &out) {
  if (size < kTextHeaderBytes || data[0] != Text) return false;
  const uint16_t len = static_cast<uint16_t>(data[1] | (data[2] << 8));
  if (len > kMaxTextBytes || size < kTextHeaderBytes + len) return false;
  out.assign(reinterpret_cast<const char *>(data + kTextHeaderBytes), len);
  return true;
}

inline uint32_t nextUtf8Codepoint(const std::string &s, size_t &i) {
  const unsigned char c0 = (unsigned char) s[i];
  int extra;
  uint32_t cp;
  if (c0 < 0x80) { extra = 0; cp = c0; }
  else if ((c0 & 0xe0) == 0xc0) { extra = 1; cp = c0 & 0x1f; }
  else if ((c0 & 0xf0) == 0xe0) { extra = 2; cp = c0 & 0x0f; }
  else if ((c0 & 0xf8) == 0xf0) { extra = 3; cp = c0 & 0x07; }
  else { i += 1; return 0xfffd; }
  if (i + extra >= s.size()) { i = s.size(); return 0xfffd; }
  for (int k = 1; k <= extra; k++) {
    unsigned char c = (unsigned char) s[i + k];
    if ((c & 0xc0) != 0x80) { i += 1; return 0xfffd; }
    cp = (cp << 6) | (c & 0x3f);
  }
  i += extra + 1;
  return cp;
}

inline float readF32(const uint8_t *p) {
  float v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline int16_t readI16(const uint8_t *p) {
  int16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

}
