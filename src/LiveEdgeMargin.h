#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dispatcharr
{

// Shared by DispatcharrClient::SeekInProgressRecordingStream() (with
// marginSegments=1) and SeekLiveTimeshiftStream() (with
// marginSegments=kLiveEdgeSeekBackoffSegments, currently 3) -- computes
// the clamp target for a forward/seek-to-live-edge seek, backed off from
// the true tail (`totalBytes`) by the combined byteSize of the trailing
// `marginSegments` segments (or fewer, if there aren't that many yet).
//
// The backoff itself is real, confirmed-live-incident-driven behavior,
// not an arbitrary margin: landing a seek exactly at the tail leaves zero
// read-ahead, so playback immediately re-catches-up to the (still-)tail
// and pays a full segment-production cycle's wait a second time right
// after what looked like a completed seek -- see each caller's own
// comment for the specific incidents this fixed (a "skip ahead to live
// took ~10s" investigation, and separately a live-edge-only H.264
// decode-error/audio-desync storm documented in docs/TIMESHIFT.md's
// "Packet corrupt" section).
//
// SegmentT only needs a `byteSize` member (duck-typed via the template,
// works for both LiveTimeshiftSegmentInfo and
// InProgressRecordingSegmentInfo without depending on either) -- pulled
// out here specifically so it's unit-testable standalone; see
// ../tests/test_live_edge_margin.cpp.
template <typename SegmentT>
int64_t ComputeLiveEdgeTailTarget(const std::vector<SegmentT>& segments, int64_t totalBytes, size_t marginSegments)
{
  size_t backoffCount = std::min(marginSegments, segments.size());
  int64_t backoffBytes = 0;
  for (size_t i = segments.size() - backoffCount; i < segments.size(); ++i)
    backoffBytes += segments[i].byteSize;
  return std::max<int64_t>(0, totalBytes - backoffBytes);
}

// Where a seek to `requested` lands, given the backed-off `tailTarget` and the
// stream's current read position (`current`).
//
// A target past `tailTarget` used to be clamped straight down to it, which put a
// viewer already sitting past it -- any reader waiting at the tail is, since the
// margin is a segment or more -- *behind* where they already were. Confirmed live
// (docs/RECORDINGS.md, "Two live checks of the in-progress read path"): forward
// steps at the tail of an in-progress recording moved the read position back by
// 262,144 to 1,310,720 bytes, and a "skip forward" with less than that left read
// as nothing happening. Bounded by construction (the reader can be no further
// ahead of `tailTarget` than the margin), but there is no reason to move
// backward on a request that was not one.
//
// So: a target at or before `tailTarget` is untouched; one past it lands on
// `tailTarget`, or on the current position when that is already further along
// (or on the requested target itself when that is behind `current`, which makes
// a short backward seek inside the margin stay where it was asked to). That adds
// no exposure to the live-edge decode errors the margin guards against -- the
// reader is already there -- and never lands past `requested`.
inline int64_t ClampSeekToTail(int64_t requested, int64_t current, int64_t tailTarget)
{
  if (requested <= tailTarget)
    return requested;
  return std::max(tailTarget, std::min(current, requested));
}

// Where a live-timeshift seek actually lands, once a non-negative target has been
// resolved: first held back from the live edge (ClampSeekToTail() against
// `tailTarget`), then -- afterwards, so it wins when the two disagree -- moved up
// to `firstAvailable`, the start of the oldest segment the plugin's rolling
// buffer still holds (FirstAvailableLiveSegmentIndex()). Head over tail is
// deliberate: a position behind the window reads a recycled file, which the
// Content-Range cross-check then ends the whole stream over (found live), while
// a position past the tail margin only costs a short wait. Both flags are
// reported for the caller's log line.
struct LiveSeekResolution
{
  int64_t position = 0;
  bool clampedToTail = false;
  bool clampedToHead = false;
};

inline LiveSeekResolution ResolveLiveSeekTarget(int64_t requested, int64_t current, int64_t tailTarget,
                                                int64_t firstAvailable)
{
  LiveSeekResolution r;
  r.position = requested;
  r.clampedToTail = requested > tailTarget;
  if (r.clampedToTail)
    r.position = ClampSeekToTail(requested, current, tailTarget);
  r.clampedToHead = r.position < firstAvailable;
  if (r.clampedToHead)
    r.position = firstAvailable;
  return r;
}

// Whether a live-timeshift read should stop rather than wait: the plugin has said the
// buffer's ffmpeg has exited (`ended` -- no further segment will ever be appended) and
// the reader has consumed everything that was ever going to exist. A reader still
// behind the tail is not affected, which is the point: a viewer paused or rewound
// well behind live keeps their buffered window after ffmpeg dies, instead of the
// whole buffer being torn down at the first poll (docs/OPEN_ITEMS.md, "Dead-buffer
// detection destroys a paused/rewound viewer's rewind window").
inline bool IsAtEndedTail(bool ended, int64_t position, int64_t totalBytes)
{
  return ended && position >= totalBytes;
}

// Index of the first of this stream's own segments that the server-side
// rolling buffer still lists, given the lowest sequence its latest manifest
// carries (`oldestAvailableSequence`, see OldestLiveManifestSequence()) -- 0
// when that isn't known yet (-1) or nothing has rolled off. Everything before
// it is history the plugin has already let go of: ffmpeg's hls muxer deletes
// those files a while after they leave the window (before timeshift_buffer
// 0.8.0 the segment muxer's -segment_wrap recycled them instead, and a
// recycled file held unrelated newer content, which ReadLiveTimeshiftStream()'s
// own size cross-check then (correctly) treated as fatal). Segments are in
// ascending sequence order, so this is a lower bound.
//
// SegmentT needs a `sequence` member.
template <typename SegmentT>
size_t FirstAvailableLiveSegmentIndex(const std::vector<SegmentT>& segments, int64_t oldestAvailableSequence)
{
  if (oldestAvailableSequence < 0)
    return 0;
  const auto it = std::lower_bound(segments.begin(), segments.end(), oldestAvailableSequence,
                                   [](const SegmentT& seg, int64_t sequence) { return seg.sequence < sequence; });
  return static_cast<size_t>(it - segments.begin());
}

// Drops the leading segments the plugin's rolling buffer has let go of
// (everything below FirstAvailableLiveSegmentIndex()) from the local list, and
// returns how many it dropped. Without this the list only ever grew -- one entry
// per segment for as long as a session stayed open, 43,200 a day at the default
// 2 s segments -- and every Read() scanned it from the front
// (FindSegmentContainingPosition()); found by the 2026-10-04 hardening sweep.
//
// Safe because byteOffset/timeOffsetMs are absolute positions in this stream's own
// address space, so the surviving entries mean exactly what they did. Reads and
// seeks already clamp past the rolled-off region (ResolveLiveSeekTarget(), and
// ReadLiveTimeshiftStream() moves a stale position up to the first available
// byte), so nothing looks a dropped segment up. The LAST segment is always kept,
// even if the oldest-available sequence has overtaken it, because its sequence is
// what the next manifest merge reads as "already known": dropping every entry
// would make the next refresh append the whole window again.
//
// SegmentT needs a `sequence` member.
template <typename SegmentT>
size_t PruneRolledOffLiveSegments(std::vector<SegmentT>& segments, int64_t oldestAvailableSequence)
{
  if (segments.size() < 2)
    return 0;
  const size_t firstAvailable = FirstAvailableLiveSegmentIndex(segments, oldestAvailableSequence);
  const size_t drop = std::min(firstAvailable, segments.size() - 1);
  if (drop == 0)
    return 0;
  segments.erase(segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(drop));
  return drop;
}

// DispatcharrClient::OpenLiveTimeshiftStream()'s cold-start trim --
// discards every segment except the trailing `marginSegments` worth,
// rebasing the kept ones (and totalBytes/totalDurationMs) so the oldest
// surviving segment becomes local byte/time 0. A no-op when there
// aren't more than `marginSegments` segments yet.
//
// **Confirmed live** this trim is necessary even for a buffer that was
// never stopped/restarted: reattaching to a channel whose buffer had
// been running a while, then seeking into the *older* part of that
// history, reproduced a `CDVDDemuxFFmpeg::SeekTime` landing on a
// garbage time near the MPEG-TS 33-bit PTS wraparound point (~26.5h)
// instead of anywhere near the requested target, from a plain -20s
// relative seek -- see OpenLiveTimeshiftStream()'s own comment and
// docs/TIMESHIFT.md's "Concurrent viewers" section for the full
// account, including why this also rules out letting a viewer join an
// already-running buffer and rewind into pre-join history.
//
// SegmentT needs byteOffset/timeOffsetMs/byteSize members (only
// LiveTimeshiftSegmentInfo in practice, since InProgressRecordingStream
// has no equivalent trim -- a recording, unlike a rolling live buffer,
// is meant to be kept in full -- but templated the same way as
// ComputeLiveEdgeTailTarget() above for consistency and standalone
// testability). `marginSegments` must be > 0 -- the only real caller
// always passes a fixed positive constant (kLiveEdgeMarginSegments, 3);
// a margin of 0 would need to rebase relative to a one-past-the-end
// segment that doesn't exist.
template <typename SegmentT>
void TrimToTrailingLiveEdgeMargin(std::vector<SegmentT>& segments, int64_t& totalBytes, int64_t& totalDurationMs,
                                  size_t marginSegments)
{
  if (segments.size() <= marginSegments)
    return;
  // A margin of nothing keeps nothing (the caller never asks: see above). Without this, `segments[dropCount]` below
  // read one past the end.
  if (marginSegments == 0)
  {
    segments.clear();
    totalBytes = 0;
    totalDurationMs = 0;
    return;
  }

  size_t dropCount = segments.size() - marginSegments;
  int64_t byteBase = segments[dropCount].byteOffset;
  int64_t timeBase = segments[dropCount].timeOffsetMs;
  segments.erase(segments.begin(), segments.begin() + dropCount);
  for (auto& seg : segments)
  {
    seg.byteOffset -= byteBase;
    seg.timeOffsetMs -= timeBase;
  }
  totalBytes -= byteBase;
  totalDurationMs -= timeBase;
}

// DispatcharrClient::OpenLiveTimeshiftStream()'s own live-edge starting
// position: a few segments *behind* the true tail (totalBytes), not
// exactly at it -- see that caller's own comment for why (ffmpeg only
// exposes a segment once it's fully closed, so sitting exactly at the tail
// means there's nothing to read until the next segment closes; confirmed
// live this produced a periodic "stream stalled" rebuffer cycle tracking
// segment_seconds almost exactly). Falls back to the true tail (0 margin)
// if fewer than marginSegments segments exist yet, e.g. right after a cold
// StartTimeshiftBuffer(). Unlike ComputeLiveEdgeTailTarget() above (which
// backs off by the trailing segments' combined *byteSize*), this picks a
// segment by *index* (marginSegments back from the end) and returns that
// segment's own byteOffset -- the real caller always calls this
// immediately after TrimToTrailingLiveEdgeMargin() above, at which point
// segments.size() <= marginSegments always holds and this degenerates to
// the trimmed window's own start (0), but the formula itself doesn't
// assume that.
//
// SegmentT only needs a `byteOffset` member (duck-typed, same convention
// as the two templates above).
template <typename SegmentT>
int64_t ComputeLiveEdgeStartPosition(const std::vector<SegmentT>& segments, int64_t totalBytes, size_t marginSegments)
{
  size_t segmentCount = segments.size();
  size_t marginIndex = segmentCount > marginSegments ? segmentCount - marginSegments : 0;
  return marginIndex < segmentCount ? segments[marginIndex].byteOffset : totalBytes;
}

} // namespace dispatcharr
