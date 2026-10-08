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
// Whether a frame received FROM the server obeys the two RFC 6455 rules this client
// used to assume rather than check: a control frame (opcode 0x8-0xF) must carry at
// most 125 bytes of payload and must not be fragmented (FIN set), and a server must
// never mask what it sends (section 5.1 -- the client "MUST close the connection" on
// one). `payloadLength` is the decoded length, extended length included. Dispatcharr
// itself never breaks either, so only a misbehaving peer or proxy can hit this; the
// caller treats a violation as a protocol error and reconnects. It also keeps the pong
// the client builds in answer to a ping within BuildMaskedControlFrame()'s 16-bit
// length encoding (docs/CLOSED_ITEMS.md, "Lower-severity WebSocketClient.cpp gaps").
bool IsValidServerFrame(uint8_t opcode, bool fin, bool masked, uint64_t payloadLength);

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

// Which kind of message ReceiveTextMessage() is currently assembling
// across a run of fragmented frames (opcode 0x0 continuations) --
// kNone before any initiating frame has arrived, or once a message has
// completed/been dropped.
enum class WebSocketMessageKind
{
  kNone,
  kText,
  kBinary,
};

// What ReceiveTextMessage() should do with a just-received *data* frame
// (opcode 0x0/0x1/0x2 only -- control frames 0x8/0x9/0xA are handled by
// the caller directly before ever reaching this, since ping/pong/close
// need real socket I/O this pure function can't do).
enum class WebSocketDataFrameAction
{
  kAccumulate,            // append this frame's payload to the in-progress message; more to come
  kAccumulateAndComplete, // append; the message is now complete (FIN was set)
  kDrop,                  // a binary frame or one of its continuations -- not this server's own event payloads, skip
  kProtocolError,         // a new initiating frame (0x1/0x2) arrived while a previous message was still being assembled
};

// Decides the action above from a data frame's own opcode/FIN bit, given
// the message kind currently being assembled -- the pure dispatch core of
// WebSocketClient::ReceiveTextMessage()'s per-frame switch, previously
// buried inline. Pulled out after a project-wide review found two real
// bugs in that inline switch: a completely dead `if (fin && !assembling)
// continue; continue;` branch (both paths did the same thing), and --
// more seriously -- a non-FIN binary frame's own continuation frames
// (opcode 0x0, which RFC 6455 shares between a text message's
// continuations and a binary message's own -- the spec doesn't tag which
// message type a continuation belongs to; only the initiating frame's
// opcode does) fell into the same branch as a text continuation and got
// silently appended to the assembled message, returned as "text" once
// FIN arrived. Updates `kind` in place: set when starting a new message,
// reset to kNone once a message completes, is dropped, or hits a
// protocol error.
WebSocketDataFrameAction DecideWebSocketDataFrameAction(uint8_t opcode, bool fin, WebSocketMessageKind& kind);

} // namespace dispatcharr
