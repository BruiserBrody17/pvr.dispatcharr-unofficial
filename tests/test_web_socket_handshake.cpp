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

// ---------------------------------------------------------------------
// BuildWebSocketHandshakeRequest
// ---------------------------------------------------------------------

TEST_CASE("BuildWebSocketHandshakeRequest matches the exact RFC 6455 request layout", "[WebSocketHandshake]")
{
  std::string request = BuildWebSocketHandshakeRequest("/ws/?token=abc", "dispatcharr.local", 9191, "dGhlIHNhbXBsZQ==");

  std::string expected = "GET /ws/?token=abc HTTP/1.1\r\n"
                         "Host: dispatcharr.local:9191\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "\r\n";
  CHECK(request == expected);
}

TEST_CASE("BuildWebSocketHandshakeRequest ends with a blank line (a bare CRLF) terminating the headers",
          "[WebSocketHandshake]")
{
  std::string request = BuildWebSocketHandshakeRequest("/ws/", "host", 80, "key==");
  CHECK(request.substr(request.size() - 4) == "\r\n\r\n");
}

TEST_CASE("BuildWebSocketHandshakeRequest always declares Sec-WebSocket-Version: 13", "[WebSocketHandshake]")
{
  std::string request = BuildWebSocketHandshakeRequest("/ws/", "host", 80, "key==");
  CHECK(request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
}

TEST_CASE("BuildWebSocketHandshakeRequest brackets an IPv6 host literal in its Host: header", "[WebSocketHandshake]")
{
  std::string request = BuildWebSocketHandshakeRequest("/ws/", "::1", 9191, "key==");
  CHECK(request.find("Host: [::1]:9191\r\n") != std::string::npos);
}

TEST_CASE("BuildWebSocketHandshakeRequest includes the port in the Host header", "[WebSocketHandshake]")
{
  std::string request = BuildWebSocketHandshakeRequest("/ws/", "host", 12345, "key==");
  CHECK(request.find("Host: host:12345\r\n") != std::string::npos);
}

// ---------------------------------------------------------------------
// Sec-WebSocket-Accept validation
// ---------------------------------------------------------------------

TEST_CASE("ComputeWebSocketAccept matches RFC 6455's own worked example", "[WebSocketHandshake]")
{
  CHECK(ComputeWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("FindWebSocketAcceptHeader reads the header case-insensitively and trims it", "[WebSocketHandshake]")
{
  CHECK(FindWebSocketAcceptHeader("HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: abc=\r\n") == "abc=");
  CHECK(FindWebSocketAcceptHeader("HTTP/1.1 101 x\r\nsec-websocket-accept:   abc=  \r\n") == "abc=");
  CHECK(FindWebSocketAcceptHeader("HTTP/1.1 101 x\r\nSEC-WEBSOCKET-ACCEPT:abc=") == "abc=");
  CHECK(FindWebSocketAcceptHeader("HTTP/1.1 101 x\r\nUpgrade: websocket\r\n").empty());
  CHECK(FindWebSocketAcceptHeader("").empty());
}

TEST_CASE("FindWebSocketAcceptHeader doesn't mistake a similar header or a value for it", "[WebSocketHandshake]")
{
  CHECK(FindWebSocketAcceptHeader("X-Sec-WebSocket-Accept: abc=\r\n").empty());
  CHECK(FindWebSocketAcceptHeader("Sec-WebSocket-Accept-Extra: abc=\r\n").empty());
  CHECK(FindWebSocketAcceptHeader("Server: Sec-WebSocket-Accept: abc=\r\n").empty());
}

TEST_CASE("IsWebSocketHandshakeAcceptedForKey accepts the server's correct answer", "[WebSocketHandshake]")
{
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::string headers = "HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
  CHECK(IsWebSocketHandshakeAcceptedForKey(headers, key));
}

TEST_CASE("IsWebSocketHandshakeAcceptedForKey rejects a wrong, missing or case-altered answer", "[WebSocketHandshake]")
{
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::string base = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(base + "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=", key));
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(base, key));
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(base + "Sec-WebSocket-Accept: ", key));
  // base64 is case-sensitive: the right letters in the wrong case are a wrong answer.
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(base + "Sec-WebSocket-Accept: S3PPLMBITXAQ9KYGZZHZRBK+XO=", key));
}

TEST_CASE("IsWebSocketHandshakeAcceptedForKey rejects an answer computed for a different key", "[WebSocketHandshake]")
{
  const std::string headers = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(headers, "AQIDBAUGBwgJCgsMDQ4PEA=="));
}

TEST_CASE("IsWebSocketHandshakeAcceptedForKey still needs the 101 and the Upgrade header", "[WebSocketHandshake]")
{
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(
      "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", key));
  CHECK_FALSE(IsWebSocketHandshakeAcceptedForKey(
      "HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", key));
}

TEST_CASE("FindWebSocketAcceptHeader trims a tab after the colon and mixed-case names", "[WebSocketHandshake]")
{
  CHECK(FindWebSocketAcceptHeader("HTTP/1.1 101 Switching Protocols\r\nsEc-WebSocket-AcCePt:\tabc=\r\n\r\n") == "abc=");
  CHECK(FindWebSocketAcceptHeader("Sec-WebSocket-Accept:\t \r\n") == "");
}
