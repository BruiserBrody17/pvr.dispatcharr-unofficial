#include "WebSocketHandshake.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("IsWebSocketHandshakeAccepted accepts a real HTTP/1.1 101 upgrade response", "[WebSocketHandshake]")
{
  std::string headers = "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: abc123==";
  CHECK(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted accepts an HTTP/1.0 101 upgrade response", "[WebSocketHandshake]")
{
  std::string headers = "HTTP/1.0 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n";
  CHECK(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted accepts a bare ' 101 ' status line from a reordering intermediary",
          "[WebSocketHandshake]")
{
  std::string headers = "Some-Proxy-Prefix: x\r\n"
                        "Status: 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n";
  CHECK(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted is case-insensitive", "[WebSocketHandshake]")
{
  std::string headers = "HTTP/1.1 101 Switching Protocols\r\n"
                        "UPGRADE: WebSocket\r\n";
  CHECK(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted rejects a 200 OK (e.g. auth failure falling back to a plain response)",
          "[WebSocketHandshake]")
{
  std::string headers = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html\r\n";
  CHECK_FALSE(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted rejects a 401 Unauthorized", "[WebSocketHandshake]")
{
  std::string headers = "HTTP/1.1 401 Unauthorized\r\n"
                        "WWW-Authenticate: Bearer\r\n";
  CHECK_FALSE(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted rejects a 101 response missing the Upgrade header", "[WebSocketHandshake]")
{
  std::string headers = "HTTP/1.1 101 Switching Protocols\r\n"
                        "Connection: Upgrade\r\n";
  CHECK_FALSE(IsWebSocketHandshakeAccepted(headers));
}

TEST_CASE("IsWebSocketHandshakeAccepted rejects an empty response", "[WebSocketHandshake]")
{
  CHECK_FALSE(IsWebSocketHandshakeAccepted(""));
}
