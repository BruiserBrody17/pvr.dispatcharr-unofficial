#pragma once

#include <cstdint>
#include <vector>

namespace dispatcharr
{

// Builds an RFC 6455 masked WebSocket frame for a small control-frame
// payload (FIN bit always set -- this client never sends a fragmented
// frame). Used for WebSocketClient::SendPong()'s PONG reply (opcode
// 0x0A) and SendClose()'s CLOSE frame (opcode 0x08, empty payload).
//
// A pong payload is always tiny in practice (an echoed ping payload,
// itself capped at 125 bytes by the spec), so the extended
// (126-length-prefix) branch below is defensive, not something this
// client is expected to ever actually hit.
//
// Takes the mask key as an explicit parameter (a real client always
// generates it fresh per frame via RandomBytes(), per the spec's own
// masking requirement) rather than generating it internally, so this is
// unit-testable standalone with a known, fixed key; see
// ../tests/test_web_socket_frame.cpp.
std::vector<uint8_t> BuildMaskedControlFrame(uint8_t opcode, const std::vector<uint8_t>& payload,
                                             const uint8_t maskKey[4]);

} // namespace dispatcharr
