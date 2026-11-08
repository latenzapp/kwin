#pragma once

/*
 * CBeamBridge — unix SOCK_SEQPACKET server. Speaks the kwin-bridge
 * protocol (see kwin_bridge_protocol.h, resolved via BEAM_PROTO_DIR) to a
 * single peer process, which owns everything downstream.
 *
 * Threading: a dedicated accept thread loops on accept4() only; each
 * accepted client's recv loop runs on its own thread so a live client never
 * blocks the accept thread from picking up (and replacing) the next
 * connection.
 *
 * It carries no encoded video: what goes out is the composited buffer's file
 * descriptor and what comes back is a release. The peer is a separate
 * program.
 *
 * SPDX-FileCopyrightText: 2026 Simone Salgaro
 *
 * Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
 * part of upstream KWin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kwin_bridge_protocol.h"

namespace NStreaming {

class CBeamBridge {
 public:
  struct OutputDescriptor {
    uint16_t idx = 0;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t nativeW = 0;
    uint32_t nativeH = 0;
    uint32_t fps = 0;
    std::string name;
  };

  using FnStreamStart = std::function<void(uint16_t outputIdx, uint32_t codec, uint32_t fps,
                                            uint32_t startKbps, uint32_t maxKbps, uint32_t mode)>;
  // The peer is done with a loaned buffer. `fenceFd` is a sync_file to hand
  // to EglSwapchain::release (so the slot stays busy until the peer's GPU
  // reads retire), or -1. OWNERSHIP OF fenceFd TRANSFERS TO THE CALLBACK --
  // it must close it or give it to something that will.
  using FnBufferRelease = std::function<void(uint16_t outputIdx, uint32_t bufferId,
                                             uint32_t frameId, int fenceFd)>;
  // The peer could not resolve a bufferId and wants its descriptors resent.
  // BufferDrop means "forget this id" in both directions.
  using FnBufferForget = std::function<void(uint16_t outputIdx, uint32_t bufferId)>;
  using FnStreamStop = std::function<void(uint16_t outputIdx)>;
  using FnSetFps = std::function<void(uint16_t outputIdx, uint32_t fps)>;
  // `outputIdx` is MsgHeader.outputIdx: the OUTPUT, and therefore the SEAT,
  // this input belongs to (see webrtc_seats.h).
  using FnInput = std::function<void(uint16_t outputIdx, const uint8_t *data, size_t len)>;
  using FnClientEvent = std::function<void()>;
  using FnOutputAdd = std::function<void(uint32_t width, uint32_t height, int32_t x, int32_t y,
                                          uint32_t fps)>;
  using FnOutputRemove = std::function<void(uint16_t outputIdx)>;
  using FnOutputConfigure = std::function<void(uint16_t outputIdx, uint32_t width, uint32_t height,
                                                bool hasPos, int32_t x, int32_t y, uint32_t fps)>;
  // `scale` is already validated against kMinScaleMilli/kMaxScaleMilli and
  // converted out of the wire's fixed point by the time it gets here.
  using FnOutputSetScale = std::function<void(uint16_t outputIdx, double scale)>;

  CBeamBridge();
  ~CBeamBridge();

  // Binds and starts listening on `socketPath` (created if missing, 0700).
  bool init(const std::string &socketPath);

  // Must be called before init(): these end up in the Capabilities message
  // sent to every client that connects.
  void setDmabufCapabilities(bool nativeFence, uint32_t maxInflightBuffers) {
    m_nativeFenceSupported = nativeFence;
    m_maxInflightBuffers = maxInflightBuffers;
  }

  void shutdown();

  // Replaces the current output list and, if a client is connected, sends
  // an OutputsChanged message immediately.
  void setOutputs(const std::vector<OutputDescriptor> &outputs);

  // Cursor position/visibility update for outputIdx. Cheap, sent inline
  // under m_sendMutex (no queue: tiny fixed-size payload, called at most
  // once per compositor tick, never on the hot frame path).
  void sendCursorPos(uint16_t outputIdx, int32_t x, int32_t y, int32_t hotspotX,
                      int32_t hotspotY, bool visible);

  // Cursor shape update for outputIdx; rgba is straight-alpha RGBA8,
  // width*height*4 bytes, row-major. Silently dropped if it exceeds
  // kMaxCursorImageBytes.
  void sendCursorImage(uint16_t outputIdx, uint32_t width, uint32_t height, int32_t hotspotX,
                        int32_t hotspotY, const uint8_t *rgba, size_t rgbaLen);

  // Sends one loaned DMABUF frame. `fds` (plane fds, then optionally the
  // acquire fence) ride in an SCM_RIGHTS control message and are NOT closed
  // here -- the caller owns them, because they belong to a swapchain slot
  // that outlives this call.
  //
  // Sent INLINE on the calling (compositor) thread: the message is ~90 bytes,
  // and the loan has to be recorded by the caller atomically with the send or
  // the next composite races it.
  //
  // Returns false if there is no client or the send failed; the caller must
  // then NOT record a loan, because no release will ever come back.
  bool sendDmabufFrame(uint16_t outputIdx, const kwin_bridge::DmabufFramePayload &payload,
                       uint8_t flags, const int *fds, int fdCount);

  // Tells the peer a bufferId is dead so it can drop its import-cache entry.
  void sendBufferDrop(uint16_t outputIdx, uint32_t bufferId);

  // What the peer advertised in ClientHello, or 0 if it never sent one (a
  // v1 peer, which by definition supports neither dmabuf nor fences).
  uint32_t peerFeatures() const { return m_peerFeatures.load(); }

  void setOnStreamStart(FnStreamStart cb) { m_onStreamStart = std::move(cb); }
  void setOnStreamStop(FnStreamStop cb) { m_onStreamStop = std::move(cb); }
  void setOnSetFps(FnSetFps cb) { m_onSetFps = std::move(cb); }
  void setOnInput(FnInput cb) { m_onInput = std::move(cb); }
  void setOnClientConnected(FnClientEvent cb) { m_onClientConnected = std::move(cb); }
  void setOnClientDisconnected(FnClientEvent cb) { m_onClientDisconnected = std::move(cb); }
  void setOnOutputAdd(FnOutputAdd cb) { m_onOutputAdd = std::move(cb); }
  void setOnOutputRemove(FnOutputRemove cb) { m_onOutputRemove = std::move(cb); }
  void setOnOutputConfigure(FnOutputConfigure cb) { m_onOutputConfigure = std::move(cb); }
  void setOnOutputSetScale(FnOutputSetScale cb) { m_onOutputSetScale = std::move(cb); }
  void setOnBufferRelease(FnBufferRelease cb) { m_onBufferRelease = std::move(cb); }
  void setOnBufferForget(FnBufferForget cb) { m_onBufferForget = std::move(cb); }

 private:

  void acceptLoop();
  void recvLoop(int fd);
  // Returns how many of `fds` it took ownership of; recvLoop closes the rest.
  int handleMessage(const uint8_t *data, size_t len, int *fds, int fdCount);

  /**
   * What a send does when the socket buffer is full.
   *
   * This socket is written from the COMPOSITOR THREAD (sendDmabufFrame is
   * inline by design), so a peer that stops reading would stall the desktop:
   * SO_SNDTIMEO bounds each blocked send at 50 ms, but the cursor layer pushes
   * a CursorPos on every pointer move, and 50 ms per message on the thread
   * that also drains the input queue is a stall, just a bounded one: a peer
   * that never reads fills the socket in about a second, after which input
   * can sit queued for many seconds.
   *
   * DropOnFull is the answer for the high-frequency messages, and it is safe
   * for a reason particular to each: a dropped frame is already a handled path
   * (the caller reclaims the loan and forgets the buffer id), and a dropped
   * cursor message is state the next one supersedes -- with m_cursorDirty
   * making sure a "next one" happens even if the pointer stops moving.
   *
   * Blocking stays the default, for the rare protocol events that must arrive:
   * Hello, Capabilities, OutputsChanged, BufferDrop.
   */
  enum class SendPolicy { Blocking, DropOnFull };

  /**
   * Re-sends the cached cursor state if a DropOnFull send lost it.
   *
   * Called from the frame path, which is the only thing guaranteed to run
   * again: a cursor that stops moving produces no further CursorPos, so
   * without this a shape or position dropped once would stay wrong until the
   * user moved the mouse. Cheap when clean -- one relaxed atomic load.
   */
  void replayCursorIfDirty();
  std::atomic<bool> m_cursorDirty{false};
  /** DropOnFull sends the peer was too slow to take. Reported, never fatal. */
  std::atomic<uint64_t> m_dropped{0};

  // The ONE place a message reaches the client. Takes m_sendMutex and reads
  // m_clientFd inside it, which is the property that matters: every sender
  // that read the fd first and locked afterwards owned a window in which
  // acceptLoop could close that fd -- and hand its number straight back to
  // the next open() in this process -- between the read and the send.
  //
  // `what` names the message for the throttled failure log, and is also why
  // this is not a template: one line per message type is worth more than the
  // inlining.
  bool sendToClient(const char *what, const void *buf, size_t len, SendPolicy policy,
                    const int *fds = nullptr, int fdCount = 0);

  void sendHello();
  // Sent immediately after Hello. A separate message because HelloPayload is
  // followed by a variable-length OutputDesc[] and so cannot be extended:
  // existing payload layouts never change.
  void sendCapabilities();
  // Serializes the current output list (HelloPayload + OutputDesc[] -- the
  // wire layout Hello and OutputsChanged share) and sends it as msgType.
  // Caller must hold m_outputsMutex.
  void sendOutputListLocked(uint8_t msgType);
  void replayCursorCache();
  // Serialize-and-send only: shared by the public setters (which update the
  // cache first) and by replayCursorCache (which must not).
  void sendCursorPosMsg(uint16_t outputIdx, int32_t x, int32_t y, int32_t hotspotX,
                        int32_t hotspotY, bool visible);
  void sendCursorImageMsg(uint16_t outputIdx, uint32_t width, uint32_t height,
                          int32_t hotspotX, int32_t hotspotY, const uint8_t *rgba,
                          size_t rgbaLen);
  void closeClient();

  std::string m_socketPath;
  // HelloPayload/CapabilitiesPayload carry a codecMask and this process always
  // writes 0 into it: existing payload layouts never change, and this process
  // does not encode, so it advertises no codecs.
  static constexpr uint32_t kCodecMask = 0;

  int m_listenFd = -1;
  std::atomic<int> m_clientFd{-1};
  std::atomic<bool> m_running{false};
  std::thread m_acceptThread;
  std::thread m_recvThread;

  mutable std::mutex m_outputsMutex;
  std::vector<OutputDescriptor> m_outputs;

  // Last known cursor pos/image per output, kept up to date regardless of
  // whether a client is attached (sendCursorPos/sendCursorImage update this
  // unconditionally) and replayed to a freshly (re)connected client right
  // after sendHello -- same "bring the new client up to date" pattern as
  // m_outputs. Needed because the cursor layer only re-renders (and thus
  // only calls sendCursorImage) on a REAL shape change; a client that
  // wasn't attached yet for the one time the shape was last computed would
  // otherwise never learn it.
  struct CachedCursor {
    bool havePos = false;
    int32_t x = 0, y = 0, hotspotX = 0, hotspotY = 0;
    bool visible = false;
    bool haveImage = false;
    uint32_t width = 0, height = 0;
    int32_t imgHotspotX = 0, imgHotspotY = 0;
    std::vector<uint8_t> rgba;
  };
  mutable std::mutex m_cursorMutex;
  std::map<uint16_t, CachedCursor> m_cursorCache;

  std::mutex m_sendMutex;  // serializes writes to m_clientFd

  std::atomic<uint32_t> m_peerFeatures{0};
  // Advertised in Capabilities. Set by the backend before init() -- it is the
  // one that knows whether this EGL display can make a native fence, and how
  // many buffers it is willing to have on loan at once.
  bool m_nativeFenceSupported = false;
  uint32_t m_maxInflightBuffers = 2;

  FnStreamStart m_onStreamStart;
  FnStreamStop m_onStreamStop;
  FnSetFps m_onSetFps;
  FnInput m_onInput;
  FnClientEvent m_onClientConnected;
  FnClientEvent m_onClientDisconnected;
  FnOutputAdd m_onOutputAdd;
  FnOutputRemove m_onOutputRemove;
  FnOutputConfigure m_onOutputConfigure;
  FnOutputSetScale m_onOutputSetScale;
  FnBufferRelease m_onBufferRelease;
  FnBufferForget m_onBufferForget;

  CBeamBridge(const CBeamBridge &) = delete;
  CBeamBridge &operator=(const CBeamBridge &) = delete;
};

}  // namespace NStreaming
