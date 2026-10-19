#pragma once

namespace dispatcharr
{

// How long the calls that must stay short may wait for Dispatcharr, derived from the configured connection timeout
// (`timeout` in settings.xml, 30 s by default): a sixth of it, never under 5 s, never over 30 s, and never more than
// the timeout itself. At the default that is the 5 s the fourteenth and fifteenth hardening sweeps chose.
//
// These are the requests a viewer is made to wait for when the server has gone: the stop request of Stop, the
// live-edge manifest refresh a blocked read repeats, an in-progress recording's steady-state lookup, playlist fetch and
// keep-alive, the startup version check, the catch-up session request Kodi makes on its main thread, and the
// authentication in front of them. Against a server that accepts
// connections and never answers each took the full timeout (30 s), and Stop waited for several of them, up to 140 s
// (docs/OPEN_ITEMS.md). A server that merely answers slowly is the other side of the same trade: with 6 s of latency on
// every request, a bound fixed at 5 s makes a running stream's refreshes always time out. Deriving it from the
// connection timeout instead of fixing it gives such a deployment one number to raise (docs/OPEN_ITEMS.md, closed
// 2026-10-06), and raises the bound in proportion: 60 s gives 10 s, 180 s gives 30 s.
//
// An open still waits the whole configured timeout (a stream's cold start is allowed to be slow); only the calls above
// use this.
constexpr long kMinShortRequestTimeoutMs = 5000;
constexpr long kMaxShortRequestTimeoutMs = 30000;

constexpr long ShortRequestTimeoutMs(int timeoutSeconds)
{
  const long fullMs = static_cast<long>(timeoutSeconds) * 1000;
  long ms = fullMs / 6;
  if (ms < kMinShortRequestTimeoutMs)
    ms = kMinShortRequestTimeoutMs;
  if (ms > kMaxShortRequestTimeoutMs)
    ms = kMaxShortRequestTimeoutMs;
  if (fullMs > 0 && ms > fullMs)
    ms = fullMs;
  return ms;
}

// The wall-clock ceiling on an open's cold-start loop (OpenLiveTimeshiftStream(), OpenInProgressRecordingStream()).
// Each loop is bounded by a count of tries, but every try may take the whole configured timeout, so a server that
// answered the start request and then goes silent held the open for tries x timeout (about fifteen minutes for the
// live loop at the default 30 s; found by the 2026-10-11 stalled-stream review). The budget is the loop's own healthy
// waiting (its tries x its sleep, which a slow-to-start encoder legitimately uses) plus one short bound, and it is
// checked after each failed try, so a try that has already used a full timeout is the last one: a silent server ends
// the open after one timeout (30 s at the default) instead of two (61 s measured with the earlier allowance of a full
// timeout beyond the healthy wait). That matters because Kodi runs a Stop on its main thread and waits for the open to
// return, so the open's length is how long the UI is held (docs/OPEN_ITEMS.md, the entry on a stalled open). A server
// that merely answers slowly is not cut short: each try still gets the whole configured timeout, only whether a
// further try is allowed after a failed one is decided here. At the default the budget is 20 s for the live loop (30
// tries of 500 ms) and 50 s for an in-progress recording's (90 tries).
constexpr long ColdStartWallClockBudgetMs(int maxTries, int sleepMs, int timeoutSeconds)
{
  return static_cast<long>(maxTries) * sleepMs + ShortRequestTimeoutMs(timeoutSeconds);
}

// How many tries the live cold-start loop (OpenLiveTimeshiftStream()) is allowed, given what its last failed try was.
// The base count (30 tries of 500 ms, 15 s) is the whole allowance for a try that got no answer from the plugin
// (a timeout, a refused connection, an HTTP error): that is the stalled-server case ColdStartWallClockBudgetMs()
// bounds. A try the plugin did answer, with "live playlist not found" and no `fatal` flag, is positive evidence that
// its ffmpeg is alive and the channel is still coming up (the plugin answers `fatal` once ffmpeg has exited), so the
// loop may wait out the longer count for a channel whose provider is slow to deliver its first full segment (a user's
// two slow channels gave up at 15 s and played at once on the second try, docs/OPEN_ITEMS.md).
constexpr int ColdStartTryLimit(bool pluginAnsweredNotReady, int baseTries, int extendedTries)
{
  return pluginAnsweredNotReady && extendedTries > baseTries ? extendedTries : baseTries;
}

// Whether an in-progress recording's manifest refresh is in steady state, where its requests get the short bound: it
// already has segments AND is not the cold-start loop of an open. A reopened recording is seeded from the segment cache
// and so already "has segments", but it is a cold start all the same: a slow server must get the configured timeout,
// and a failed first refresh must stay retryable instead of failing the open.
constexpr bool IsInProgressSteadyState(bool coldStart, bool haveSegments)
{
  return !coldStart && haveSegments;
}

// Whether a steady-state refresh gives up before fetching the playlist because the recording lookup got no answer: the
// server is then very likely unreachable and the playlist fetch would only wait out another bound. Not when the lookup
// never reached the server (authentication refused during a cooldown or backoff, `authenticationNotCompleted`):
// the playlist needs only the API key, and skipping it stopped a growing recording at the tail for as long as a login
// cooldown ran (30 s after a blip, up to 30 minutes after a rejected login) while the server was fine.
constexpr bool ShouldSkipPlaylistAfterUnansweredLookup(bool steadyState, bool lookupOk, long lookupHttpStatus,
                                                       bool authenticationNotCompleted)
{
  return steadyState && !lookupOk && lookupHttpStatus == 0 && !authenticationNotCompleted;
}

} // namespace dispatcharr
