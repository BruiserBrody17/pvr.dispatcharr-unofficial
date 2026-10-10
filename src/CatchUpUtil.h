#pragma once

#include <algorithm>
#include <chrono>
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
// Upper bound on the estimate itself, added 2026-09-26 (a 39th-pass
// audit, fixing a real, confirmed gap found via a project-wide review,
// not itself independently reproduced): with no ceiling, one outlier
// segment among the last kSegmentDurationSampleCount can dominate the
// average and inflate ComputeCatchUpAttempts()'s own blocking-read
// budget from seconds to hours (the two known-live sources of an
// outlier this large -- M3u8SegmentParser.cpp's/LiveManifestParser.cpp's
// own UB/overflow clamps -- still allow a single segment up to a full
// day: at worst, a lone 86400s segment among 5 near-zero ones averages
// to ~4.8 hours, which the 3x-margin formula below then turns into a
// ~14.4-hour blocking Read() call). A genuine plausible trigger, not
// just a defensive guard against malformed input: Dispatcharr's own DVR
// ffmpeg command (`_dvr_build_ffmpeg_cmd()`, apps/channels/tasks.py,
// confirmed against its own real current upstream source) uses
// `-fflags +genpts`/`-avoid_negative_ts make_zero`, neither of which
// removes a mid-stream PTS discontinuity from a genuinely glitchy IPTV
// source -- and the HLS muxer derives each segment's own #EXTINF/
// duration_ms from PTS deltas, so a discontinuity is a real way to
// produce one wildly-oversized segment duration. 60 seconds is
// deliberately generous (a real segment is a handful of seconds at
// most, confirmed throughout this codebase/docs/TIMESHIFT.md) while
// still bounding the worst case to a few minutes instead of many hours
// -- pressing Stop mid-block otherwise made Kodi itself appear frozen on
// that stream for as long as the inflated budget lasted.
constexpr int64_t kMaxSegmentDurationMs = 60000;

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
  return std::min(std::max(avgSegmentDurationMs, kMinSegmentDurationMs), kMaxSegmentDurationMs);
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

// The wall-clock ceiling on one blocking catch-up wait. ComputeCatchUpAttempts() bounds the loop by
// an attempt count alone, but each attempt makes a forced refresh that can itself take a whole
// request timeout (30 s by default, two requests per attempt for an in-progress recording): with the
// server unreachable in a way that times out instead of refusing (a VPN drop, a firewall DROP) one
// Read() could block 25 to 50 minutes, and Kodi's Stop does not take effect until a blocking Read
// returns (found by the 2026-10-04 fifth hardening sweep). Twice the attempts' own sleep time leaves
// room for the refreshes' round trips in the normal case (a 4 s segment: 24 s against 12 s of
// sleeping), and the 120 s cap bounds a stream with unusually long segments.
constexpr int64_t kMaxCatchUpWallClockMs = 120000;

inline int64_t ComputeCatchUpWallClockBudgetMs(int attempts, int catchUpSleepMs)
{
  const int64_t budget = static_cast<int64_t>(attempts > 0 ? attempts : 0) * catchUpSleepMs * 2;
  return budget < kMaxCatchUpWallClockMs ? budget : kMaxCatchUpWallClockMs;
}

// Checked after each attempt's refresh: whether the wait has used up its wall-clock budget. The
// attempt in flight when it expires still finishes (a request cannot be cut short from here), so the
// worst case is the budget plus one attempt's own requests.
inline bool HasCatchUpBudgetElapsed(std::chrono::milliseconds elapsed, int64_t budgetMs)
{
  return elapsed.count() >= budgetMs;
}

// Also shared by ReadInProgressRecordingStream()/ReadLiveTimeshiftStream():
// a read landing at the tail shortly after a seek is far more likely to be
// one of ffmpeg's own internal seek probes (several in quick succession,
// one commonly overshooting right up to the current tail while estimating)
// than a genuine "caught up, please wait" read -- confirmed live that
// blocking a probe for the full segment-duration budget was the single
// largest contributor to measured seek latency. A zero `lastSeekTime`
// (nothing ever seeked) never counts as a probe. If a read at this exact
// position already gave up quickly last time (lastShortGiveUpPosition),
// it's no longer a fresh probe candidate -- escalate to the full budget
// instead of repeating the short one indefinitely against the same stuck
// position; confirmed live that giving every tail-adjacent read the short
// budget caused visible playback pauses right after a seek that landed
// near live.
inline bool IsLikelySeekProbe(int64_t position, int64_t lastShortGiveUpPosition,
                              std::chrono::steady_clock::time_point lastSeekTime,
                              std::chrono::steady_clock::time_point now,
                              std::chrono::steady_clock::duration seekProbeWindow)
{
  if (position == lastShortGiveUpPosition)
    return false;
  if (lastSeekTime.time_since_epoch().count() == 0)
    return false;
  return now - lastSeekTime < seekProbeWindow;
}

// The lastShortGiveUpPosition update rule applied once a catch-up loop
// finishes: remembers this position only when it was a likely seek probe
// that still didn't catch up, so a later read at that same position
// escalates to the full budget per IsLikelySeekProbe()'s own comment above.
// Any other outcome (caught up, or a genuine non-probe catch-up that gave
// up) clears it back to the sentinel -1.
inline int64_t ComputeShortGiveUpPosition(bool likelySeekProbe, bool caughtUp, int64_t position)
{
  return (likelySeekProbe && !caughtUp) ? position : -1;
}

// ---- The live tail wait in slices (a read parked at the live edge hands control back about once a second) ----
//
// ReadLiveTimeshiftStream() used to wait for the next segment inside ONE read call, up to the whole catch-up budget
// (three segment durations, 4 to 6 s measured on three devices). Kodi acts on a close request only between reads
// (its demuxer checks its abort flag before each call into the addon, DVDDemuxFFmpeg's dvd_file_read), so a stop or a
// channel change waited out the rest of the read, and a Player.Open sent in that window was dropped. The wait now
// ends after kLiveTailWaitSliceMs with -1, which the demuxer retries (a 0 would end playback; only a budget that is
// really used up still returns it), and the budget is counted across the calls of one episode at the same position.
constexpr int64_t kLiveTailWaitSliceMs = 1000;

// A read at the same position that arrives later than this after the previous one is a new episode: Kodi retries a -1
// at once, so a longer gap means playback was paused or the reader went away, and the old start must not count.
constexpr int64_t kTailWaitEpisodeGapMs = 3000;

inline bool HasTailWaitSliceElapsed(std::chrono::milliseconds sinceThisCallStarted)
{
  return sinceThisCallStarted.count() >= kLiveTailWaitSliceMs;
}

// Where the current wait at the live tail began, so the budget survives a -1 return. Enter() starts a new episode when
// the position changed (a seek, or data was read) or the previous call is too old.
class TailWaitEpisode
{
public:
  // Records a read arriving at the tail at `position`; true when it started a new episode.
  bool Enter(int64_t position, std::chrono::steady_clock::time_point now)
  {
    const bool fresh =
        m_position != position || m_lastCall.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastCall).count() > kTailWaitEpisodeGapMs;
    if (fresh)
    {
      m_position = position;
      m_start = now;
    }
    m_lastCall = now;
    return fresh;
  }
  // Called when a call returns -1 to retry: the next call continues this episode.
  void Touch(std::chrono::steady_clock::time_point now)
  {
    m_lastCall = now;
  }
  void Reset()
  {
    *this = TailWaitEpisode();
  }
  int64_t WaitedMs(std::chrono::steady_clock::time_point now) const
  {
    return m_position < 0 ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start).count();
  }

private:
  int64_t m_position = -1;
  std::chrono::steady_clock::time_point m_start{};
  std::chrono::steady_clock::time_point m_lastCall{};
};

enum class TailWaitOutcome
{
  kRetry,  // nothing yet and the budget is not used up: return -1, Kodi retries
  kGiveUp, // the budget is used up (or this was a seek probe, which never waits): return 0 as before
};

inline TailWaitOutcome DecideTailWaitOutcome(bool likelySeekProbe, int64_t waitedMs, int64_t budgetMs)
{
  return (likelySeekProbe || waitedMs >= budgetMs) ? TailWaitOutcome::kGiveUp : TailWaitOutcome::kRetry;
}

} // namespace dispatcharr
