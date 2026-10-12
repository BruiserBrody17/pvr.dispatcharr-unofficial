#pragma once

#include <chrono>
#include <mutex>

namespace dispatcharr
{

// What the addon tells Kodi about its link to Dispatcharr through the PVR API's ConnectionStateChange callback.
// Kodi's CONNECTING is deliberately never reported (it makes Kodi ignore the client until CONNECTED, which a server
// that never answers would turn into a client Kodi never uses). kNone means "this outcome proves nothing".
// What is observed is kConnected, kUnreachable ("the server cannot be reached", from a login, a refresh or a run of
// failed requests) and kAccessDenied. kDisconnected is only ever delivered, never observed: the latch turns an
// unreachable server into it once the link had been reported up, because Kodi words that as "Connection lost" and an
// unreachable one as "Server is unreachable", and the first is the true one for a server that went away mid-session.
enum class ConnectionReport
{
  kNone,
  kConnected,
  kUnreachable,
  kAccessDenied,
  kDisconnected,
};

// The report for a failed login or token refresh, by HTTP status (0 == no response at all). The split is the one
// ShouldCountTowardLoginBackoff() (AuthBackoff.h) already makes: a rejection of the credentials or of the network
// (400/401/403) is "access denied"; no response or a 5xx is "unreachable". 429 is the login throttle, which only
// follows earlier rejections that were already reported, and says nothing new. Any other status (a 404 from a proxy
// that is not Dispatcharr, a malformed body) is left unreported, since "server mismatch" is not a state this slice
// reports. callerHasShortBound is true when the attempt ran under a caller's short deadline (a live read's, not the
// configured timeout): a timeout there only proves the server was slower than that caller could wait, so it is not
// reported as unreachable. A refused or unresolvable connection under a short deadline is lost with it, an accepted
// cost of not threading the curl result up from Request().
ConnectionReport ReportForAuthFailure(long httpStatus, bool callerHasShortBound);

// Whether a failed transfer says the server cannot be reached at all, by libcurl's result code: the host did not
// resolve or refused or dropped the connection, or it hung up without a reply. A timeout is not one of these, because a
// heavy endpoint (the guide download takes 13 to 30 s against a 30 s default) times out against a perfectly healthy
// server; except when connectNeverCompleted, a timeout in which the connection was never established, which is a
// host that drops packets instead of refusing them.
bool IsConnectLevelFailure(int curlCode, bool connectNeverCompleted);

// Decides from the run of request outcomes whether the link is down, so that one reset or a short blip does not tell
// the user the connection was lost, and a flaky link cannot flash "Connection lost" and "Connection established"
// (Kodi never suppresses either). Down means kFailuresToTrip connect-level failures in a row, none answered in
// between, spanning at least kMinSpan, with no gap between two of them over kMaxGap (a failure long ago is not part of
// a streak). Any answer from the server ends the streak.
class LinkHealth
{
public:
  static constexpr int kFailuresToTrip = 3;
  static constexpr std::chrono::seconds kMinSpan{10};
  static constexpr std::chrono::seconds kMaxGap{60};

  // One finished request. curlCode is libcurl's result (0 when the server answered), httpCode the status when it did,
  // authenticated whether the request carried the account's token. Returns kUnreachable when this failure completes
  // the streak (and for each later one in it), kConnected for an authenticated 2xx, which proves the account is
  // logged in and the server working; otherwise kNone. A 5xx answer proves neither (a gateway answering for a dead
  // Dispatcharr), so it neither counts nor ends a streak, and neither does a failure that is not connect-level.
  ConnectionReport OnAttempt(std::chrono::steady_clock::time_point now, int curlCode, long httpCode, bool authenticated,
                             bool connectNeverCompleted);

private:
  std::mutex m_mutex;
  int m_failures = 0;
  std::chrono::steady_clock::time_point m_firstFailure;
  std::chrono::steady_clock::time_point m_lastFailure;
};

// The newest connection observation, handed from whichever thread learned it to the one thread that talks to Kodi.
// Kodi's callback runs addon code (the capability and name getters) on the calling thread and takes its own locks, so
// it must never be made while an addon lock is held; the observing thread therefore only records here, and a worker
// delivers. Latest wins: a burst of changes delivers only where it ended, and delivery order cannot invert.
class ConnectionReportLatch
{
public:
  // Records the newest observation. kNone is ignored. Returns true when it differs from the one already recorded, i.e.
  // when a delivery is worth scheduling.
  bool Observe(ConnectionReport report);

  // The state to deliver, if it differs from the last one delivered; marks it delivered. An unreachable server is
  // delivered as kDisconnected when the link had been delivered as connected (or already as disconnected, which then
  // delivers nothing more). kConnected is held back while
  // connectedReady is false: Kodi re-reads the addon's capabilities on CONNECTED, so it must not be reported before
  // the account's DVR access level is known, or the re-read would see the same fail-open answer it already has. A held
  // kConnected stays pending until a later call passes connectedReady = true.
  bool TakePending(ConnectionReport& out, bool connectedReady);

private:
  std::mutex m_mutex;
  ConnectionReport m_observed = ConnectionReport::kNone;
  ConnectionReport m_delivered = ConnectionReport::kNone;
};

} // namespace dispatcharr
