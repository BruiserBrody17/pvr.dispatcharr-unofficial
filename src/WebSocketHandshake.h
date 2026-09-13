#pragma once

#include "StringUtil.h"

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

} // namespace dispatcharr
