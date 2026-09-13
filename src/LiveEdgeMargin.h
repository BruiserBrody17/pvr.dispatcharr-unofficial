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

} // namespace dispatcharr
