#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dispatcharr
{

// SHA-1 (RFC 3174) of `data`, returned as its 20 raw bytes. Exists for one
// reason: validating a WebSocket server's Sec-WebSocket-Accept header, which
// RFC 6455 defines as base64(SHA-1(client key + a fixed GUID)) -- see
// WebSocketHandshake.h. SHA-1 is broken as a *collision-resistant* hash, which
// is irrelevant here: the handshake only uses it to prove the server read this
// connection's own key, not as a security boundary, and nothing else in this
// addon should reach for it.
//
// A small, dependency-free implementation rather than pulling in a crypto
// library for one 20-byte digest. Zero Kodi/curl dependency; checked against
// the RFC's and FIPS 180's published vectors in ../tests/test_sha1.cpp.
std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t length);
std::array<uint8_t, 20> Sha1(const std::string& data);

} // namespace dispatcharr
