// WebSocketClient against a tiny local server, for what only real sockets can show: how long
// ReceiveTextMessage() can be kept busy by a peer that floods control frames or drips a frame
// in a byte at a time (docs/CLOSED_ITEMS.md, "Lower-severity WebSocketClient.cpp gaps"). POSIX only.

#include "WebSocketClient.h"

#include "WebSocketHandshake.h"

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace dispatcharr;
using Clock = std::chrono::steady_clock;

namespace
{
std::vector<unsigned char> Frame(unsigned char opcode, const std::string& payload, bool fin = true)
{
  std::vector<unsigned char> f;
  f.push_back(static_cast<unsigned char>((fin ? 0x80 : 0x00) | opcode));
  f.push_back(static_cast<unsigned char>(payload.size())); // < 126 in every test below
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

// A frame with the 126 (16-bit) or 127 (64-bit) extended length when `payload` is long, or
// when `force64` asks for the 64-bit form regardless, and optionally the MASK bit with a key
// (a server never masks; that is what makes it a violation).
std::vector<unsigned char> FrameExt(unsigned char opcode, const std::string& payload, bool fin = true,
                                    bool force64 = false, bool masked = false)
{
  std::vector<unsigned char> f;
  f.push_back(static_cast<unsigned char>((fin ? 0x80 : 0x00) | opcode));
  const unsigned char maskBit = masked ? 0x80 : 0x00;
  const size_t n = payload.size();
  if (n < 126 && !force64)
  {
    f.push_back(static_cast<unsigned char>(maskBit | n));
  }
  else if (n <= 0xFFFF && !force64)
  {
    f.push_back(static_cast<unsigned char>(maskBit | 126));
    f.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
    f.push_back(static_cast<unsigned char>(n & 0xFF));
  }
  else
  {
    f.push_back(static_cast<unsigned char>(maskBit | 127));
    for (int shift = 56; shift >= 0; shift -= 8)
      f.push_back(static_cast<unsigned char>((static_cast<uint64_t>(n) >> shift) & 0xFF));
  }
  if (masked)
  {
    const unsigned char key[4] = {0x11, 0x22, 0x33, 0x44};
    f.insert(f.end(), key, key + 4);
    for (size_t i = 0; i < n; ++i)
      f.push_back(static_cast<unsigned char>(static_cast<unsigned char>(payload[i]) ^ key[i % 4]));
  }
  else
  {
    f.insert(f.end(), payload.begin(), payload.end());
  }
  return f;
}

// Reads whatever the client sends back within ~1 s and returns the first byte, or -1.
int FirstByteFromClient(int fd)
{
  timeval tv{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  unsigned char b = 0;
  return recv(fd, &b, 1, 0) == 1 ? b : -1;
}

// Accepts one connection, completes the upgrade handshake correctly, then hands the connected
// socket to `scenario`. `stop` lets the scenario's loops end when the test is over.
class LocalWebSocketServer
{
public:
  // `replyFor`, when given, builds the whole handshake response from the client's
  // Sec-WebSocket-Key (a wrong Sec-WebSocket-Accept, a runaway header block); by default the
  // upgrade is answered correctly.
  explicit LocalWebSocketServer(std::function<void(int, const std::atomic<bool>&)> scenario,
                                std::function<std::string(const std::string&)> replyFor = nullptr)
  {
    m_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(m_listen, 1) == 0);
    socklen_t len = sizeof(addr);
    getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    m_thread = std::thread(
        [this, scenario, replyFor]()
        {
          int fd = accept(m_listen, nullptr, nullptr);
          if (fd < 0)
            return;
          std::string request;
          char buf[1024];
          while (request.find("\r\n\r\n") == std::string::npos)
          {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
              close(fd);
              return;
            }
            request.append(buf, static_cast<size_t>(n));
          }
          const std::string marker = "Sec-WebSocket-Key: ";
          size_t at = request.find(marker);
          std::string key = request.substr(at + marker.size(), request.find("\r\n", at) - at - marker.size());
          std::string reply = replyFor ? replyFor(key)
                                       : "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                         "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                             ComputeWebSocketAccept(key) + "\r\n\r\n";
          send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
          scenario(fd, m_stop);
          close(fd);
        });
  }
  ~LocalWebSocketServer()
  {
    m_stop = true;
    shutdown(m_listen, SHUT_RDWR);
    close(m_listen);
    if (m_thread.joinable())
      m_thread.join();
  }
  int port() const
  {
    return m_port;
  }

private:
  int m_listen = -1;
  int m_port = 0;
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
};

void SendBytes(int fd, const std::vector<unsigned char>& bytes)
{
  send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
}

double SecondsSince(Clock::time_point t)
{
  return std::chrono::duration<double>(Clock::now() - t).count();
}
} // namespace

TEST_CASE("WebSocketClient receives a text message from a local server", "[WebSocketClient]")
{
  LocalWebSocketServer server([](int fd, const std::atomic<bool>&) { SendBytes(fd, Frame(0x1, "hello")); });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == "hello");
}

TEST_CASE("WebSocketClient reports a quiet connection as a timeout, not an error", "[WebSocketClient]")
{
  LocalWebSocketServer server(
      [](int, const std::atomic<bool>& stop)
      {
        while (!stop)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  auto start = Clock::now();
  CHECK(client.ReceiveTextMessage(message, 1, error) == 0);
  CHECK(SecondsSince(start) < 2.5);
}

TEST_CASE("ReceiveTextMessage gives control back within its timeout even when the peer floods pings",
          "[WebSocketClient]")
{
  // The realtime thread has to return to notice a stop request; a continuous stream of control
  // frames used to keep this loop running for as long as the flood lasted.
  LocalWebSocketServer server(
      [](int fd, const std::atomic<bool>& stop)
      {
        auto until = Clock::now() + std::chrono::seconds(6);
        while (!stop && Clock::now() < until)
        {
          SendBytes(fd, Frame(0x9, "p"));
          std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  auto start = Clock::now();
  CHECK(client.ReceiveTextMessage(message, 1, error) == 0);
  CHECK(SecondsSince(start) < 2.5);
}

TEST_CASE("a frame dripped in a byte at a time fails within the timeout, not after a payload's worth of timeouts",
          "[WebSocketClient]")
{
  LocalWebSocketServer server(
      [](int fd, const std::atomic<bool>& stop)
      {
        // Header says 60 payload bytes, then one byte every 300 ms: 18 s to finish.
        SendBytes(fd, {0x81, 60});
        for (int i = 0; i < 60 && !stop; ++i)
        {
          SendBytes(fd, {'x'});
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  auto start = Clock::now();
  CHECK(client.ReceiveTextMessage(message, 1, error) == -1);
  CHECK(SecondsSince(start) < 3.0);
  CHECK(error.find("mid-frame") != std::string::npos);
}
TEST_CASE("a text message longer than 125 bytes arrives intact over the 16-bit length form", "[WebSocketClient]")
{
  // Every realtime message over 125 bytes takes this path in production, and until now no test
  // drove it through the client (the 2026-10-04 hardening sweep's missing-tests list).
  const std::string payload(300, 'a');
  LocalWebSocketServer server([&payload](int fd, const std::atomic<bool>&) { SendBytes(fd, FrameExt(0x1, payload)); });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == payload);
}

TEST_CASE("a text message arrives intact over the 64-bit length form", "[WebSocketClient]")
{
  std::string payload;
  for (int i = 0; i < 70000; ++i)
    payload.push_back(static_cast<char>('a' + i % 26));
  LocalWebSocketServer server([&payload](int fd, const std::atomic<bool>&)
                              { SendBytes(fd, FrameExt(0x1, payload, true, /*force64=*/true)); });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 5, error) == 1);
  CHECK(message == payload);
}

TEST_CASE("a masked frame from the server is refused as an RFC 6455 violation", "[WebSocketClient]")
{
  LocalWebSocketServer server([](int fd, const std::atomic<bool>&)
                              { SendBytes(fd, FrameExt(0x1, "hello", true, false, /*masked=*/true)); });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == -1);
  CHECK(error.find("RFC 6455") != std::string::npos);
  CHECK(message.empty());
}

TEST_CASE("a control frame over 125 bytes is refused as an RFC 6455 violation", "[WebSocketClient]")
{
  const std::string payload(200, 'p');
  LocalWebSocketServer server([&payload](int fd, const std::atomic<bool>&) { SendBytes(fd, FrameExt(0x9, payload)); });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == -1);
  CHECK(error.find("RFC 6455") != std::string::npos);
}

TEST_CASE("a text message split across a continuation frame is delivered as one message", "[WebSocketClient]")
{
  LocalWebSocketServer server(
      [](int fd, const std::atomic<bool>&)
      {
        SendBytes(fd, Frame(0x1, "hel", /*fin=*/false));
        SendBytes(fd, Frame(0x0, "lo", /*fin=*/true));
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == "hello");
}

TEST_CASE("a ping is answered with a masked pong and the next message still arrives", "[WebSocketClient]")
{
  std::atomic<int> firstByteBack{-2};
  LocalWebSocketServer server(
      [&firstByteBack](int fd, const std::atomic<bool>&)
      {
        SendBytes(fd, Frame(0x9, "ab"));
        firstByteBack = FirstByteFromClient(fd);
        SendBytes(fd, Frame(0x1, "after"));
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == "after");
  CHECK(firstByteBack == 0x8A); // FIN + pong
}

TEST_CASE("a close frame ends the receive with an error and is answered with a close", "[WebSocketClient]")
{
  std::atomic<int> firstByteBack{-2};
  std::string error;
  std::string message;
  {
    LocalWebSocketServer server(
        [&firstByteBack](int fd, const std::atomic<bool>&)
        {
          SendBytes(fd, Frame(0x8, ""));
          firstByteBack = FirstByteFromClient(fd);
        });
    WebSocketClient client;
    REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));
    CHECK(client.ReceiveTextMessage(message, 3, error) == -1);
    // The server thread is joined when `server` goes out of scope, so what it read back is
    // settled before it is checked below.
  }
  CHECK(error.find("closed by peer") != std::string::npos);
  CHECK(firstByteBack == 0x88); // FIN + close
}

TEST_CASE("a handshake answered with the wrong Sec-WebSocket-Accept is refused", "[WebSocketClient]")
{
  LocalWebSocketServer server([](int, const std::atomic<bool>&) {},
                              [](const std::string&)
                              {
                                return std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                   "Connection: Upgrade\r\n"
                                                   "Sec-WebSocket-Accept: bm90IHRoZSByaWdodCBrZXk=\r\n\r\n");
                              });
  WebSocketClient client;
  std::string error;
  CHECK_FALSE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));
  CHECK(error.find("Sec-WebSocket-Accept") != std::string::npos);
}

TEST_CASE("a handshake response whose headers never end is cut off at the size limit", "[WebSocketClient]")
{
  LocalWebSocketServer server([](int, const std::atomic<bool>&) {},
                              [](const std::string&)
                              {
                                // 20 KiB of header lines and no blank line: the client must stop reading at its cap
                                // instead of waiting for a terminator that never comes.
                                std::string reply = "HTTP/1.1 101 Switching Protocols\r\n";
                                while (reply.size() < 20 * 1024)
                                  reply +=
                                      "X-Padding: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
                                return reply;
                              });
  WebSocketClient client;
  std::string error;
  const auto start = Clock::now();
  CHECK_FALSE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));
  CHECK(error.find("size limit") != std::string::npos);
  CHECK(SecondsSince(start) < 4.0);
}
TEST_CASE("a stop request ends a handshake the server never answers, instead of waiting out the timeout",
          "[WebSocketClient]")
{
  // Measured live 2026-10-04: with the server accepting the connection and never answering the
  // upgrade, Kodi took a whole connect timeout (30 s at the default) to exit, because the realtime
  // thread sat in this wait and nothing could interrupt it.
  LocalWebSocketServer server(
      [](int, const std::atomic<bool>& stop)
      {
        while (!stop)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      },
      [](const std::string&)
      {
        return std::string(); /* no reply at all */
      });
  std::atomic<bool> stop{false};
  WebSocketClient client;
  client.SetStopCheck([&stop]() { return stop.load(); });
  std::thread stopper(
      [&stop]()
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop = true;
      });
  std::string error;
  const auto start = Clock::now();
  CHECK_FALSE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 30, error));
  const double elapsed = SecondsSince(start);
  stopper.join();
  CHECK(elapsed < 3.0);
  CHECK(error.find("shutting down") != std::string::npos);
}

TEST_CASE("a stop request ends a TLS handshake the server never answers", "[WebSocketClient]")
{
  // The listener accepts and then never speaks TLS. The progress callback is what lets a stop end
  // the TLS handshake inside curl; without it the whole connect timeout was waited out.
  LocalWebSocketServer server([](int, const std::atomic<bool>&) {});
  std::atomic<bool> stop{false};
  WebSocketClient client;
  client.SetStopCheck([&stop]() { return stop.load(); });
  std::thread stopper(
      [&stop]()
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop = true;
      });
  std::string error;
  const auto start = Clock::now();
  CHECK_FALSE(client.Connect("127.0.0.1", server.port(), /*useTls=*/true, "/ws/", false, 10, error));
  const double elapsed = SecondsSince(start);
  stopper.join();
  CHECK(elapsed < 3.0);
}

TEST_CASE("a stop request that is already set refuses to connect at all", "[WebSocketClient]")
{
  LocalWebSocketServer server([](int, const std::atomic<bool>&) {});
  WebSocketClient client;
  client.SetStopCheck([]() { return true; });
  std::string error;
  CHECK_FALSE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));
  CHECK(error.find("shutting down") != std::string::npos);
  CHECK_FALSE(client.IsConnected());
}

TEST_CASE("a stop request ends a quiet read within a fraction of its timeout", "[WebSocketClient]")
{
  LocalWebSocketServer server(
      [](int, const std::atomic<bool>& stop)
      {
        while (!stop)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      });
  std::atomic<bool> stop{false};
  WebSocketClient client;
  client.SetStopCheck([&stop]() { return stop.load(); });
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::thread stopper(
      [&stop]()
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop = true;
      });
  std::string message;
  const auto start = Clock::now();
  CHECK(client.ReceiveTextMessage(message, 30, error) == -1);
  const double elapsed = SecondsSince(start);
  stopper.join();
  CHECK(elapsed < 3.0);
  CHECK(error.find("shutting down") != std::string::npos);
}

TEST_CASE("without a stop check the waits behave exactly as before", "[WebSocketClient]")
{
  LocalWebSocketServer server(
      [](int fd, const std::atomic<bool>& stop)
      {
        SendBytes(fd, Frame(0x1, "hello"));
        while (!stop)
          std::this_thread::sleep_for(std::chrono::milliseconds(20)); // stays connected and quiet
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));
  std::string message;
  CHECK(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == "hello");
  // And a quiet connection still reports a timeout, not an error.
  CHECK(client.ReceiveTextMessage(message, 1, error) == 0);
}

TEST_CASE("a second message on the same connection does not carry the first one's text", "[WebSocketClient]")
{
  LocalWebSocketServer server(
      [](int fd, const std::atomic<bool>&)
      {
        SendBytes(fd, Frame(0x1, "a"));
        SendBytes(fd, Frame(0x1, "b"));
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  REQUIRE(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == "a");
  REQUIRE(client.ReceiveTextMessage(message, 3, error) == 1);
  CHECK(message == "b"); // the realtime thread reads many messages per connection
}

TEST_CASE("fragments that each fit but together exceed the message cap are refused", "[WebSocketClient]")
{
  // Two 6 MiB fragments: each is under the 10 MiB frame cap, the running total is not.
  const std::string chunk(6 * 1024 * 1024, 'x');
  LocalWebSocketServer server(
      [&chunk](int fd, const std::atomic<bool>&)
      {
        SendBytes(fd, FrameExt(0x1, chunk, /*fin=*/false, /*force64=*/true));
        SendBytes(fd, FrameExt(0x0, chunk, /*fin=*/false, /*force64=*/true));
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  CHECK(client.ReceiveTextMessage(message, 10, error) == -1);
  CHECK(error.find("sanity limit") != std::string::npos);
}

TEST_CASE("a back-to-back ping flood with no gaps still returns within the timeout", "[WebSocketClient]")
{
  // The existing flood test sleeps between pings, so a header read's own deadline ends it. With the
  // socket always readable only the between-frames deadline check can.
  LocalWebSocketServer server(
      [](int fd, const std::atomic<bool>& stop)
      {
        const auto ping = Frame(0x9, "p");
        auto until = Clock::now() + std::chrono::seconds(6);
        char sink[4096];
        while (!stop && Clock::now() < until)
        {
          SendBytes(fd, ping);
          recv(fd, sink, sizeof(sink), MSG_DONTWAIT); // keep the client's pongs from filling our buffer
        }
      });
  WebSocketClient client;
  std::string error;
  REQUIRE(client.Connect("127.0.0.1", server.port(), false, "/ws/", false, 5, error));

  std::string message;
  auto start = Clock::now();
  CHECK(client.ReceiveTextMessage(message, 1, error) == 0);
  CHECK(SecondsSince(start) < 2.5);
}

TEST_CASE("Close() forgets a half-received fragmented message", "[WebSocketClient]")
{
  WebSocketClient client;
  std::string error;
  std::string message;
  {
    LocalWebSocketServer first(
        [](int fd, const std::atomic<bool>& stop)
        {
          SendBytes(fd, Frame(0x1, "part", /*fin=*/false));
          while (!stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        });
    REQUIRE(client.Connect("127.0.0.1", first.port(), false, "/ws/", false, 5, error));
    CHECK(client.ReceiveTextMessage(message, 1, error) == 0); // the fragment is consumed, no end yet
    client.Close();
  }
  // A new connection that starts with a lone continuation frame is a protocol error: nothing is being
  // assembled, so it must not be taken as the rest of the old message.
  LocalWebSocketServer second([](int fd, const std::atomic<bool>&) { SendBytes(fd, Frame(0x0, "rest")); });
  REQUIRE(client.Connect("127.0.0.1", second.port(), false, "/ws/", false, 5, error));
  message.clear();
  const int rc = client.ReceiveTextMessage(message, 2, error);
  CHECK(message != "partrest");
  CHECK(rc != 1);
}

#endif
