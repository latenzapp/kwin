# SPDX-FileCopyrightText: 2026 Simone Salgaro
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Written for Latenz (Feb-Oct 2026) as part of the Latenz fork of KWin; not part
# of upstream KWin.
#
# KWin WebRTC Backend — headless compositing, DMABUF export
#
# This backend composites to a GBM buffer and hands that buffer to the peer
# over an AF_UNIX socket (pipeline/CBeamBridge.cpp, protocol in
# kwin_bridge_protocol.h). It does not encode and links no GPU-vendor library.

set(PIPELINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/pipeline)

# ── BEAM_PROTO_DIR is the one hard requirement ─────────────────────────────
if(NOT BEAM_PROTO_DIR)
    message(WARNING "BEAM_PROTO_DIR not set (expected: src/backends/webrtc/protocol) -- the WebRTC backend is skipped, and kwin, which uses it, will not build")
    return()
endif()
if(NOT EXISTS "${BEAM_PROTO_DIR}/kwin_bridge_protocol.h")
    message(WARNING "BEAM_PROTO_DIR=${BEAM_PROTO_DIR} does not contain kwin_bridge_protocol.h -- the WebRTC backend is skipped, and kwin, which uses it, will not build")
    return()
endif()

target_include_directories(kwin PRIVATE ${BEAM_PROTO_DIR})

target_sources(kwin PRIVATE
    webrtc_backend.cpp
    webrtc_cursor_layer.cpp
    webrtc_egl_backend.cpp
    webrtc_egl_layer.cpp
    webrtc_bridge_handlers.cpp
    webrtc_input_backend.cpp
    webrtc_logging.cpp
    webrtc_output.cpp
    webrtc_seats.cpp

    ${PIPELINE_DIR}/CBeamBridge.cpp
    ${PIPELINE_DIR}/CBeamInputTranslator.cpp
)

target_include_directories(kwin PRIVATE ${PIPELINE_DIR})

target_compile_definitions(kwin PRIVATE KWIN_WEBRTC_BACKEND=1)

# Debug logging: when enabled, WEBRTC_LOG/WEBRTC_TRACE macros emit output
# controlled at runtime by AQ_WEBRTC_DEBUG env var.
# When disabled, only WEBRTC_ERR and WEBRTC_INFO are compiled in.
option(KWIN_DEBUG_LOG "Enable verbose debug logging in the WebRTC pipeline" ON)
if(KWIN_DEBUG_LOG)
    target_compile_definitions(kwin PRIVATE KWIN_DEBUG_LOG=1)
    message(STATUS "WebRTC backend: Debug logging ENABLED")
else()
    message(STATUS "WebRTC backend: Debug logging DISABLED (errors and info only)")
endif()

