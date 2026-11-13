// SPDX-FileCopyrightText: 2026 Simone Salgaro
// SPDX-License-Identifier: MIT
//
// Written for Latenz. The bridge protocol between the compositor and the peer process. Needed to build src/backends/webrtc.
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace kwin_bridge {

inline constexpr uint16_t kProtocolVersion = 10;
inline constexpr const char *kDefaultSocketEnv = "BEAM_KWIN_SOCKET";
inline constexpr const char *kDefaultSocketRelPath = "beam/kwin-bridge.sock";

inline constexpr int kMaxDmabufPlanes = 4;
inline constexpr int kMaxFdsPerMessage = kMaxDmabufPlanes + 1;

enum class MsgType : uint8_t {
  Hello = 1,
  OutputsChanged = 2,
  Frame = 3,
  Clipboard = 4,
  CursorPos = 5,
  CursorImage = 6,
  StreamStart = 10,
  StreamStop = 11,
  ForceIdr = 12,
  SetBitrate = 13,
  SetFps = 14,
  ResubmitLast = 15,
  Input = 16,
  OutputAdd = 17,
  OutputRemove = 18,
  OutputConfigure = 19,
  Capabilities = 20,
  FrameDmabuf = 21,
  BufferDrop = 22,
  ClientHello = 23,
  BufferRelease = 24,
  OutputSetScale = 25,
  OutputSetHdr = 27,
};

enum DmabufFlags : uint8_t {
  kDmabufNewBuffer = 1 << 0,
  kDmabufHasFence = 1 << 1,
  kDmabufForced = 1 << 2,
  kDmabufHdr = 1 << 3,
};

enum BufferReleaseFlags : uint8_t {
  kReleaseHasFence = 1 << 0,
};

enum FeatureBit : uint32_t {
  kFeatDmabufFrames = 1u << 1,
  kFeatNativeFence = 1u << 2,
  kFeatBufferRelease = 1u << 3,
  kFeatDamageRects = 1u << 4,
  kFeatOutputScale = 1u << 5,
  kFeatSeats = 1u << 8,
  kFeatInputText = 1u << 9,
  kFeatHdr = 1u << 10,
};

enum StreamMode : uint32_t {
  kStreamModeBitstream = 0,
  kStreamModeDmabuf = 1,
};

enum CodecBit : uint32_t {
  kCodecH264 = 1u << 0,
  kCodecH265 = 1u << 1,
  kCodecAv1 = 1u << 2,
};

#pragma pack(push, 1)

struct MsgHeader {
  uint8_t type;
  uint8_t flags;
  uint16_t outputIdx;
  uint32_t seq;
};

struct HelloPayload {
  uint16_t protoVersion;
  uint32_t codecMask;
  uint16_t outputCount;
};

struct OutputDesc {
  uint16_t idx;
  int32_t x;
  int32_t y;
  uint32_t nativeW;
  uint32_t nativeH;
  uint32_t fps;
  uint8_t nameLen;
};

struct DmabufFramePayload {
  uint64_t ptsUs;
  uint32_t frameId;
  uint32_t bufferId;
  uint32_t width;
  uint32_t height;
  uint32_t drmFormat;
  uint64_t modifier;
  uint8_t planeCount;
  uint8_t fdCount;
  uint8_t fenceFdIndex;
  uint8_t reserved;
  uint32_t offset[4];
  uint32_t pitch[4];

  uint8_t damageRectCount;
  uint8_t damageFlags;
  uint16_t reserved2;
  struct DamageRect {
    uint16_t x, y, w, h;
  } damageRect[8];
};
inline constexpr int kMaxDamageRects = 8;

inline constexpr size_t kDmabufFramePayloadV1Size =
    offsetof(DmabufFramePayload, damageRectCount);

enum DamageFlags : uint8_t {
  kDamageFull = 1 << 0,
};

inline constexpr uint8_t kNoFenceFd = 0xFF;

struct BufferDropPayload {
  uint32_t bufferId;
};

struct CapabilitiesPayload {
  uint16_t protoVersion;
  uint32_t featureMask;
  uint32_t codecMask;
  uint32_t maxInflightBuffers;
};

struct ClientHelloPayload {
  uint16_t protoVersion;
  uint32_t featureMask;
};

struct BufferReleasePayload {
  uint32_t bufferId;
  uint32_t frameId;
};

struct StreamStartPayload {
  uint32_t codec;
  uint32_t fps;
  uint32_t startKbps;
  uint32_t maxKbps;
  uint32_t mode;
};
inline constexpr size_t kStreamStartPayloadV1Size = 4 * sizeof(uint32_t);

struct SetFpsPayload {
  uint32_t fps;
};

struct CursorPosPayload {
  int32_t x;
  int32_t y;
  int32_t hotspotX;
  int32_t hotspotY;
  uint8_t visible;
};

struct CursorImagePayload {
  uint32_t width;
  uint32_t height;
  int32_t hotspotX;
  int32_t hotspotY;
};
inline constexpr uint32_t kMaxCursorImageBytes = 256 * 256 * 4;

struct OutputAddPayload {
  uint32_t width;
  uint32_t height;
  int32_t x;
  int32_t y;
  uint32_t fps;
};

struct OutputConfigurePayload {
  uint32_t width;
  uint32_t height;
  uint8_t hasPos;
  int32_t x;
  int32_t y;
  uint32_t fps;
};

struct OutputScalePayload {
  uint32_t scaleMilli;
};

inline constexpr uint32_t kMinScaleMilli = 500;
inline constexpr uint32_t kMaxScaleMilli = 4000;

struct OutputHdrPayload {
  uint8_t on;
  uint8_t reserved[3];
  uint32_t referenceNits;
  uint32_t maxNits;
  uint32_t minMilliNits;
};

#pragma pack(pop)

}
