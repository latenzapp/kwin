#pragma once

/*
 * CBeamInputTranslator — decodes the remote input wire format (as
 * forwarded verbatim by the peer over CBeamBridge's Input message) into
 * SRemoteInputEvent, and hands the result to
 * WebRTCInputBackend::queueEvents().
 *
 * This is the only place that knows the input wire format (input_protocol.h);
 * the KWin-facing half of the input backend deals in decoded events.
 *
 * SPDX-FileCopyrightText: 2026 Simone Salgaro
 *
 * Written for Latenz (Jul-Oct 2026) as part of the Latenz fork of KWin; not
 * part of upstream KWin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <cstddef>
#include <cstdint>

namespace KWin {
class WebRTCInputBackend;
}

namespace NStreaming {

class CBeamInputTranslator {
 public:
  // Parses one input_proto message (as defined in input_protocol.h)
  // and forwards the translated event(s) to `inputBackend`. Other opcodes are
  // ignored.
  //
  // `outputIdx` is the bridge header's -- the output the event came from, and
  // therefore its SEAT (see webrtc_seats.h). It is the DEFAULT: the opcodes
  // that carry an output of their own (MoveAbs, Button, TouchDown/TouchMove,
  // TabletTool) win, because that one is the surface the user actually
  // pointed at. The rest -- Key, MoveRel, Wheel, TouchUp, Text -- carry no
  // output and so take the header's.
  static void translate(uint16_t outputIdx, const uint8_t *data, size_t len,
                        KWin::WebRTCInputBackend *inputBackend);
};

}  // namespace NStreaming
