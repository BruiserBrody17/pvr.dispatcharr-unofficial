#pragma once

#include "Sha1.h"
#include "StringUtil.h"

#include <array>
#include <string>

namespace dispatcharr
{

// Pure decision core of WebSocketClient::Connect()'s handshake check:
// given the raw HTTP response headers Dispatcharr sent back for the
// WebSocket upgrade request (everything up to, but not including, the
// blank line that ends them), decides whether Dispatcharr actually
// accepted it. Requires both a "101" status line (accepting "HTTP/1.0"
// and "HTTP/1.1", and a bare " 101 " match for a non-conformant
// intermediary that reorders/prefixes the status line) and an
// "Upgrade: websocket" response header -- either alone isn't sufficient
// (e.g. a reverse proxy returning 101 for an unrelated protocol
// upgrade).
//
// Zero curl/socket dependency once headerText is already in hand --
// pulled out here specifically so it's unit-testable standalone; see
// ../tests/test_web_socket_handshake.cpp.
inline bool IsWebSocketHandshakeAccepted(const std::string& headerText)
{
  std::string headerLower = ToLower(headerText);
  bool got101 = headerLower.find(" 101 ") != std::string::npos || headerLower.rfind("http/1.1 101", 0) == 0 ||
                headerLower.rfind("http/1.0 101", 0) == 0;
  bool gotUpgrade = headerLower.find("upgrade: websocket") != std::string::npos;
  return got101 && gotUpgrade;
}

// What a conforming server must answer in Sec-WebSocket-Accept for the
// Sec-WebSocket-Key this client sent (RFC 6455 section 4.2.2): the base64 of
// the SHA-1 of the key with a fixed GUID appended.
inline std::string ComputeWebSocketAccept(const std::string& base64Key)
{
  static const std::string kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const std::array<uint8_t, 20> digest = Sha1(base64Key + kGuid);
  return Base64Encode(digest.data(), digest.size());
}

// The value of the response's Sec-WebSocket-Accept header (name matched
// case-insensitively, surrounding whitespace trimmed), or "" if it has none.
inline std::string FindWebSocketAcceptHeader(const std::string& headerText)
{
  size_t pos = 0;
  while (pos < headerText.size())
  {
    size_t eol = headerText.find('\n', pos);
    if (eol == std::string::npos)
      eol = headerText.size();
    std::string line = headerText.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t colon = line.find(':');
    if (colon == std::string::npos || ToLower(line.substr(0, colon)) != "sec-websocket-accept")
      continue;
    std::string value = line.substr(colon + 1);
    const char* ws = " \t\r";
    const size_t first = value.find_first_not_of(ws);
    if (first == std::string::npos)
      return "";
    const size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
  }
  return "";
}

// IsWebSocketHandshakeAccepted() plus RFC 6455's own requirement that a client
// check the server's Sec-WebSocket-Accept against the key it sent (closed
// 2026-09-30, docs/OPEN_ITEMS.md): a 101 and an Upgrade header alone would
// also be sent by an intermediary that merely forwarded the request somewhere
// that never read this connection's key. The comparison is exact, because
// base64 is case-sensitive.
inline bool IsWebSocketHandshakeAcceptedForKey(const std::string& headerText, const std::string& base64Key)
{
  if (!IsWebSocketHandshakeAccepted(headerText))
    return false;
  const std::string accept = FindWebSocketAcceptHeader(headerText);
  return !accept.empty() && accept == ComputeWebSocketAccept(base64Key);
}

// The RFC 6455 handshake request itself, built by WebSocketClient::Connect()
// once its TCP/TLS CONNECT_ONLY connect has already succeeded. base64Key
// is taken as an explicit parameter (a real caller always generates a
// fresh 16-byte nonce via RandomBytes() + Base64Encode() first, per the
// spec's own Sec-WebSocket-Key requirement) rather than generated
// internally, so the exact byte output is testable against a known key
// -- the same convention BuildMaskedControlFrame() (WebSocketFrame.h)
// already uses for its own mask key. Pulled out specifically so it's
// unit-testable standalone without a real socket; see
// ../tests/test_web_socket_handshake.cpp.
//
// The peer's Sec-WebSocket-Accept answer to this key is checked by
// IsWebSocketHandshakeAcceptedForKey() above.
inline std::string BuildWebSocketHandshakeRequest(const std::string& pathAndQuery, const std::string& host, int port,
                                                  const std::string& base64Key)
{
  return "GET " + pathAndQuery +
         " HTTP/1.1\r\n"
         "Host: " +
         FormatHostForUrl(host) + ":" + std::to_string(port) +
         "\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Key: " +
         base64Key +
         "\r\n"
         "Sec-WebSocket-Version: 13\r\n"
         "\r\n";
}

} // namespace dispatcharr
