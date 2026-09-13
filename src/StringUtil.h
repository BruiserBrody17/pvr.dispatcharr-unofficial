#pragma once

#include <cstdint>
#include <string>

namespace dispatcharr
{

// Standard Base64 (RFC 4648), no line wrapping -- used for
// WebSocketClient's Sec-WebSocket-Key handshake header, pulled out here
// (and kept dependency-free) specifically so it's unit-testable
// standalone; see ../tests/test_string_util.cpp.
std::string Base64Encode(const uint8_t* data, size_t len);

// ASCII lowercase, used for case-insensitive HTTP header comparison.
std::string ToLower(std::string s);

} // namespace dispatcharr
