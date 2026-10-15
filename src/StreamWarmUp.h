#pragma once

#include "RequestTimeout.h"
#include "StreamPropertyUtil.h"

#include <string>

namespace dispatcharr
{

// Kodi 22 probes a plain stream URL before it plays it: a HEAD (Dispatcharr's proxy answers 405), a GET of the same URL
// with `?mimetype=` added, and then the real GET. On a channel the proxy is not already streaming, that first GET is
// the one that starts it, and the real GET arrives while the channel is still coming up: Dispatcharr answers it with
// 503 and `Retry-After: 1`, Kodi does not retry, and the first attempt to play the channel fails (captured on the wire,
// docs/OPEN_ITEMS.md). Kodi 21 sends the one GET and plays. So in the modes that hand Kodi the proxy URL, the addon
// starts the channel itself, in the background, and stays attached while Kodi starts, so the channel is never empty
// while Kodi's requests arrive. It cannot wait for the channel: the call that starts it runs on Kodi's main thread.
//
// Server-side timeshift has no URL (Kodi reads through the addon's own stream callbacks), so there is nothing to warm.
inline bool ShouldWarmLiveStream(int liveTimeshiftMode, const std::string& streamUrl)
{
  return liveTimeshiftMode != kLiveTimeshiftServer && !streamUrl.empty();
}

// The URL as a plain HTTP client sees it: GetLiveStreamUrl() ends with Kodi's `|Connection=close` option syntax, which
// is not part of the URL.
inline std::string StreamUrlWithoutKodiOptions(const std::string& url)
{
  const size_t bar = url.find('|');
  return bar == std::string::npos ? url : url.substr(0, bar);
}

// How long the background warm-up keeps trying to get the channel up. A channel's first bytes took 2 to 7 s on a
// healthy server (the provider connects, the proxy buffers), and one channel in ten took longer than 12 s. Kodi's own
// request would have waited for the same start-up, so this is about as long as Kodi would wait: four times the short
// request bound, 20 s at the default timeout, and no more than a minute however high the timeout is set. A warm-up that
// runs out has only ended its own connection; Kodi's request is unaffected.
constexpr long WarmStreamBudgetMs(int timeoutSeconds)
{
  const long ms = 4 * ShortRequestTimeoutMs(timeoutSeconds);
  return ms > 60000 ? 60000 : ms;
}

// How long the warm-up connection stays attached after the first bytes, draining the stream. Kodi 22 opens and closes
// its own probe connections while it starts playing, and the proxy stops a channel that has no client for a moment: a
// Kodi request that lands in that gap gets the same 503 (measured: closing the warm-up at the first bytes still left
// 3 of 9 cold first attempts failing, while a client held on the channel left none of 6). Kodi's own playback
// connection is up within about 8 s of the call, which is what holds the channel from then on; the hold ends 15 s
// after the first bytes whatever happens, so a channel the user did not go on to watch is not kept running.
constexpr long kWarmHoldMs = 15000;

// Whether the hold goes on, `heldMs` after the first bytes.
inline bool ShouldKeepWarmHold(bool stopRequested, long heldMs)
{
  return !stopRequested && heldMs < kWarmHoldMs;
}

enum class WarmStreamStep
{
  kReady,  // the proxy sent the first bytes: the channel is running
  kRetry,  // not yet: the channel is still coming up, or the answer was lost
  kGiveUp, // an answer that will not change (not found, refused), or the budget is spent
};

// What to do after one warm-up request. `transferFailed` is a transport-level failure (refused, reset, timed out) that
// we did not cause; the request is ended by the addon itself once the first bytes arrive, which is `gotData`.
// 503 is the proxy saying the channel is not ready yet (it sends `Retry-After: 1`); 502 and 504 are a gateway in front
// of it saying the same; and a plain 500 from the proxy was seen once in a live run of twenty cold starts, answered at
// once while the channel was being set up, with the next attempt fine. Any other status, 401, 403 and 404 included, is
// the server's answer to the channel itself and asking again changes nothing.
inline WarmStreamStep DecideWarmStreamStep(bool transferFailed, long httpCode, bool gotData, long elapsedMs,
                                           long budgetMs)
{
  if (!transferFailed && httpCode == 200 && gotData)
    return WarmStreamStep::kReady;
  if (elapsedMs >= budgetMs)
    return WarmStreamStep::kGiveUp;
  if (transferFailed || httpCode == 503 || httpCode == 502 || httpCode == 504 || httpCode == 500 || httpCode == 0)
    return WarmStreamStep::kRetry;
  return WarmStreamStep::kGiveUp;
}

} // namespace dispatcharr
