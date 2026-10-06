#include "RecordingVisibility.h"

namespace dispatcharr
{

bool IsListedAsRecording(const Recording& rec)
{
  return !rec.isUpcoming;
}

bool IsListedAsTimer(const Recording& rec, time_t now)
{
  if (rec.isInProgress)
    return true;
  return rec.isUpcoming && !IsMissedOccurrence(rec.isUpcoming, rec.endTime, now);
}

bool IsMissedOccurrence(bool isUpcoming, time_t endTime, time_t now)
{
  return isUpcoming && endTime > 0 && endTime <= now;
}

bool ShouldUseGrowingBufferPlayback(const Recording& rec)
{
  return rec.isInProgress || rec.hlsDirStillPresent;
}

bool ResolveInProgressFinished(bool lookupOk, bool isInProgress, bool previousFinished, bool allProbesSucceeded)
{
  if (!lookupOk)
    return previousFinished;
  if (isInProgress)
    return false;
  return allProbesSucceeded;
}

bool GateFinishedOnEndList(bool candidateFinished, bool playlistHasEndList,
                           std::optional<std::chrono::steady_clock::time_point>& waitingSince,
                           std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration grace)
{
  if (!candidateFinished)
  {
    waitingSince.reset();
    return false;
  }
  if (playlistHasEndList)
  {
    waitingSince.reset();
    return true;
  }
  if (!waitingSince)
    waitingSince = now;
  return now - *waitingSince >= grace;
}

bool ShouldRetryInProgressColdStart(long playlistHttpStatus, bool lookupOk, bool isInProgress)
{
  return playlistHttpStatus == 404 && lookupOk && isInProgress;
}

bool IsInProgressContentGone(bool hadSegments, bool playlistWasNotFound, bool lookupOk, long lookupHttpStatus,
                             bool isInProgress)
{
  if (!hadSegments || !playlistWasNotFound)
    return false;
  if (lookupOk)
    return !isInProgress;
  return lookupHttpStatus == 404;
}

bool ShouldStopInsteadOfDelete(bool forceDelete, bool lookupOk, bool serverSaysInProgress)
{
  return forceDelete || (lookupOk && serverSaysInProgress);
}

bool IsAlreadyFinishedRecording(bool lookupOk, bool isInProgress, bool isUpcoming)
{
  return lookupOk && !isInProgress && !isUpcoming;
}

DeleteTimerAction DecideDeleteTimerAction(bool forceDelete, bool lookupOk, long lookupHttpStatus, bool isInProgress,
                                          bool isUpcoming)
{
  if (lookupOk)
  {
    if (IsAlreadyFinishedRecording(lookupOk, isInProgress, isUpcoming))
      return DeleteTimerAction::kAlreadyDone;
    return ShouldStopInsteadOfDelete(forceDelete, lookupOk, isInProgress) ? DeleteTimerAction::kStop
                                                                          : DeleteTimerAction::kDelete;
  }
  if (lookupHttpStatus == 404)
    return DeleteTimerAction::kAlreadyDone;
  return forceDelete ? DeleteTimerAction::kStop : DeleteTimerAction::kRefuseUnverified;
}

InProgressColdStartStep DecideInProgressColdStartStep(bool refreshOk, bool retryable, bool haveSegments, bool finished)
{
  if (!refreshOk)
    return retryable ? InProgressColdStartStep::kWait : InProgressColdStartStep::kFail;
  if (haveSegments)
    return InProgressColdStartStep::kReady;
  if (finished)
    return InProgressColdStartStep::kFinishedEmpty;
  return InProgressColdStartStep::kWait;
}

} // namespace dispatcharr
