#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace dispatcharr
{

// Shared by DispatcharrClient::ReadInProgressRecordingStream() and
// ReadLiveTimeshiftStream()'s catch-up-to-tail wait, after the two
// diverged once already: the live path got hardened against a confirmed
// real crash (a lone segment's own duration is too noisy a sample to
// size a retry budget off of -- a real instance produced one just 151ms
// long against a configured target, which collapsed the old
// single-segment/1.5x-margin budget until ffmpeg's demuxer read the
// resulting stall as genuine end-of-stream and closed playback outright,
// confirmed live via VideoPlayer: eof immediately followed by Kodi
// kicking back to the main menu -- see docs/TIMESHIFT.md's "Real
// hardware (CoreELEC/ODROID N2+)" section), but the in-progress-recording
// path never received the same fix since there was nothing shared to fix
// once. Sharing the calculation here is specifically so that can't
// happen again -- also pulled out into its own header (a template, so it
// stays header-only) specifically so it's unit-testable standalone with
// a minimal synthetic segment type; see ../tests/test_catch_up_util.cpp.
constexpr size_t kSegmentDurationSampleCount = 5;
constexpr int64_t kMinSegmentDurationMs = 1500;

// Averaged over the last few segments, not just the single most recent
// one, with a floor under the average for a fresh buffer's first few
// still-warming-up segments -- see the comment above for why both
// matter. SegmentT only needs a `timeOffsetMs` member (duck-typed via
// the template, works for both LiveTimeshiftSegmentInfo and
// InProgressRecordingSegmentInfo without depending on either).
template <typename SegmentT>
int64_t EstimateSegmentDurationMs(int64_t totalDurationMs, const std::vector<SegmentT>& segments)
{
  int64_t avgSegmentDurationMs = 6000;
  size_t sampleCount = std::min(kSegmentDurationSampleCount, segments.size());
  if (sampleCount > 0)
  {
    int64_t sumMs = totalDurationMs - segments[segments.size() - sampleCount].timeOffsetMs;
    if (sumMs > 0)
      avgSegmentDurationMs = sumMs / static_cast<int64_t>(sampleCount);
  }
  return std::max(avgSegmentDurationMs, kMinSegmentDurationMs);
}

// 3x margin, not 1.5x: confirmed live against a real instance that 1.5x
// runs far thinner than intended in practice -- ordinary, non-error
// catch-up cycles were routinely using 60-95% of that budget just to
// catch up under normal jitter, not just in a genuine outage. A likely
// seek probe (see each caller's own comment on that distinction) always
// gets just one attempt, regardless of margin, so probing stays fast
// rather than paying a full segment-duration wait for something that
// doesn't need it.
inline int ComputeCatchUpAttempts(bool likelySeekProbe, int64_t segmentDurationEstimateMs, int catchUpSleepMs)
{
  if (likelySeekProbe)
    return 1;
  return static_cast<int>((segmentDurationEstimateMs * 3) / catchUpSleepMs) + 1;
}

} // namespace dispatcharr
