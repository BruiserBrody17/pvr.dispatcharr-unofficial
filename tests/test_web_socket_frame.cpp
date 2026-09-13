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
