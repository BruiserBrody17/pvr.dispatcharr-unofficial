#include "WebSocketFrame.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("BuildMaskedControlFrame builds an exact PONG frame with a small payload", "[WebSocketFrame]")
{
  std::vector<uint8_t> payload = {1, 2, 3};
  uint8_t maskKey[4] = {0xAA, 0xBB, 0xCC, 0xDD};

  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x0A, payload, maskKey);

  std::vector<uint8_t> expected = {
      0x8A,                               // FIN + opcode PONG (0x0A)
      0x83,                               // MASK bit + length 3
      0xAA,     0xBB,     0xCC,     0xDD, // mask key
      1 ^ 0xAA, 2 ^ 0xBB, 3 ^ 0xCC,       // masked payload
  };
  CHECK(frame == expected);
}

TEST_CASE("BuildMaskedControlFrame with an empty payload matches WebSocketClient::SendClose()'s exact byte layout",
          "[WebSocketFrame]")
{
  uint8_t maskKey[4] = {0x01, 0x02, 0x03, 0x04};

  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x08, {}, maskKey);

  std::vector<uint8_t> expected = {
      0x88,                   // FIN + opcode CLOSE (0x08)
      0x80,                   // MASK bit + length 0
      0x01, 0x02, 0x03, 0x04, // mask key
  };
  CHECK(frame == expected);
}

TEST_CASE("BuildMaskedControlFrame always sets the MASK bit regardless of payload size", "[WebSocketFrame]")
{
  uint8_t maskKey[4] = {0, 0, 0, 0};

  CHECK((BuildMaskedControlFrame(0x01, {}, maskKey)[1] & 0x80) != 0);
  CHECK((BuildMaskedControlFrame(0x01, {1, 2, 3}, maskKey)[1] & 0x80) != 0);
}

TEST_CASE("BuildMaskedControlFrame uses the extended 2-byte length prefix for a payload over 125 bytes",
          "[WebSocketFrame]")
{
  std::vector<uint8_t> payload(200, 0x42);
  uint8_t maskKey[4] = {0, 0, 0, 0};

  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x0A, payload, maskKey);

  REQUIRE(frame.size() >= 4);
  CHECK(frame[1] == (0x80 | 126)); // MASK bit + the 126 extended-length marker
  CHECK(frame[2] == 0);            // high byte of 200
  CHECK(frame[3] == 200);          // low byte of 200
  // header(2) + extended length(2) + mask(4) + payload(200)
  CHECK(frame.size() == 2 + 2 + 4 + 200);
}

TEST_CASE("BuildMaskedControlFrame masks every payload byte, cycling the 4-byte key", "[WebSocketFrame]")
{
  std::vector<uint8_t> payload = {10, 20, 30, 40, 50, 60};
  uint8_t maskKey[4] = {1, 2, 3, 4};

  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x0A, payload, maskKey);

  // Payload starts right after the 2-byte header + 4-byte mask key.
  std::vector<uint8_t> maskedPayload(frame.begin() + 6, frame.end());
  std::vector<uint8_t> expected;
  for (size_t i = 0; i < payload.size(); ++i)
    expected.push_back(payload[i] ^ maskKey[i % 4]);
  CHECK(maskedPayload == expected);
}

TEST_CASE("BuildMaskedControlFrame's total size is header + mask + payload for a small payload", "[WebSocketFrame]")
{
  std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
  uint8_t maskKey[4] = {0, 0, 0, 0};

  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x0A, payload, maskKey);

  CHECK(frame.size() == 2 + 4 + payload.size());
}

TEST_CASE("ParseFrameHeaderBytes decodes FIN, opcode, MASK bit, and the 7-bit length field", "[WebSocketFrame]")
{
  uint8_t header[2] = {0x81, 0x05}; // FIN + opcode TEXT (0x1), unmasked, length 5
  WebSocketFrameHeader h = ParseFrameHeaderBytes(header);

  CHECK(h.fin == true);
  CHECK(h.opcode == 0x1);
  CHECK(h.masked == false);
  CHECK(h.payloadLength7Bit == 5);
}

TEST_CASE("ParseFrameHeaderBytes decodes a non-final, masked continuation frame", "[WebSocketFrame]")
{
  uint8_t header[2] = {0x00, 0xFA}; // no FIN, opcode continuation (0x0), masked, length 122
  WebSocketFrameHeader h = ParseFrameHeaderBytes(header);

  CHECK(h.fin == false);
  CHECK(h.opcode == 0x0);
  CHECK(h.masked == true);
  CHECK(h.payloadLength7Bit == 122);
}

TEST_CASE("ParseFrameHeaderBytes reports 126/127 as the raw length field, not a decoded length", "[WebSocketFrame]")
{
  // The caller (ReceiveTextMessage()) is the one that recognizes these as
  // "read more extension bytes" sentinels -- this function just reports
  // the field verbatim.
  uint8_t header126[2] = {0x82, 0xFE}; // opcode BINARY, masked, length field 126
  CHECK(ParseFrameHeaderBytes(header126).payloadLength7Bit == 126);

  uint8_t header127[2] = {0x82, 0xFF}; // length field 127
  CHECK(ParseFrameHeaderBytes(header127).payloadLength7Bit == 127);
}

TEST_CASE("ParseFrameHeaderBytes extracts the low nibble as opcode regardless of the high nibble's reserved bits",
          "[WebSocketFrame]")
{
  // The 3 reserved (RSV1-3) bits sit between FIN and opcode in the same
  // byte -- confirm they don't leak into the decoded opcode.
  uint8_t header[2] = {0xF8, 0x00}; // FIN + all 3 reserved bits set + opcode CLOSE (0x8)
  WebSocketFrameHeader h = ParseFrameHeaderBytes(header);

  CHECK(h.fin == true);
  CHECK(h.opcode == 0x8);
}

TEST_CASE("DecodeExtendedPayloadLength16 decodes a big-endian 16-bit length", "[WebSocketFrame]")
{
  uint8_t ext[2] = {0x01, 0x2C}; // 300
  CHECK(DecodeExtendedPayloadLength16(ext) == 300);
}

TEST_CASE("DecodeExtendedPayloadLength16 handles the maximum 16-bit value", "[WebSocketFrame]")
{
  uint8_t ext[2] = {0xFF, 0xFF};
  CHECK(DecodeExtendedPayloadLength16(ext) == 65535);
}

TEST_CASE("DecodeExtendedPayloadLength64 decodes a big-endian 64-bit length", "[WebSocketFrame]")
{
  uint8_t ext[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}; // 65536
  CHECK(DecodeExtendedPayloadLength64(ext) == 65536);
}

TEST_CASE("DecodeExtendedPayloadLength64 handles a value spanning every byte", "[WebSocketFrame]")
{
  uint8_t ext[8] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}; // 2^32
  CHECK(DecodeExtendedPayloadLength64(ext) == (uint64_t{1} << 32));
}

TEST_CASE("UnmaskPayload XORs every byte, cycling the 4-byte key", "[WebSocketFrame]")
{
  std::vector<uint8_t> payload = {10, 20, 30, 40, 50, 60};
  uint8_t maskKey[4] = {1, 2, 3, 4};
  std::vector<uint8_t> expected;
  for (size_t i = 0; i < payload.size(); ++i)
    expected.push_back(payload[i] ^ maskKey[i % 4]);

  UnmaskPayload(payload, maskKey);

  CHECK(payload == expected);
}

TEST_CASE("UnmaskPayload is its own inverse -- masking twice with the same key restores the original",
          "[WebSocketFrame]")
{
  std::vector<uint8_t> original = {1, 2, 3, 4, 5, 6, 7};
  std::vector<uint8_t> payload = original;
  uint8_t maskKey[4] = {0xDE, 0xAD, 0xBE, 0xEF};

  UnmaskPayload(payload, maskKey);
  CHECK(payload != original);
  UnmaskPayload(payload, maskKey);
  CHECK(payload == original);
}

TEST_CASE("UnmaskPayload handles an empty payload", "[WebSocketFrame]")
{
  std::vector<uint8_t> payload;
  uint8_t maskKey[4] = {1, 2, 3, 4};
  UnmaskPayload(payload, maskKey);
  CHECK(payload.empty());
}
