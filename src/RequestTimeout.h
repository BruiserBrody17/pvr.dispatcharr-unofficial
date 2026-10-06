#pragma once

namespace dispatcharr
{

// How long the calls that must stay short may wait for Dispatcharr, derived from the configured connection timeout
// (`timeout` in settings.xml, 30 s by default): a sixth of it, never under 5 s, never over 30 s, and never more than
// the timeout itself. At the default that is the 5 s the fourteenth and fifteenth hardening sweeps chose.
//
// These are the requests a viewer is made to wait for when the server has gone: the stop request of Stop, the
// live-edge manifest refresh a blocked read repeats, an in-progress recording's steady-state lookup, playlist fetch and
// keep-alive, the startup version check, and the authentication in front of them. Against a server that accepts
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
