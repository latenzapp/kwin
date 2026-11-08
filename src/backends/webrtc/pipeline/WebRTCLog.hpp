/*
    SPDX-FileCopyrightText: 2026 Simone Salgaro

    Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not
    part of upstream KWin.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/*
 * WebRTC Debug Logging Macros
 *
 * Compile-time control via KWIN_DEBUG_LOG (CMake option):
 *   KWIN_DEBUG_LOG=1  → WEBRTC_LOG and WEBRTC_TRACE are compiled in and
 *                        controlled at runtime by AQ_WEBRTC_DEBUG env var.
 *   KWIN_DEBUG_LOG=0  → WEBRTC_LOG and WEBRTC_TRACE are compiled out entirely;
 *                        only WEBRTC_ERR and WEBRTC_INFO remain.
 *
 * Runtime control via AQ_WEBRTC_DEBUG (only when compiled with KWIN_DEBUG_LOG):
 *   AQ_WEBRTC_DEBUG=1  → lifecycle / init / error diagnostics
 *   AQ_WEBRTC_DEBUG=2  → per-frame trace logging (very noisy)
 *   unset / 0          → silent (only errors via WEBRTC_ERR)
 *
 * These macros write to stderr with a "[WebRTC]" prefix so they appear
 * alongside KWin logs without needing a backend pointer.
 */

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <chrono>

namespace NStreaming {

inline int webrtcDebugLevel() {
    static int level = -1;
    if (level < 0) {
        const char* env = std::getenv("AQ_WEBRTC_DEBUG");
        level = (env && env[0]) ? std::atoi(env) : 0;
    }
    return level;
}

} // namespace NStreaming

#ifdef KWIN_DEBUG_LOG

// Level 1: lifecycle / init / error diagnostics
#define WEBRTC_LOG(fmt, ...)                                                    \
    do {                                                                        \
        if (NStreaming::webrtcDebugLevel() >= 1)                       \
            std::fprintf(stderr, "[WebRTC] " fmt "\n", ##__VA_ARGS__);          \
    } while (0)

// Level 2: per-frame / hot-path trace (very verbose)
#define WEBRTC_TRACE(fmt, ...)                                                  \
    do {                                                                        \
        if (NStreaming::webrtcDebugLevel() >= 2)                       \
            std::fprintf(stderr, "[WebRTC:trace] " fmt "\n", ##__VA_ARGS__);    \
    } while (0)

#else  // KWIN_DEBUG_LOG disabled — compile out all debug logging
#define WEBRTC_LOG(fmt, ...)   do {} while (0)
#define WEBRTC_TRACE(fmt, ...) do {} while (0)
#endif // KWIN_DEBUG_LOG

// Always printed (errors)
#define WEBRTC_ERR(fmt, ...)                                                    \
    std::fprintf(stderr, "[WebRTC:ERROR] " fmt "\n", ##__VA_ARGS__)

// Always printed, and NOT an error.
//
// For the handful of lines worth printing to the compositor's stderr at the
// default verbosity -- a peer arriving, a peer leaving. WEBRTC_LOG is compiled
// out without KWIN_DEBUG_LOG and gated on AQ_WEBRTC_DEBUG on top, and
// WEBRTC_ERR would put an ERROR tag on routine reconnects.
#define WEBRTC_INFO(fmt, ...)                                                   \
    std::fprintf(stderr, "[WebRTC] " fmt "\n", ##__VA_ARGS__)

