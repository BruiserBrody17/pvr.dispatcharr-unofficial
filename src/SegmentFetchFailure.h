#pragma once

#include <algorithm>
#include <chrono>

namespace dispatcharr
{

// Bounded-retry decision behind ReadLiveTimeshiftStream()'s segment-body-
// fetch error handling (DispatcharrClient.cpp) -- found live 2026-09-28
// while reproducing a paused-viewer heartbeat scenario, not specific to
// it: a segment fetch returning any HTTP status other than 200/206/404
// used to just return -1 unconditionally, forever, with nothing else
// attempted. Kodi's own core retries a -1 Read() near-immediately rather
// than giving up, so a single dead/replaced buffer turned into an
// unbounded, zero-backoff request storm against the same doomed URL --
// confirmed live at roughly 7-8 requests/sec, sustained indefinitely
// (1,230 identical failed fetches logged over ~2m37s in one test run,
// only ending because the test was stopped by hand), with Kodi's own UI
// showing normal-looking advancing playback the whole time -- a silent
// hang, not a clean, diagnosable failure.
//
// consecutiveFailures is the caller's own running count of segment
// fetches that have failed this way in a row this Open() session (a 404
// counts too since 2026-10-29; reset to 0 by a successful read -- see
// LiveTimeshiftStreamState::consecutiveSegmentFetchFailures's own
// comment). refreshFatal is whatever the caller's own
// RefreshLiveManifest(force=true, ...) call for this same failure came
// back with: true means the plugin's own get_live_manifest action
// already positively confirmed this buffer is gone (the case actually
// reproduced live -- _get_buffer_state() returned None server-side),
// which ends the retry loop immediately regardless of the attempt count,
// exactly like every other RefreshLiveManifest() call site in this file
// already treats a true fatalOut.
//
// maxAttempts is the fail-safe bound for the opposite, unconfirmed case:
// the manifest refresh itself reports the buffer is still there, but
// this exact segment fetch keeps failing anyway -- e.g. a stale
// access_token cached from before the buffer silently died and got
// replaced by a fresh one for the same channel, which RefreshLiveManifest()
// has no way to detect on its own, since get_live_manifest's own response
// never carries an access_token (only start_buffer's/a reattach's does).
// Without this bound, that specific case would double this addon's own
// request rate against the server (both the doomed segment fetch and a
// forced manifest refresh, every single Read() call) rather than
// actually fixing anything -- so once the count of consecutive failures
// reaches maxAttempts, give up and treat it as fatal too, on the
// reasoning that a real transient blip (a one-off 5xx from Dispatcharr's
// own reverse proxy) resolves within a handful of attempts, and anything
// that doesn't is not going to resolve itself no matter how many more
// times this addon asks.
//
// failingFor/minFailingFor (added 2026-10-02, found by the manual-testing pass -- docs/OPEN_ITEMS.md): the
// count alone ended live playback for good within about two seconds of the server becoming unreachable, since
// four refused connections in a row take no longer than that. An outage that long is routine (Dispatcharr or
// its plugin restarting, a network blip), and playback resumed fine once the server was back -- except that
// the stream had already been marked fatal. So an unconfirmed failure only counts as dead once the streak
// has both reached maxAttempts and lasted minFailingFor; the retry delay below keeps that wait cheap.
constexpr bool ShouldGiveUpAfterSegmentFetchFailure(int consecutiveFailures, bool refreshFatal, int maxAttempts,
                                                    std::chrono::milliseconds failingFor,
                                                    std::chrono::milliseconds minFailingFor)
{
  return refreshFatal || (consecutiveFailures >= maxAttempts && failingFor >= minFailingFor);
}

// How long to wait before the next try after the Nth consecutive failure: 250 ms, doubling, capped at 2 s. A
// flat 250 ms made a half-minute outage cost over a hundred requests, each a segment fetch and a manifest
// refresh.
constexpr int SegmentFetchRetryDelayMs(int consecutiveFailures)
{
  int delay = 250;
  for (int i = 1; i < consecutiveFailures && delay < 2000; ++i)
    delay *= 2;
  return std::min(delay, 2000);
}

// A recording read (a completed recording's ranged GET, or an in-progress one's segment GET) that fails because
// the server could not be reached, or answered 500/502/503/504 (a proxy in front of a restarting Dispatcharr, or
// Django's answer to a transient database or I/O error; 500 added 2026-10-05, the fifteenth hardening sweep: a
// three-second blip of 500s ended a recording read for good once PermanentReadFailureTracker existed), is
// retried for a while before the read reports failure (added 2026-10-02, found by the manual-testing pass --
// docs/OPEN_ITEMS.md). A failed read ends recorded playback once Kodi's queue drains -- unlike a live channel,
// whose NextStream() answers "retry", a recording (finished or in progress) answers "none", so after a failed read
// Kodi reads again only while its players still hold data -- so the first refused connection used to end it,
// measured live at about eight seconds into an outage. The retries therefore stay inside one Read() call, where
// Kodi's player waits as for a slow stream instead of draining its queue: handing them back in one-second slices,
// as the live read does, ended an in-progress recording 9 and 14 s into a 25 s outage that the in-call budget rode
// out (measured live 2026-11-02). The cost is that a Stop during an outage waits for the read in progress. Any
// other status (a 4xx, a size disagreement) is not a transient failure and still fails at once.
constexpr bool IsTransientReadFailure(bool transferOk, long httpCode)
{
  return !transferOk || httpCode == 500 || httpCode == 502 || httpCode == 503 || httpCode == 504;
}

// Whether a read that has been failing transiently for `waited` should try again. The budget is how long one
// Read() call may keep trying: a blocked read is just buffering to the viewer, but not for ever.
constexpr bool ShouldKeepRetryingTransientRead(std::chrono::milliseconds waited, std::chrono::milliseconds budget)
{
  return waited < budget;
}

// What ReadLiveTimeshiftStream() does after a failed segment fetch, once HandleLiveTimeshiftSegmentFetchFailure() has
// taken its retry delay and asked for another try. The retries used to stay inside one Read() for up to 25 s, and Kodi
// closes a player only between reads, on its main thread, so a Stop during a run of failures held Kodi's whole
// interface until that read returned: 18.6 s, measured 2026-10-29 (docs/CLOSED_ITEMS.md, "A Stop during a run of failed
// live segment fetches holds Kodi's interface for up to 25 s"). A read now hands control back once it has run for a
// slice, with -1, which Kodi retries on a live channel (a 0 would set its end of file, the reason the retries exist).
// The handler's give-up rule is what ends a stream that stays dead, with 0 and a notice; the old 25 s budget answered 0
// a few seconds before it, which with reads handed back would leave Kodi at end of file with nothing said, so it is
// gone. kStop is the addon being torn down.
enum class LiveSegmentRetryStep
{
  kTryAgain, // still inside the slice: fetch again in this same read
  kHandBack, // the slice is used: return -1, Kodi reads again
  kStop,     // the addon is being torn down: return 0
};

constexpr LiveSegmentRetryStep DecideLiveSegmentRetryStep(bool aborted, std::chrono::milliseconds sinceReadStarted,
                                                          std::chrono::milliseconds slice)
{
  if (aborted)
    return LiveSegmentRetryStep::kStop;
  return sinceReadStarted >= slice ? LiveSegmentRetryStep::kHandBack : LiveSegmentRetryStep::kTryAgain;
}

// A recording read that fails for a reason that will not clear (a 403 or 404 on the file or segment, a file
// replaced under the stream) returned -1 on every call, and Kodi retries a failed read at once -- measured about 226
// segment GETs in 9 s in the glue harness, and about 8 per second in Kodi, until the player gave up (found by the
// fourteenth hardening sweep). After a run of such failures that has lasted long enough to rule out a blip the
// stream ends as EOF (0, which Kodi does not retry) and stays ended, with the cause already logged.
constexpr int kPermanentReadFailureMinCount = 5;
constexpr std::chrono::milliseconds kPermanentReadFailureMinSpan{3000};

struct PermanentReadFailureTracker
{
  int count = 0;
  std::chrono::steady_clock::time_point firstAt{};
  bool ended = false;
};

// Records one more such failure at `now`; true when the stream should now be ended (and stays so: `ended`).
inline bool RecordPermanentReadFailure(PermanentReadFailureTracker& tracker, std::chrono::steady_clock::time_point now)
{
  if (tracker.ended)
    return true;
  if (tracker.count == 0)
    tracker.firstAt = now;
  ++tracker.count;
  if (tracker.count >= kPermanentReadFailureMinCount && now - tracker.firstAt >= kPermanentReadFailureMinSpan)
    tracker.ended = true;
  return tracker.ended;
}

// A read that delivered bytes ends the run: the failures were not consecutive.
inline void ResetPermanentReadFailures(PermanentReadFailureTracker& tracker)
{
  if (!tracker.ended)
  {
    tracker.count = 0;
    tracker.firstAt = {};
  }
}

} // namespace dispatcharr
