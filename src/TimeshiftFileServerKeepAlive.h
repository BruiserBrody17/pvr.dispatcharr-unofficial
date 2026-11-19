#pragma once

#include <chrono>

namespace dispatcharr
{

// The pure decision core of DispatcharrClient::MaybeSendLiveTimeshiftKeepAlive() (DispatcharrClient.cpp).
//
// The timeshift_buffer plugin's reaper ends a buffer nobody has shown interest in for idle_timeout_seconds (default
// 30 s). Every successful request to its file server counts as interest, and so do its heartbeat and get_live_manifest
// actions. A viewer that is playing reads segments from the file server all the time. A viewer that has paused, or that
// waits at the live edge for the next segment, reads nothing, and its only sign of life used to be those actions.
//
// From Dispatcharr 0.34 every plugin action is a Celery task on the same prefork pool (six processes by default) as the
// server's own guide and playlist work, and nothing gives it priority. Assigning or changing guide data on many
// channels at once (auto-match, a bulk edit, a playlist sync that creates channels) queues one task per channel that
// reads the whole guide file again (`parse_programs_for_tvg_id`), which can hold every process for minutes. The actions
// then wait in the queue while the reaper, a thread of its own, keeps counting: measured live 2026-11-17 with the pool
// held for about 40 s, the actions took up to 40 s to be answered (docs/CLOSED_ITEMS.md, "A paused viewer's timeshift
// buffer can be ended while Dispatcharr's task pool is busy").
//
// The fix is a HEAD request for the buffer's playlist on the file server, which runs outside that queue. It carries the
// buffer's token and this viewer's id, so it refreshes the buffer and this viewer alike (_touch_heartbeat() in the
// plugin, which answers HEAD since before the viewer id existed). It is only sent once nothing else has reached the
// file server for a whole interval, so a viewer that is playing sends none.
//
// The same request also settles what a wait at the live edge is (DecideTailWaitFileServerCheck() below). A manifest
// refresh the busy pool does not answer in time looked like an outage, which keeps the tail wait's short budget, so a
// viewer at the live edge had playback ended within seconds of the pool filling, with its buffer alive.
constexpr std::chrono::seconds kTimeshiftDefaultIdleTimeout{30};
constexpr std::chrono::seconds kTimeshiftKeepAliveInterval{10};
// After a failed attempt, counted from when it ended. Longer than the in-progress recording keep-alive's 2 s: an
// unreachable file server is a lasting state, not a blip, and every attempt holds a Kodi thread.
constexpr std::chrono::seconds kTimeshiftKeepAliveRetryDelay{5};
// One attempt's own bound, shorter than the API's short timeout: the file server answers a HEAD from a stat() on local
// disk, and a paused viewer's poll already waits on the manifest refresh's bound after it.
constexpr std::chrono::milliseconds kTimeshiftKeepAliveTimeout{2000};
static_assert(kTimeshiftKeepAliveInterval + 2 * (kTimeshiftKeepAliveRetryDelay + kTimeshiftKeepAliveTimeout) <=
                  kTimeshiftDefaultIdleTimeout,
              "the interval and two failed attempts with their retry delays must fit inside the plugin's default idle "
              "timeout, so a third attempt still lands in time");

// Whether a keep-alive is due. A buffer known to be dead (`fatal`) has nothing left to keep. A zero `nextDueAt` (never
// scheduled) counts as due, the "never happened yet" convention of Staleness.h.
inline bool ShouldSendTimeshiftKeepAlive(bool fatal, std::chrono::steady_clock::time_point nextDueAt,
                                         std::chrono::steady_clock::time_point now)
{
  return !fatal && now >= nextDueAt;
}

// Whether the file server took the request, and so refreshed the buffer. A 403 (the token belongs to a buffer that has
// since been replaced) or a 404 (no playlist) refreshed nothing; the manifest poll is what tells those apart and ends
// the stream.
inline bool IsTimeshiftKeepAliveAnswered(bool transportOk, long httpStatus)
{
  return transportOk && httpStatus >= 200 && httpStatus < 300;
}

// When the next keep-alive falls due after `now`: a whole interval after anything that reached the file server (a
// segment read or an answered keep-alive), the retry delay after a failed attempt.
inline std::chrono::steady_clock::time_point NextTimeshiftKeepAliveDueAt(std::chrono::steady_clock::time_point now,
                                                                         bool answered)
{
  return now + (answered ? kTimeshiftKeepAliveInterval : kTimeshiftKeepAliveRetryDelay);
}

// What a live read waiting at the tail does with the file server after a manifest refresh the plugin did not answer.
// kKnownAlive: the file server answered for this buffer within the last interval, so the buffer exists and the wait is
// a stall that earns the stall budget (ComputeLiveTailWaitBudgetMs(), CatchUpUtil.h). kProbe: ask it now. kUnknown: an
// attempt failed less than the retry delay ago (or the buffer is dead), so the refresh's failure stands, which keeps
// an outage's short budget without a HEAD on every one of the tail wait's quarter-second refreshes. In a whole-server
// outage the file server is gone with the API, so nothing changes there. Segment reads do not count as an answer: they
// stop at the tail, and counting them would stretch an outage that begins at the live edge to the stall budget.
enum class TailWaitFileServerCheck
{
  kKnownAlive,
  kProbe,
  kUnknown,
};

inline TailWaitFileServerCheck DecideTailWaitFileServerCheck(bool fatal,
                                                             std::chrono::steady_clock::time_point lastAnsweredAt,
                                                             std::chrono::steady_clock::time_point lastAttemptAt,
                                                             std::chrono::steady_clock::time_point now)
{
  using TimePoint = std::chrono::steady_clock::time_point;
  if (fatal)
    return TailWaitFileServerCheck::kUnknown;
  if (lastAnsweredAt != TimePoint{} && now - lastAnsweredAt <= kTimeshiftKeepAliveInterval)
    return TailWaitFileServerCheck::kKnownAlive;
  if (lastAttemptAt != TimePoint{} && now - lastAttemptAt < kTimeshiftKeepAliveRetryDelay)
    return TailWaitFileServerCheck::kUnknown;
  return TailWaitFileServerCheck::kProbe;
}

// The manifest route (timeshift_buffer 0.8.19, plugin.py's _MANIFEST_ROUTE_NAME): the same answer as the
// get_live_manifest action, from the file server. Polled instead of the action once start_buffer's answer or a manifest
// has named it, because from Dispatcharr 0.34 each action leaks two Redis connections in the uwsgi worker that
// dispatches it, and a viewer at the live edge polls two to three times a second (docs/OPEN_ITEMS.md, "Dispatcharr 0.34
// leaks two Redis connections per plugin action"). After a failed attempt the action answers instead, and the route is
// tried again only this long after: a failure means the buffer is gone (the action then says so), the file server is
// out of reach (so are the segments), or Redis is, none of which a quarter-second retry at the live tail would see
// change, and each failed attempt is a round trip spent before the action's own.
constexpr std::chrono::seconds kManifestRouteRetryDelay{10};
// One route attempt's own bound, connection included. Shorter than the short request timeout (5 s at the least) the
// action then gets in full: an attempt that fails holds the thread Kodi's player or GetStreamTimes() waits on, so a
// file server that does not answer would otherwise double that wait once per retry delay (found by the branch's
// review). The route answers from Redis and the files' stat()s; three seconds is far beyond what that takes.
constexpr std::chrono::milliseconds kManifestRouteTimeout{3000};

// Whether a refresh tries the manifest route first: start_buffer's answer or a manifest has named one, and the retry
// delay after the last failed attempt has run out. A zero `retryAt` (never failed) counts as run out, the convention of
// Staleness.h.
inline bool ShouldTryManifestRoute(bool routeKnown, std::chrono::steady_clock::time_point retryAt,
                                   std::chrono::steady_clock::time_point now)
{
  return routeKnown && now >= retryAt;
}

// Whether a live read owes the plugin its heartbeat action, once `interval` has passed since the last one was due. Not
// while the plugin's manifest route is known (so the plugin is 0.8.19 or later) and the file server answered for this
// viewer within that interval: a route poll and a keep-alive name the viewer and write the same refresh the action
// does (plugin.py's _apply_read_heartbeat()), so the action would add nothing but one more Redis connection leaked by a
// Dispatcharr 0.34 worker (kManifestRouteRetryDelay above). With an older plugin, or while the route fails, it is sent.
inline bool ShouldSendTimeshiftHeartbeatAction(bool manifestRouteKnown,
                                               std::chrono::steady_clock::time_point lastFileServerAnswerAt,
                                               std::chrono::steady_clock::time_point now,
                                               std::chrono::steady_clock::duration interval)
{
  if (!manifestRouteKnown || lastFileServerAnswerAt == std::chrono::steady_clock::time_point{})
    return true;
  return now - lastFileServerAnswerAt >= interval;
}

} // namespace dispatcharr
