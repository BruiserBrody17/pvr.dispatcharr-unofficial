#pragma once

#include <chrono>
#include <cstdint>

namespace dispatcharr
{

// The pure decision core of DispatcharrClient::MaybeSendInProgressHlsKeepAlive()
// (DispatcharrClient.cpp) -- found live 2026-09-30 while confirming
// docs/OPEN_ITEMS.md's own 28th-pass entry on this: pause an in-progress
// recording's playback, let the recording end while paused, resume, and
// playback dies (the player ended by itself 17s after resume, after 63
// consecutive segment 404s at ~7.8/s).
//
// Why it happens: once a recording finishes, Dispatcharr keeps its HLS
// directory only as long as its own `dvr:hls_viewer:{id}` Redis key exists
// (apps/channels/tasks.py's post-recording wait loop), and that key is set
// with a 20s TTL by the HLS view for a `.ts` request and nothing else
// (apps/channels/api_views.py) -- a playlist fetch, a status lookup, or a
// probe of a segment that doesn't exist yet all leave it alone. A paused
// viewer's own background traffic (the GetStreamTimes() polling that keeps
// running through a pause) is exactly that: nothing touches the key, so it
// lapses ~20s after the last real `.ts` request and the directory is
// removed out from under a viewer that still holds unread segments.
//
// The fix is a periodic lightweight `.ts` request purely to refresh that
// key. DRF maps HEAD to the same view function a GET runs (confirmed
// against the same source), and ProbeSegmentByteSize() already HEADs
// segments successfully, so a HEAD does the job without downloading
// anything.
//
// kDispatcharrHlsViewerTtl mirrors that `ex=20`; the static_assert below
// keeps the interval at no more than half of it, so one failed attempt can
// still be retried before the key actually lapses.
constexpr std::chrono::seconds kDispatcharrHlsViewerTtl{20};
constexpr std::chrono::seconds kHlsKeepAliveInterval{10};
// How long to wait before retrying an attempt that failed transiently (a
// transport error, a 5xx, a 401 that the manifest refresh's own key
// self-heal is about to fix) -- short enough that several retries fit
// inside what's left of the TTL after a missed interval, long enough that a
// persistently failing server isn't hit on every ~1Hz GetStreamTimes() poll.
constexpr std::chrono::seconds kHlsKeepAliveRetryDelay{2};
static_assert(kHlsKeepAliveInterval * 2 <= kDispatcharrHlsViewerTtl,
              "the keep-alive interval must leave room for at least one retry inside the server's own TTL");
static_assert(kHlsKeepAliveRetryDelay < kHlsKeepAliveInterval,
              "a retry must come sooner than the next scheduled attempt");

// Whether a keep-alive request is due right now.
//
// `position >= totalBytes` (the reader has already been handed every byte
// this addon knows about) deliberately suppresses it -- the one
// non-obvious rule here, and load-bearing: Dispatcharr only finalizes a
// finished recording (removes the HLS directory, then flips its status to
// completed) after the viewer key expires, and this addon only learns a
// recording is finished (finished == true, hence EOF) from exactly that
// finalization. A reader sitting at the tail that also kept the key alive
// would therefore hold the recording open forever, waiting for an EOF its
// own keep-alive prevents. A viewer with nothing left unread has no use for
// the directory anyway; anything still unread is what the keep-alive exists
// to protect.
//
// `contentGone` covers both a recording already known to be finished and a
// keep-alive that itself already came back 404 -- either way there is
// nothing left to keep. A zero/default `nextDueAt` (never scheduled) counts
// as due, the same "never happened yet" sentinel convention as
// Staleness.h's own helpers.
inline bool ShouldSendHlsViewerKeepAlive(bool contentGone, int64_t position, int64_t totalBytes,
                                         std::chrono::steady_clock::time_point nextDueAt,
                                         std::chrono::steady_clock::time_point now)
{
  if (contentGone)
    return false;
  if (position >= totalBytes)
    return false;
  return now >= nextDueAt;
}

enum class HlsKeepAliveOutcome
{
  // The server handled the request, so the viewer key was refreshed.
  kRefreshed,
  // The HLS directory is already gone -- a 404 for the segment, or a
  // redirect (the same "directory is gone" signal a playlist request
  // gets, see FetchRawInProgressPlaylist()'s own header comment). No
  // amount of retrying brings it back.
  kGone,
  // Anything else: a transport failure, a 401, a 5xx, a 403. Worth
  // another attempt shortly, since none of these say the content itself
  // is gone.
  kRetryLater,
};

inline HlsKeepAliveOutcome ClassifyHlsKeepAliveResponse(bool transportOk, long httpStatus)
{
  if (!transportOk)
    return HlsKeepAliveOutcome::kRetryLater;
  if (httpStatus >= 200 && httpStatus < 300)
    return HlsKeepAliveOutcome::kRefreshed;
  if (httpStatus == 404 || (httpStatus >= 300 && httpStatus < 400))
    return HlsKeepAliveOutcome::kGone;
  return HlsKeepAliveOutcome::kRetryLater;
}

// When the next keep-alive falls due after `now`. Every request this
// addon makes that lands on a `.ts` URL (a segment body fetch, a newly
// discovered segment's size probe, the keep-alive itself) refreshes the
// server's viewer key just as much as a keep-alive would, so each of them
// pushes the schedule out from its own completion time -- a viewer that is
// actively playing never sends a keep-alive at all. Only a failed attempt
// gets the shorter retry delay.
inline std::chrono::steady_clock::time_point NextHlsKeepAliveDueAt(std::chrono::steady_clock::time_point now,
                                                                   HlsKeepAliveOutcome outcome)
{
  return now + (outcome == HlsKeepAliveOutcome::kRetryLater ? kHlsKeepAliveRetryDelay : kHlsKeepAliveInterval);
}

} // namespace dispatcharr
