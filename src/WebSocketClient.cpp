#include "WebSocketClient.h"

#include "SocketWait.h"
#include "StringUtil.h"
#include "WebSocketFrame.h"
#include "WebSocketHandshake.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace dispatcharr
{

namespace
{

constexpr std::chrono::milliseconds kWaitSlice{
    500}; // how long any single socket wait lasts before the stop check runs again
constexpr size_t kMaxFramePayload = 10 * 1024 * 1024; // sanity cap, not a real limit Dispatcharr hits

void RandomBytes(uint8_t* out, size_t len)
{
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 255);
  for (size_t i = 0; i < len; ++i)
    out[i] = static_cast<uint8_t>(dist(rng));
}

} // namespace

namespace
{
int StopProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  const auto* stop = static_cast<const std::function<bool()>*>(clientp);
  return (stop && *stop && (*stop)()) ? 1 : 0; // non-zero ends the transfer (CURLE_ABORTED_BY_CALLBACK)
}
} // namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient()
{
  Close();
}

void WebSocketClient::Close()
{
  if (m_curl)
  {
    curl_easy_cleanup(static_cast<CURL*>(m_curl));
    m_curl = nullptr;
  }
  m_recvBuffer.clear();
  m_recvPos = 0;
  m_assembledMessage.clear();
  m_messageKind = WebSocketMessageKind::kNone;
}

bool WebSocketClient::SendAll(const uint8_t* data, size_t len, int timeoutSeconds, std::string& error)
{
  CURL* curl = static_cast<CURL*>(m_curl);
  size_t sent = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  while (sent < len)
  {
    size_t n = 0;
    CURLcode res = curl_easy_send(curl, data + sent, len - sent, &n);
    if (res == CURLE_AGAIN)
    {
      if (Stopping())
      {
        error = "WebSocket send abandoned: shutting down";
        return false;
      }
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
      {
        error = "Timed out waiting for the WebSocket send buffer to drain";
        return false;
      }
      // Sliced so the stop check above runs about twice a second, not once per timeout.
      auto remainingMs = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), kWaitSlice);
      curl_socket_t sockfd = CURL_SOCKET_BAD;
      curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
      int rc = WaitForSocketReady(sockfd, /*forWrite=*/true, remainingMs);
      if (rc < 0)
      {
        error = "WebSocket socket wait failed while waiting to send";
        return false;
      }
      // rc == 0 (timeout) or rc > 0 (writable, or a spurious wakeup) both
      // just loop back to curl_easy_send -- the deadline check above is
      // what actually bounds this, not this select() call's own return
      // value, since a spurious wakeup shouldn't be treated as an error.
      continue;
    }
    if (res != CURLE_OK)
    {
      error = std::string("WebSocket send failed: ") + curl_easy_strerror(res);
      return false;
    }
    sent += n;
  }
  return true;
}

int WebSocketClient::FillBuffer(std::chrono::steady_clock::time_point deadline, std::string& error)
{
  CURL* curl = static_cast<CURL*>(m_curl);
  curl_socket_t sockfd = CURL_SOCKET_BAD;
  curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);

  uint8_t chunk[4096];
  for (;;)
  {
    size_t n = 0;
    CURLcode res = curl_easy_recv(curl, chunk, sizeof(chunk), &n);
    if (res == CURLE_OK)
    {
      if (n == 0)
      {
        error = "WebSocket connection closed by peer";
        return -1;
      }
      m_recvBuffer.insert(m_recvBuffer.end(), chunk, chunk + n);
      return 1;
    }
    if (res == CURLE_AGAIN)
    {
      if (Stopping())
      {
        error = "WebSocket read abandoned: shutting down";
        return -1;
      }
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return 0;
      // Sliced (see SendAll()): a slice that times out loops back through curl_easy_recv and
      // the deadline check above, which is what ends the wait for real.
      auto remainingMs = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), kWaitSlice);
      int rc = WaitForSocketReady(sockfd, /*forWrite=*/false, remainingMs);
      if (rc == 0)
        continue;
      if (rc < 0)
      {
        error = "WebSocket socket wait failed while waiting for data";
        return -1;
      }
      continue; // socket says readable; loop back to curl_easy_recv
    }
    error = std::string("WebSocket recv failed: ") + curl_easy_strerror(res);
    return -1;
  }
}

int WebSocketClient::ReadExact(uint8_t* out, size_t len, std::chrono::steady_clock::time_point deadline,
                               std::string& error)
{
  while (m_recvBuffer.size() - m_recvPos < len)
  {
    int r = FillBuffer(deadline, error);
    if (r <= 0)
      return r;
  }
  std::memcpy(out, m_recvBuffer.data() + m_recvPos, len);
  m_recvPos += len;
  // Compact once consumed data dominates the buffer, so a long-lived
  // connection doesn't grow this vector forever.
  if (m_recvPos > 0 && m_recvPos * 2 > m_recvBuffer.size())
  {
    m_recvBuffer.erase(m_recvBuffer.begin(), m_recvBuffer.begin() + static_cast<long>(m_recvPos));
    m_recvPos = 0;
  }
  return 1;
}

bool WebSocketClient::Connect(const std::string& host, int port, bool useTls, const std::string& pathAndQuery,
                              bool verifySsl, int connectTimeoutSeconds, std::string& error)
{
  Close();

  if (Stopping())
  {
    error = "WebSocket connect abandoned: shutting down";
    return false;
  }
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Failed to initialise libcurl";
    return false;
  }

  std::string scheme = useTls ? "https://" : "http://";
  // FormatHostForUrl() brackets a bare IPv6 literal (StringUtil.h) -- the
  // same fix already applied to every other host:port URL/Host header
  // this addon builds (DispatcharrClient::BaseUrl(),
  // BuildTimeshiftPlaylistUrl()/BuildTimeshiftSegmentBaseUrl(),
  // BuildWebSocketHandshakeRequest()'s own Host: header below) had missed
  // this one: curl can't parse "http://::1:9191/" back into an address
  // and a port.
  std::string url = scheme + FormatHostForUrl(host) + ":" + std::to_string(port) + "/";

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
  // Force HTTP/1.1 -- a real, confirmed gap found via a project-wide
  // review, not reproduced live (a 30th-pass audit, confirmed against
  // the real curl source this addon's own depends builds link against,
  // curl 8.6.0, both CoreELEC's and Kodi-for-Android's): a curl built
  // with nghttp2 (confirmed both do, USE_NGHTTP2 -> USE_HTTP2) defaults
  // `httpwant` to CURL_HTTP_VERSION_2TLS (url.c), and CURLOPT_CONNECT_ONLY
  // doesn't change ALPN's own offered protocol list (vtls.c's
  // alpn_get_spec() only looks at `httpwant`) -- so over TLS, this
  // handle's own TLS ClientHello offers "h2, http/1.1" via ALPN
  // regardless. If the server (a reverse proxy in front of Dispatcharr
  // -- Caddy/Traefik negotiate h2 by default, nginx does with "http2 on")
  // picks h2, curl installs its own HTTP/2 filter on this connection
  // unconditionally (cf-https-connect.c's baller_connected(), which
  // doesn't check connect-only mode either). Every later
  // curl_easy_send() of this handshake's raw HTTP/1.1 upgrade request
  // then goes through that filter's own cf_h2_send() -> h2_submit(),
  // which parses the raw bytes as an HTTP/1 request and re-encodes them
  // as an HTTP/2 HEADERS frame (http2.c) -- silently dropping the
  // Connection/Upgrade headers a WebSocket handshake depends on, since
  // neither is valid in HTTP/2. The server then never answers
  // "HTTP/1.1 101", IsWebSocketHandshakeAccepted() always fails, and
  // realtime updates can never connect at all behind such a proxy --
  // silently, at debug log level only, with no user-visible error and a
  // permanent fallback to the periodic refresh. Only relevant with
  // use_https on and only when it goes through such a proxy; nothing
  // else on this connection needs (or, per RFC 6455, is defined for)
  // anything beyond HTTP/1.1 anyway, so pinning it here is safe
  // regardless of what the server would have picked.
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(connectTimeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // see ApplyStandardCurlOptions()
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connectTimeoutSeconds));
  // TCP keepalive -- a real, confirmed-live gap this closes (found via a
  // project-wide review, matching an unexplained observation already
  // documented in docs/RECORDINGS.md: the realtime-update connection
  // appearing not to reconnect after a Dispatcharr outage-and-recovery,
  // only one "connected" log line the whole session). This client never
  // sends anything of its own accord (only answers the server's own
  // pings) and PVRDispatcharr's own read loop treats a plain read
  // timeout as "nothing new" indefinitely -- so a half-open connection
  // (the peer vanishes without ever sending a FIN/RST, e.g. an
  // application crash a reverse proxy or the OS's own TCP stack doesn't
  // notice, a NAT mapping silently expiring, a network path dropping)
  // would otherwise never be detected at all: recv() on a genuinely
  // half-open socket just keeps returning "no data yet", identical to a
  // healthy connection with nothing to report. Enabling the OS's own TCP
  // keepalive probing is what actually surfaces this as a real socket
  // error eventually, which PVRDispatcharr's own read loop already
  // treats as "reconnect".
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);

  // libcurl calls this about once a second even while a connect is stuck on an unresponsive
  // host, so a stop request ends the connect instead of waiting out CONNECTTIMEOUT.
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &StopProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_shouldStop);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK)
  {
    error = std::string("WebSocket TCP/TLS connect failed: ") + curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return false;
  }

  m_curl = curl;

  uint8_t nonce[16];
  RandomBytes(nonce, sizeof(nonce));
  std::string key = Base64Encode(nonce, sizeof(nonce));

  // The request assembly itself lives in
  // dispatcharr::BuildWebSocketHandshakeRequest() (WebSocketHandshake.h,
  // alongside IsWebSocketHandshakeAccepted()) so it's unit-testable
  // standalone -- see that function's own comment.
  std::string request = BuildWebSocketHandshakeRequest(pathAndQuery, host, port, key);

  if (!SendAll(reinterpret_cast<const uint8_t*>(request.data()), request.size(), connectTimeoutSeconds, error))
  {
    Close();
    return false;
  }

  // Read the HTTP response until the blank line that ends the headers.
  // Anything read past that point is the start of the WebSocket frame
  // stream and stays buffered (m_recvPos) for ReceiveTextMessage().
  //
  // Capped at kMaxHandshakeHeaderBytes (added 2026-09-27, a 58th-pass
  // audit, fixing a real, confirmed gap found via a project-wide review,
  // not itself independently reproduced): this loop previously had no
  // size bound at all -- a peer or proxy that trickles data without ever
  // sending the "\r\n\r\n" terminator (FillBuffer()'s own timeout only
  // fires when NOTHING arrives within the timeout window, not when data
  // keeps arriving too slowly to matter) would grow m_recvBuffer
  // without limit, and this loop's own headerText.assign() re-copies the
  // entire buffered content on every single iteration regardless,
  // quadratic in the number of iterations. A real WebSocket handshake
  // response's headers are always small (a few hundred bytes, at most a
  // few KB even with several Set-Cookie headers) -- 16 KiB is a generous
  // multiple of any real response, never legitimately reachable.
  constexpr size_t kMaxHandshakeHeaderBytes = 16384;
  // One overall deadline for the whole loop, not connectTimeoutSeconds
  // handed to FillBuffer() fresh on every iteration (fixed 2026-09-27, a
  // 59th-pass audit, fixing a real, confirmed gap the size cap above
  // didn't cover, found via a project-wide review, not itself
  // independently reproduced -- see docs/OPEN_ITEMS.md's own entry on
  // this loop for the full account): FillBuffer()'s own timeout only
  // fires when NOTHING arrives within its window, not when data keeps
  // trickling in too slowly to matter -- so a peer sending a byte or two
  // just under every connectTimeoutSeconds window, with the "\r\n\r\n"
  // terminator never actually sent, could keep this loop running for
  // roughly (kMaxHandshakeHeaderBytes / 1 byte per chunk) *
  // connectTimeoutSeconds before the size cap above ever triggers --
  // many days at this file's own default. Connect() runs on the
  // realtime-update thread, which StopWorkerThread() joins, so a stuck
  // handshake here could hang addon shutdown for that same duration.
  auto handshakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(connectTimeoutSeconds);
  std::string headerText;
  for (;;)
  {
    // Search what's already buffered before pulling more off the wire.
    const char* base = reinterpret_cast<const char*>(m_recvBuffer.data()) + m_recvPos;
    size_t available = m_recvBuffer.size() - m_recvPos;
    if (available > kMaxHandshakeHeaderBytes)
    {
      error = "WebSocket handshake response headers exceeded the size limit";
      Close();
      return false;
    }
    headerText.assign(base, available);
    size_t terminator = headerText.find("\r\n\r\n");
    if (terminator != std::string::npos)
    {
      m_recvPos += terminator + 4;
      headerText.resize(terminator);
      break;
    }
    if (std::chrono::steady_clock::now() >= handshakeDeadline)
    {
      error = "Timed out waiting for the WebSocket handshake response";
      Close();
      return false;
    }
    int r = FillBuffer(handshakeDeadline, error);
    if (r <= 0)
    {
      if (r == 0)
        error = "Timed out waiting for the WebSocket handshake response";
      Close();
      return false;
    }
  }

  if (!IsWebSocketHandshakeAcceptedForKey(headerText, key))
  {
    // An upgrade that did get a 101 but answered with the wrong
    // Sec-WebSocket-Accept is a different problem from one that was never
    // accepted (an account without access): the first is something in the path
    // (a proxy) that never read this connection's key, so don't send the user
    // chasing credentials for it.
    error = IsWebSocketHandshakeAccepted(headerText)
                ? "The WebSocket upgrade was answered, but not with the Sec-WebSocket-Accept value this connection's "
                  "key requires -- something between this addon and Dispatcharr is answering for it (response: " +
                      headerText.substr(0, 200) + ")"
                : "Dispatcharr did not accept the WebSocket upgrade (check the account can "
                  "authenticate; response: " +
                      headerText.substr(0, 200) + ")";
    Close();
    return false;
  }

  return true;
}

bool WebSocketClient::SendPong(const std::vector<uint8_t>& payload, int timeoutSeconds, std::string& error)
{
  uint8_t maskKey[4];
  RandomBytes(maskKey, sizeof(maskKey));
  // The frame layout itself lives in dispatcharr::BuildMaskedControlFrame()
  // (WebSocketFrame.{h,cpp}) so it's unit-testable standalone -- see that
  // function's own comment.
  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x0A, payload, maskKey); // opcode PONG
  return SendAll(frame.data(), frame.size(), timeoutSeconds, error);
}

bool WebSocketClient::SendClose(int timeoutSeconds, std::string& error)
{
  uint8_t maskKey[4];
  RandomBytes(maskKey, sizeof(maskKey));
  std::vector<uint8_t> frame = BuildMaskedControlFrame(0x08, {}, maskKey); // opcode CLOSE, empty payload
  return SendAll(frame.data(), frame.size(), timeoutSeconds, error);
}

int WebSocketClient::ReceiveTextMessage(std::string& message, int timeoutSeconds, std::string& error)
{
  message.clear();
  // m_assembledMessage/m_messageKind are members, not locals -- see their
  // own comment in WebSocketClient.h for why: a plain header-read timeout
  // just below returns 0 without losing whatever fragment (FIN=0 frames
  // already accumulated) is still in progress, so a caller that retries
  // after a 0 (this method's own documented contract) resumes it instead
  // of silently dropping it.

  // One deadline for this whole call, not a fresh timeoutSeconds per read: the wait for a
  // frame to begin runs against it, and so does the loop itself -- a peer feeding a steady
  // stream of ping/pong/binary frames never leaves this loop otherwise, and the realtime
  // thread that calls this has to come back up to notice a stop request (docs/CLOSED_ITEMS.md,
  // "Lower-severity WebSocketClient.cpp gaps"). Giving up between frames is safe for the same
  // reason a header timeout is: the read position is at a frame boundary.
  const auto callDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

  for (bool firstFrame = true;; firstFrame = false)
  {
    if (!firstFrame && std::chrono::steady_clock::now() >= callDeadline)
      return 0;

    // A timeout here (r == 0) is safe to propagate as a benign "nothing
    // new yet" -- m_recvPos is exactly at a frame boundary, so the next
    // call starts a genuinely fresh header read. That's NOT true for any
    // of the reads below this point, once part of this same frame has
    // already been consumed: a mid-frame timeout there escalates to a
    // hard error instead (see each one's own comment) -- a real,
    // confirmed bug this fixes: propagating 0 from a mid-frame timeout
    // left m_recvPos pointing partway into the still-unread remainder of
    // this frame, so the next call's own header read misinterpreted
    // whatever payload bytes happened to already be there as a brand-new
    // frame header, desyncing the whole stream (usually recovered from
    // via the sanity-limit check below or a bogus close, then a
    // reconnect -- but wrong regardless).
    uint8_t header[2];
    int r = ReadExact(header, 2, callDeadline, error);
    if (r <= 0)
      return r;
    // The rest of this frame has to arrive within timeoutSeconds of its start, however slowly
    // the peer drips it: a mid-frame stall is a hard error (below), and each read used to get
    // its own fresh budget, so a frame trickled one byte at a time could take a payload's worth
    // of timeouts.
    const auto frameDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

    // The FIN/opcode/MASK-bit/7-bit-length parse itself lives in
    // dispatcharr::ParseFrameHeaderBytes() (WebSocketFrame.{h,cpp}) so it's
    // unit-testable standalone -- see that function's own comment.
    WebSocketFrameHeader parsedHeader = ParseFrameHeaderBytes(header);
    bool fin = parsedHeader.fin;
    uint8_t opcode = parsedHeader.opcode;
    bool masked = parsedHeader.masked;
    uint64_t len = parsedHeader.payloadLength7Bit;

    if (len == 126)
    {
      uint8_t ext[2];
      r = ReadExact(ext, 2, frameDeadline, error);
      if (r == 0)
      {
        error = "WebSocket read timed out mid-frame";
        return -1;
      }
      if (r < 0)
        return r;
      len = DecodeExtendedPayloadLength16(ext);
    }
    else if (len == 127)
    {
      uint8_t ext[8];
      r = ReadExact(ext, 8, frameDeadline, error);
      if (r == 0)
      {
        error = "WebSocket read timed out mid-frame";
        return -1;
      }
      if (r < 0)
        return r;
      len = DecodeExtendedPayloadLength64(ext);
    }
    // RFC 6455 rules this client used to assume: a server never masks, and a control
    // frame is short and unfragmented. Only the extended lengths above are known by now.
    if (!IsValidServerFrame(opcode, fin, masked, len))
    {
      error = "WebSocket peer sent a frame that violates RFC 6455 (masked, or an oversized/fragmented control frame)";
      return -1;
    }
    // kMaxFramePayload caps a single frame, but a message fragmented
    // across many continuation frames (opcode 0x0) could otherwise
    // accumulate in `m_assembledMessage` without any overall bound --
    // cap the running total too, not just each individual frame.
    if (len > kMaxFramePayload || m_assembledMessage.size() + len > kMaxFramePayload)
    {
      error = "WebSocket frame payload exceeds sanity limit";
      return -1;
    }

    uint8_t maskKey[4] = {0, 0, 0, 0};
    if (masked)
    {
      r = ReadExact(maskKey, 4, frameDeadline, error);
      if (r == 0)
      {
        error = "WebSocket read timed out mid-frame";
        return -1;
      }
      if (r < 0)
        return r;
    }

    std::vector<uint8_t> payload(static_cast<size_t>(len));
    if (len > 0)
    {
      r = ReadExact(payload.data(), payload.size(), frameDeadline, error);
      if (r == 0)
      {
        error = "WebSocket read timed out mid-frame";
        return -1;
      }
      if (r < 0)
        return r;
      if (masked)
        UnmaskPayload(payload, maskKey);
    }

    switch (opcode)
    {
    case 0x9: // ping
      if (!SendPong(payload, timeoutSeconds, error))
        return -1;
      continue;
    case 0xA: // pong
      continue;
    case 0x8: // close
      SendClose(timeoutSeconds, error);
      error = "WebSocket connection closed by peer (close frame)";
      return -1;
    default:
      // Everything else (text/continuation/binary/reserved) is a data
      // frame -- the accumulate-vs-drop-vs-complete decision itself lives
      // in dispatcharr::DecideWebSocketDataFrameAction() (WebSocketFrame.{h,cpp})
      // so it's unit-testable standalone -- see that function's own
      // comment, including the real bug this replaced (a binary frame's
      // own continuations could end up accumulated and returned as text).
      switch (DecideWebSocketDataFrameAction(opcode, fin, m_messageKind))
      {
      case WebSocketDataFrameAction::kAccumulate:
        m_assembledMessage.insert(m_assembledMessage.end(), payload.begin(), payload.end());
        continue;
      case WebSocketDataFrameAction::kAccumulateAndComplete:
        m_assembledMessage.insert(m_assembledMessage.end(), payload.begin(), payload.end());
        message.assign(reinterpret_cast<const char*>(m_assembledMessage.data()), m_assembledMessage.size());
        m_assembledMessage.clear(); // reset for the next message on this same connection
        return 1;
      case WebSocketDataFrameAction::kDrop:
        continue;
      case WebSocketDataFrameAction::kProtocolError:
        error = "WebSocket peer violated its own message fragmentation";
        return -1;
      }
    }
  }
}

} // namespace dispatcharr
