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

// The RFC 6455 decode counterpart to BuildMaskedControlFrame() above --
// WebSocketClient::ReceiveTextMessage()'s frame-header parsing, pulled out
// so the FIN/opcode/MASK-bit/7-bit-length-field bit-twiddling (and the
// extended 16-/64-bit length decode below) is unit-testable standalone
// without a real socket; see ../tests/test_web_socket_frame.cpp.
struct WebSocketFrameHeader
{
  bool fin = false;
  uint8_t opcode = 0;
  bool masked = false;
  // The raw 7-bit length field, 0-127. A value of 126 or 127 is RFC 6455's
  // own sentinel for "the real length follows as 2 (126) or 8 (127) more
  // bytes" -- decode those separately via DecodeExtendedPayloadLength16()/
  // DecodeExtendedPayloadLength64() below, matching how ReceiveTextMessage()
  // has to read them off the wire one step at a time anyway.
  uint64_t payloadLength7Bit = 0;
};

WebSocketFrameHeader ParseFrameHeaderBytes(const uint8_t header[2]);

uint64_t DecodeExtendedPayloadLength16(const uint8_t ext[2]);

uint64_t DecodeExtendedPayloadLength64(const uint8_t ext[8]);

// In-place XOR-unmask, per RFC 6455's masking algorithm -- the same
// operation BuildMaskedControlFrame() above applies when masking a frame
// (XOR is its own inverse), exposed separately here since
// ReceiveTextMessage() unmasks an already-received payload rather than
// building one.
void UnmaskPayload(std::vector<uint8_t>& payload, const uint8_t maskKey[4]);

} // namespace dispatcharr
