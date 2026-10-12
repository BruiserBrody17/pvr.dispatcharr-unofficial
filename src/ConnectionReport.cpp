#include "ConnectionReport.h"

#include "AuthBackoff.h"

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
  if (httpStatus >= 500)
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
  if (m_observed == ConnectionReport::kNone || m_observed == m_delivered)
    return false;
  if (m_observed == ConnectionReport::kConnected && !connectedReady)
    return false;
  out = m_observed;
  m_delivered = m_observed;
  return true;
}

} // namespace dispatcharr
