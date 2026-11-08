/*
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "CBeamBridge.hpp"
#include "../webrtc_seats.h"

#include "WebRTCLog.hpp"
#include "kwin_bridge_protocol.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

using namespace kwin_bridge;

namespace NStreaming {

CBeamBridge::CBeamBridge() = default;

CBeamBridge::~CBeamBridge() { shutdown(); }

bool CBeamBridge::init(const std::string &socketPath) {
  m_socketPath = socketPath;

  ::unlink(m_socketPath.c_str());

  m_listenFd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (m_listenFd < 0) {
    WEBRTC_LOG("CBeamBridge: socket() failed: %s", strerror(errno));
    return false;
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(m_listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    WEBRTC_LOG("CBeamBridge: bind(%s) failed: %s", m_socketPath.c_str(), strerror(errno));
    ::close(m_listenFd);
    m_listenFd = -1;
    return false;
  }
  ::chmod(m_socketPath.c_str(), 0700);

  if (::listen(m_listenFd, 1) < 0) {
    WEBRTC_LOG("CBeamBridge: listen() failed: %s", strerror(errno));
    ::close(m_listenFd);
    m_listenFd = -1;
    return false;
  }

  m_running.store(true);
  m_acceptThread = std::thread(&CBeamBridge::acceptLoop, this);

  WEBRTC_LOG("CBeamBridge: listening on %s", m_socketPath.c_str());
  return true;
}

void CBeamBridge::shutdown() {
  if (!m_running.exchange(false)) return;

  if (m_listenFd >= 0) {
    ::shutdown(m_listenFd, SHUT_RDWR);
    ::close(m_listenFd);
    m_listenFd = -1;
  }
  closeClient();

  if (m_acceptThread.joinable()) m_acceptThread.join();
  // Only joined here, after m_acceptThread has fully exited: while running,
  // m_recvThread is owned exclusively by acceptLoop() (it joins the
  // previous one itself when a new client replaces the old).
  if (m_recvThread.joinable()) m_recvThread.join();

  ::unlink(m_socketPath.c_str());
}

void CBeamBridge::closeClient() {
  // compare_exchange, not exchange: recvLoop's own exit path closes the fd
  // too, and a plain read-then-store in either place leaves a window where
  // BOTH decide they own it. A double close in this process does not fail
  // quietly -- the second one closes whatever number the kernel handed out in
  // between, which here is a dmabuf, an EGL sync file or the Wayland socket.
  // Whoever wins the exchange closes; the loser does nothing.
  //
  // Under m_sendMutex as well: sendToClient reads the fd and calls sendmsg on
  // it under that lock, so a close outside it can land between the two. The
  // send then fails with EBADF -- or, if the kernel has already handed the
  // number to a dmabuf, a sync file or a socket, writes to that instead.
  std::lock_guard<std::mutex> slk(m_sendMutex);
  int fd = m_clientFd.load();
  while (fd >= 0 && !m_clientFd.compare_exchange_weak(fd, -1)) {
    // fd is reloaded by compare_exchange_weak on failure.
  }
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
}

// Every message goes out through here, which is what makes "read the fd under
// the same lock that serializes the write" true for all of them.
bool CBeamBridge::sendToClient(const char *what, const void *buf, size_t len,
                               SendPolicy policy, const int *fds, int fdCount) {
  union {
    char control[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)];
    struct cmsghdr align;
  } cmsgbuf{};

  struct iovec iov{const_cast<void *>(buf), len};
  struct msghdr mh{};
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  if (fdCount > 0) {
    mh.msg_control = cmsgbuf.control;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * fdCount);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * fdCount);
    std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * fdCount);
  }

  // MSG_DONTWAIT turns "the socket buffer is full" into an immediate EAGAIN
  // instead of up to SO_SNDTIMEO of the compositor thread. See SendPolicy.
  const int flags = MSG_NOSIGNAL | (policy == SendPolicy::DropOnFull ? MSG_DONTWAIT : 0);

  std::lock_guard<std::mutex> slk(m_sendMutex);
  const int fd = m_clientFd.load();
  if (fd < 0) return false;
  if (::sendmsg(fd, &mh, flags) >= 0) return true;

  // A full buffer under DropOnFull is backpressure, not a fault: the caller
  // handles it (a frame reclaims its loan, a cursor message marks itself
  // dirty). Counted and reported periodically rather than logged per drop,
  // because a wedged peer would otherwise produce one line per frame -- but
  // reported at all, because a stream that quietly delivers nothing is the
  // failure this whole path exists to survive.
  if (policy == SendPolicy::DropOnFull && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    const uint64_t n = ++m_dropped;
    if (n == 1 || n % 240 == 0) {
      WEBRTC_INFO("CBeamBridge: peer not reading, dropped %llu message(s), latest %s",
                  static_cast<unsigned long long>(n), what);
    }
    return false;
  }

  // "The peer left" is not an error. ECONNRESET/EPIPE/ENOTCONN here mean the
  // client went away between the last message and this one, which happens on
  // every ordinary restart of the peer -- broadcastOutputList() fires on connect
  // and can race a client that has already gone. Logged at debug level, not
  // as an error. The next client gets the same state in its Hello.
  //
  // Everything else IS worth a line, and EAGAIN especially: reaching it HERE
  // means a Blocking send sat out the full SO_SNDTIMEO, i.e. a peer that is
  // connected and has stopped reading long enough to stall a protocol event.
  // The high-frequency messages never arrive here -- they are DropOnFull and
  // returned above.
  const int err = errno;
  if (err == ECONNRESET || err == EPIPE || err == ENOTCONN) {
    WEBRTC_LOG("CBeamBridge: send(%s) to a client that had already gone: %s", what,
               strerror(err));
    return false;
  }
  static int nlog = 0;
  if (nlog++ < 8) WEBRTC_ERR("CBeamBridge: send(%s) failed: %s", what, strerror(err));
  return false;
}

void CBeamBridge::acceptLoop() {
  while (m_running.load()) {
    int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      if (!m_running.load()) break;
      continue;
    }

    WEBRTC_LOG("CBeamBridge: client connected (fd=%d)", fd);

    // One client at a time: replace whoever was there. closeClient() shuts down
    // the old fd so its recv thread's blocking recv() unblocks; join it before
    // installing the new one so cleanup (m_onClientDisconnected) runs in order
    // and only one recv thread is ever live. Joining first also keeps the
    // m_recvThread assignment below legal: assigning to a joinable std::thread
    // terminates.
    closeClient();
    if (m_recvThread.joinable()) m_recvThread.join();

    // Bounded sends, for the messages that still block. This socket is written
    // from the COMPOSITOR thread (sendDmabufFrame is inline by design), and a
    // blocking sendmsg on a peer that has stopped reading stalls the whole
    // desktop for as long as the peer stays wedged -- the loan cap bounds
    // buffers in flight, not pressure on the socket buffer. SOCK_SEQPACKET
    // never sends partially, so a timeout means the datagram did not go, which
    // is exactly the answer every caller here already handles.
    //
    // The frame and cursor paths do not wait at all
    // (SendPolicy::DropOnFull), so what this bounds is the handful of protocol
    // events that must arrive: Hello, Capabilities, OutputsChanged,
    // BufferDrop. Those are rare enough that 50 ms of the compositor thread is
    // an acceptable worst case, and dropping one is not.
    {
      const char *env = std::getenv("BEAM_KWIN_BRIDGE_SEND_TIMEOUT_MS");
      const int ms = (env && env[0]) ? std::atoi(env) : 50;
      if (ms > 0) {
        struct timeval tv{ms / 1000, (ms % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      }
    }

    m_clientFd.store(fd);

    // A new client has told us nothing yet. Stale features from the previous
    // one would make us send it a transport it never asked for.
    m_peerFeatures.store(0);

    sendHello();
    sendCapabilities();
    replayCursorCache();
    if (m_onClientConnected) m_onClientConnected();

    // recvLoop runs on its own thread (not inline here) so this loop can
    // immediately go back to accept4() and pick up — and preempt — the next
    // client while this one is still connected. Running it inline would
    // block acceptLoop for the entire lifetime of the current client, so a
    // second connect() (e.g. a probe that connects and sends nothing)
    // would sit accepted-by-the-kernel-but-never-accept()-ed and hang
    // forever waiting for a Hello it would never receive.
    m_recvThread = std::thread(&CBeamBridge::recvLoop, this, fd);
  }
}

void CBeamBridge::sendHello() {
  std::lock_guard<std::mutex> olk(m_outputsMutex);
  sendOutputListLocked(static_cast<uint8_t>(MsgType::Hello));
}

void CBeamBridge::sendOutputListLocked(uint8_t msgType) {
  std::vector<uint8_t> buf(sizeof(MsgHeader) + sizeof(HelloPayload));
  auto *hdr = reinterpret_cast<MsgHeader *>(buf.data());
  *hdr = MsgHeader{msgType, 0, 0, 0};

  // hello fields are written before the resize loop below invalidates the
  // pointer -- keep it that way.
  auto *hello = reinterpret_cast<HelloPayload *>(buf.data() + sizeof(MsgHeader));
  hello->protoVersion = kProtocolVersion;
  hello->codecMask = kCodecMask;
  hello->outputCount = static_cast<uint16_t>(m_outputs.size());

  for (const auto &o : m_outputs) {
    OutputDesc desc{};
    desc.idx = o.idx;
    desc.x = o.x;
    desc.y = o.y;
    desc.nativeW = o.nativeW;
    desc.nativeH = o.nativeH;
    desc.fps = o.fps;
    desc.nameLen = static_cast<uint8_t>(std::min<size_t>(o.name.size(), 255));

    size_t off = buf.size();
    buf.resize(off + sizeof(OutputDesc) + desc.nameLen);
    std::memcpy(buf.data() + off, &desc, sizeof(OutputDesc));
    std::memcpy(buf.data() + off + sizeof(OutputDesc), o.name.data(), desc.nameLen);
  }

  // Logged on failure rather than dropped in silence: a Hello that never
  // arrived leaves a client that will never learn the output list, and the
  // only symptom is a stream nobody ever starts.
  sendToClient(msgType == static_cast<uint8_t>(MsgType::Hello) ? "Hello" : "OutputsChanged",
               buf.data(), buf.size(), SendPolicy::Blocking);
}

void CBeamBridge::sendCapabilities() {
  // Bit 0 (encoded-bitstream frames) is never advertised: this compositor
  // does not encode.
  //
  // kFeatOutputScale: MsgType::OutputSetScale is understood. Without the bit
  // the peer must assume it is not.
  //
  // kFeatInputText: MsgType::Input's Text opcode is typed here, in this
  // compositor's own layout (CBeamInputTranslator).
  uint32_t feat = kFeatDmabufFrames | kFeatBufferRelease | kFeatDamageRects |
                  kFeatOutputScale | kFeatInputText;
  // Only when this compositor was actually started with more than one seat.
  // A single-seat run must never claim it -- every seat would drive one pointer
  // (see webrtc_seats.h).
  if (KWin::WebRTCSeats::instance().enabled()) feat |= kFeatSeats;
  // Only claim the fence bit if this GL stack can actually make one. Unlike
  // the peer, we know at this point: the EGL display exists long before a
  // client connects.
  if (m_nativeFenceSupported) feat |= kFeatNativeFence;

  CapabilitiesPayload p{};
  p.protoVersion = kProtocolVersion;
  p.featureMask = feat;
  p.codecMask = kCodecMask;
  p.maxInflightBuffers = m_maxInflightBuffers;

  std::vector<uint8_t> buf(sizeof(MsgHeader) + sizeof(p));
  auto *hdr = reinterpret_cast<MsgHeader *>(buf.data());
  *hdr = MsgHeader{static_cast<uint8_t>(MsgType::Capabilities), 0, 0, 0};
  std::memcpy(buf.data() + sizeof(MsgHeader), &p, sizeof(p));

  sendToClient("Capabilities", buf.data(), buf.size(), SendPolicy::Blocking);
}

bool CBeamBridge::sendDmabufFrame(uint16_t outputIdx, const DmabufFramePayload &payload,
                                  uint8_t flags, const int *fds, int fdCount) {
  if (fdCount < 0 || fdCount > kMaxFdsPerMessage) return false;

  // Before the frame, not after: if the peer has caught up, the cursor it is
  // about to draw over this frame should already be the right one.
  replayCursorIfDirty();

  uint8_t buf[sizeof(MsgHeader) + sizeof(DmabufFramePayload)];
  auto *hdr = reinterpret_cast<MsgHeader *>(buf);
  *hdr = MsgHeader{static_cast<uint8_t>(MsgType::FrameDmabuf), flags, outputIdx, 0};
  std::memcpy(buf + sizeof(MsgHeader), &payload, sizeof(payload));

  return sendToClient("FrameDmabuf", buf, sizeof(buf), SendPolicy::DropOnFull, fds, fdCount);
}

void CBeamBridge::sendBufferDrop(uint16_t outputIdx, uint32_t bufferId) {
  BufferDropPayload p{bufferId};
  uint8_t buf[sizeof(MsgHeader) + sizeof(p)];
  auto *hdr = reinterpret_cast<MsgHeader *>(buf);
  *hdr = MsgHeader{static_cast<uint8_t>(MsgType::BufferDrop), 0, outputIdx, 0};
  std::memcpy(buf + sizeof(MsgHeader), &p, sizeof(p));

  sendToClient("BufferDrop", buf, sizeof(buf), SendPolicy::Blocking);
}

/**
 * Re-sends the cached cursor after a DropOnFull send lost it.
 *
 * Clearing the flag BEFORE the replay, not after: a cursor update racing this
 * call must be able to set it again and get its own replay, and re-sending
 * state that is already correct is free. The other order would swallow it.
 *
 * The replay is DropOnFull too, so a peer that is still wedged simply marks
 * the flag again and this stays cheap -- one relaxed load per frame while
 * everything is healthy, which it almost always is.
 */
void CBeamBridge::replayCursorIfDirty() {
  if (!m_cursorDirty.load(std::memory_order_relaxed)) return;
  m_cursorDirty.store(false, std::memory_order_relaxed);
  replayCursorCache();
}

// Brings a freshly (re)connected client up to date on cursor state that
// only ever gets pushed on real change (see the CachedCursor doc comment)
// -- same rationale as sendHello() replaying the current output list.
void CBeamBridge::replayCursorCache() {
  std::lock_guard<std::mutex> lk(m_cursorMutex);
  for (const auto &[outputIdx, c] : m_cursorCache) {
    if (c.haveImage) {
      sendCursorImageMsg(outputIdx, c.width, c.height, c.imgHotspotX, c.imgHotspotY,
                         c.rgba.data(), c.rgba.size());
    }
    if (c.havePos) {
      sendCursorPosMsg(outputIdx, c.x, c.y, c.hotspotX, c.hotspotY, c.visible);
    }
  }
}

void CBeamBridge::sendCursorPosMsg(uint16_t outputIdx, int32_t x, int32_t y,
                                    int32_t hotspotX, int32_t hotspotY, bool visible) {
  std::vector<uint8_t> buf(sizeof(MsgHeader) + sizeof(CursorPosPayload));
  auto *hdr = reinterpret_cast<MsgHeader *>(buf.data());
  *hdr = MsgHeader{static_cast<uint8_t>(MsgType::CursorPos), 0, outputIdx, 0};
  auto *p = reinterpret_cast<CursorPosPayload *>(buf.data() + sizeof(MsgHeader));
  p->x = x;
  p->y = y;
  p->hotspotX = hotspotX;
  p->hotspotY = hotspotY;
  p->visible = visible ? 1 : 0;

  if (!sendToClient("CursorPos", buf.data(), buf.size(), SendPolicy::DropOnFull))
    m_cursorDirty.store(true, std::memory_order_relaxed);
}

void CBeamBridge::sendCursorImageMsg(uint16_t outputIdx, uint32_t width, uint32_t height,
                                      int32_t hotspotX, int32_t hotspotY, const uint8_t *rgba,
                                      size_t rgbaLen) {
  std::vector<uint8_t> buf(sizeof(MsgHeader) + sizeof(CursorImagePayload) + rgbaLen);
  auto *hdr = reinterpret_cast<MsgHeader *>(buf.data());
  *hdr = MsgHeader{static_cast<uint8_t>(MsgType::CursorImage), 0, outputIdx, 0};
  auto *p = reinterpret_cast<CursorImagePayload *>(buf.data() + sizeof(MsgHeader));
  p->width = width;
  p->height = height;
  p->hotspotX = hotspotX;
  p->hotspotY = hotspotY;
  if (rgbaLen > 0) {
    std::memcpy(buf.data() + sizeof(MsgHeader) + sizeof(CursorImagePayload), rgba, rgbaLen);
  }

  if (!sendToClient("CursorImage", buf.data(), buf.size(), SendPolicy::DropOnFull))
    m_cursorDirty.store(true, std::memory_order_relaxed);
}

void CBeamBridge::setOutputs(const std::vector<OutputDescriptor> &outputs) {
  // Single hold of m_outputsMutex for both the store and the broadcast, so
  // no interleaving sender can observe (and serialize) a half-updated list.
  std::lock_guard<std::mutex> lk(m_outputsMutex);
  m_outputs = outputs;
  // No client test here: sendToClient makes that decision under the send lock,
  // where the answer is still true when it acts on it.
  sendOutputListLocked(static_cast<uint8_t>(MsgType::OutputsChanged));
}

void CBeamBridge::sendCursorPos(uint16_t outputIdx, int32_t x, int32_t y, int32_t hotspotX,
                                  int32_t hotspotY, bool visible) {
  {
    std::lock_guard<std::mutex> lk(m_cursorMutex);
    CachedCursor &c = m_cursorCache[outputIdx];
    c.havePos = true;
    c.x = x;
    c.y = y;
    c.hotspotX = hotspotX;
    c.hotspotY = hotspotY;
    c.visible = visible;
  }

  sendCursorPosMsg(outputIdx, x, y, hotspotX, hotspotY, visible);
}

void CBeamBridge::sendCursorImage(uint16_t outputIdx, uint32_t width, uint32_t height,
                                    int32_t hotspotX, int32_t hotspotY, const uint8_t *rgba,
                                    size_t rgbaLen) {
  if (rgbaLen > kMaxCursorImageBytes || rgbaLen != static_cast<size_t>(width) * height * 4) {
    WEBRTC_LOG("CBeamBridge: sendCursorImage rejected (w=%u h=%u len=%zu)", width, height,
               rgbaLen);
    return;
  }

  {
    std::lock_guard<std::mutex> lk(m_cursorMutex);
    CachedCursor &c = m_cursorCache[outputIdx];
    c.haveImage = true;
    c.width = width;
    c.height = height;
    c.imgHotspotX = hotspotX;
    c.imgHotspotY = hotspotY;
    c.rgba.assign(rgba, rgba + rgbaLen);
  }

  sendCursorImageMsg(outputIdx, width, height, hotspotX, hotspotY, rgba, rgbaLen);
}

// recvmsg, not recv: BufferRelease can carry a release sync_file in an
// SCM_RIGHTS control message, and recv() would leave it undelivered (leaking
// the descriptor on the sender's side, since SCM_RIGHTS only transfers when
// the receiver provides a control buffer).
void CBeamBridge::recvLoop(int fd) {
  std::vector<uint8_t> buf(64 * 1024);
  union {
    char control[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)];
    struct cmsghdr align;
  } cmsgbuf;

  while (m_running.load()) {
    struct iovec iov{buf.data(), buf.size()};
    struct msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsgbuf.control;
    mh.msg_controllen = sizeof(cmsgbuf.control);

    ssize_t n = ::recvmsg(fd, &mh, 0);
    if (n <= 0) break;

    int fds[kMaxFdsPerMessage];
    int nfds = 0;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
      if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
      const int count = static_cast<int>((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < count && nfds < kMaxFdsPerMessage; ++i) {
        int got = -1;
        std::memcpy(&got, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
        fds[nfds++] = got;
      }
    }
    // MSG_TRUNC as well as MSG_CTRUNC: a datagram larger than this buffer
    // arrives cut short, and parsing what is left as if it were whole reads
    // payload fields out of the tail of a previous message.
    if (mh.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) {
      for (int i = 0; i < nfds; ++i) ::close(fds[i]);
      static int nlog = 0;
      if (nlog++ < 8) {
        WEBRTC_ERR("CBeamBridge: truncated datagram dropped (flags 0x%x, %zd bytes)",
                   static_cast<unsigned>(mh.msg_flags), n);
      }
      continue;
    }

    const int consumed = handleMessage(buf.data(), static_cast<size_t>(n), fds, nfds);
    for (int i = consumed; i < nfds; ++i) ::close(fds[i]);
  }

  // Same reasoning as closeClient(): claim the fd atomically or leave it
  // alone (a load-then-store here would race acceptLoop's own close of the
  // same number), and close it under m_sendMutex so no send is between its load
  // and its sendmsg on this number.
  {
    std::lock_guard<std::mutex> slk(m_sendMutex);
    int expected = fd;
    if (m_clientFd.compare_exchange_strong(expected, -1)) {
      ::close(fd);
    }
  }
  if (m_onClientDisconnected) m_onClientDisconnected();
  WEBRTC_LOG("CBeamBridge: client disconnected (fd=%d)", fd);
}

int CBeamBridge::handleMessage(const uint8_t *data, size_t len, int *fds, int fdCount) {
  if (len < sizeof(MsgHeader)) return 0;
  const auto *hdr = reinterpret_cast<const MsgHeader *>(data);
  const uint8_t *payload = data + sizeof(MsgHeader);
  const size_t payloadLen = len - sizeof(MsgHeader);

  switch (static_cast<MsgType>(hdr->type)) {
    case MsgType::StreamStart: {
      // Checked against the V1 size, not sizeof(): `mode` is an appended v2
      // field and a v1 peer's payload legitimately stops before it. Getting
      // this wrong would make every v1 peer's StreamStart silently vanish.
      if (payloadLen < kStreamStartPayloadV1Size) return 0;
      const auto *p = reinterpret_cast<const StreamStartPayload *>(payload);
      const uint32_t mode =
          payloadLen >= sizeof(StreamStartPayload) ? p->mode : kStreamModeBitstream;
      if (m_onStreamStart)
        m_onStreamStart(hdr->outputIdx, p->codec, p->fps, p->startKbps, p->maxKbps, mode);
      break;
    }
    case MsgType::ClientHello: {
      if (payloadLen < sizeof(ClientHelloPayload)) return 0;
      const auto *p = reinterpret_cast<const ClientHelloPayload *>(payload);
      m_peerFeatures.store(p->featureMask);
      WEBRTC_LOG("CBeamBridge: client proto=%u features=0x%x", (unsigned)p->protoVersion,
                 p->featureMask);
      break;
    }
    case MsgType::BufferRelease: {
      if (payloadLen < sizeof(BufferReleasePayload)) return 0;
      const auto *p = reinterpret_cast<const BufferReleasePayload *>(payload);
      const bool hasFence = (hdr->flags & kReleaseHasFence) != 0 && fdCount >= 1;
      const int fenceFd = hasFence ? fds[0] : -1;
      if (m_onBufferRelease) {
        // Ownership of fenceFd moves into the callback (which hands it to
        // EglSwapchain::release). If nobody is listening we still have to
        // close it -- a release that finds no loan is normal during teardown.
        m_onBufferRelease(hdr->outputIdx, p->bufferId, p->frameId, fenceFd);
      } else if (fenceFd >= 0) {
        ::close(fenceFd);
      }
      return hasFence ? 1 : 0;
    }
    case MsgType::BufferDrop: {
      if (payloadLen < sizeof(BufferDropPayload)) return 0;
      const auto *p = reinterpret_cast<const BufferDropPayload *>(payload);
      if (m_onBufferForget) m_onBufferForget(hdr->outputIdx, p->bufferId);
      break;
    }
    case MsgType::StreamStop:
      if (m_onStreamStop) m_onStreamStop(hdr->outputIdx);
      break;
    // ForceIdr, SetBitrate and ResubmitLast are parsed and DROPPED. The
    // message types stay in the protocol enum, so existing values are never
    // reused, but this process has nothing to do with them.
    case MsgType::ForceIdr:
    case MsgType::SetBitrate:
    case MsgType::ResubmitLast:
      break;
    case MsgType::SetFps: {
      if (payloadLen < sizeof(SetFpsPayload)) return 0;
      const auto *p = reinterpret_cast<const SetFpsPayload *>(payload);
      if (m_onSetFps) m_onSetFps(hdr->outputIdx, p->fps);
      break;
    }
    case MsgType::Input:
      if (m_onInput) m_onInput(hdr->outputIdx, payload, payloadLen);
      break;
    case MsgType::OutputAdd: {
      if (payloadLen < sizeof(OutputAddPayload)) return 0;
      const auto *p = reinterpret_cast<const OutputAddPayload *>(payload);
      if (m_onOutputAdd) m_onOutputAdd(p->width, p->height, p->x, p->y, p->fps);
      break;
    }
    case MsgType::OutputRemove:
      if (m_onOutputRemove) m_onOutputRemove(hdr->outputIdx);
      break;
    case MsgType::OutputConfigure: {
      if (payloadLen < sizeof(OutputConfigurePayload)) return 0;
      const auto *p = reinterpret_cast<const OutputConfigurePayload *>(payload);
      if (m_onOutputConfigure)
        m_onOutputConfigure(hdr->outputIdx, p->width, p->height, p->hasPos != 0, p->x, p->y,
                             p->fps);
      break;
    }
    case MsgType::OutputSetScale: {
      if (payloadLen < sizeof(OutputScalePayload)) return 0;
      const auto *p = reinterpret_cast<const OutputScalePayload *>(payload);
      // Bounds are enforced here, not at the call site: this is the trust
      // boundary, and a scale of 0 (or 40) would leave the output in a state
      // no UI can get it out of.
      if (p->scaleMilli < kMinScaleMilli || p->scaleMilli > kMaxScaleMilli) {
        WEBRTC_LOG("CBeamBridge: OutputSetScale %u/1000 out of range, ignored",
                   (unsigned)p->scaleMilli);
        break;
      }
      if (m_onOutputSetScale) m_onOutputSetScale(hdr->outputIdx, p->scaleMilli / 1000.0);
      break;
    }
    default:
      break;
  }
  (void)fds;
  (void)fdCount;
  return 0;
}

}  // namespace NStreaming
