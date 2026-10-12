#pragma once

#include <mutex>

namespace dispatcharr
{

// What the addon tells Kodi about its link to Dispatcharr through the PVR API's ConnectionStateChange callback.
// Only the three states a login attempt can prove; Kodi's CONNECTING is deliberately never reported (it makes Kodi
// ignore the client until CONNECTED, which a server that never answers would turn into a client Kodi never uses).
// kNone means "this outcome proves nothing".
enum class ConnectionReport
{
  kNone,
  kConnected,
  kUnreachable,
  kAccessDenied,
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

  // The state to deliver, if it differs from the last one delivered; marks it delivered. kConnected is held back while
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
