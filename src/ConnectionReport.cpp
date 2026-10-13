#include "ConnectionReport.h"

#include "AuthBackoff.h"

#include <curl/curl.h>

namespace dispatcharr
{

ConnectionReport ReportForAuthFailure(long httpStatus, bool callerHasShortBound)
{
  if (httpStatus == 429)
    return ConnectionReport::kNone;
  if (ShouldCountTowardLoginBackoff(httpStatus))
    return ConnectionReport::kAccessDenied;
  if (httpStatus == 0)
    return callerHasShortBound ? ConnectionReport::kNone : ConnectionReport::kUnreachable;
  // Before the 5xx range: 501 is a server that does not implement POST, not a gateway with nothing behind it.
  if ((httpStatus >= 200 && httpStatus < 400) || httpStatus == 404 || httpStatus == 405 || httpStatus == 501)
    return ConnectionReport::kServerMismatch;
  if (httpStatus >= 500)
    return ConnectionReport::kUnreachable;
  return ConnectionReport::kNone;
}

bool IsConnectLevelFailure(int curlCode, bool connectNeverCompleted)
{
  switch (curlCode)
  {
  case CURLE_COULDNT_RESOLVE_PROXY:
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_CONNECT:
  case CURLE_GOT_NOTHING:
  case CURLE_SEND_ERROR:
  case CURLE_RECV_ERROR:
    return true;
  case CURLE_OPERATION_TIMEDOUT:
    return connectNeverCompleted;
  default:
    return false;
  }
}

ConnectionReport LinkHealth::OnAttempt(std::chrono::steady_clock::time_point now, int curlCode, long httpCode,
                                       bool authenticated, bool connectNeverCompleted)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (curlCode == 0)
  {
    if (httpCode >= 500)
      return ConnectionReport::kNone;
    m_failures = 0;
    return authenticated && httpCode >= 200 && httpCode < 300 ? ConnectionReport::kConnected : ConnectionReport::kNone;
  }
  if (!IsConnectLevelFailure(curlCode, connectNeverCompleted))
    return ConnectionReport::kNone;
  if (m_failures > 0 && now - m_lastFailure > kMaxGap)
    m_failures = 0;
  if (m_failures == 0)
    m_firstFailure = now;
  ++m_failures;
  m_lastFailure = now;
  if (m_failures >= kFailuresToTrip && now - m_firstFailure >= kMinSpan)
    return ConnectionReport::kUnreachable;
  return ConnectionReport::kNone;
}

bool ConnectionReportLatch::Observe(ConnectionReport report)
{
  if (report == ConnectionReport::kNone)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool changed = report != m_observed;
  m_observed = report;
  return changed;
}

bool ConnectionReportLatch::TakePending(ConnectionReport& out, bool connectedReady)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_observed == ConnectionReport::kNone)
    return false;
  ConnectionReport target = m_observed;
  if (target == ConnectionReport::kUnreachable &&
      (m_delivered == ConnectionReport::kConnected || m_delivered == ConnectionReport::kDisconnected))
    target = ConnectionReport::kDisconnected;
  if (target == m_delivered)
    return false;
  if (target == ConnectionReport::kConnected && !connectedReady)
    return false;
  out = target;
  m_delivered = target;
  return true;
}

} // namespace dispatcharr
