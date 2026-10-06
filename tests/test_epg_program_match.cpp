#include "EpgProgramMatch.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
EpgEntry MakeEntry(time_t start, time_t end)
{
  EpgEntry entry;
  entry.startTime = start;
  entry.endTime = end;
  return entry;
}
} // namespace

TEST_CASE("FindEpgEntryIndexCoveringRecording matches an entry that fully covers the recording", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == 0);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording matches the real bug this fixes -- a recording clamped to start "
          "late into an already-airing programme",
          "[EpgProgramMatch]")
{
  // Dispatcharr's own RecordingSerializer clamps a past start_time to
  // now -- the programme itself really started at 1000 and runs to
  // 2000, but the user pressed Record at 1500 (more than Kodi's own
  // 2-minute fallback window into the programme), so recStart is 1500,
  // not 1000. The 80%-of-recording-duration overlap ratio is still 1.0
  // here (the whole clamped recording is inside the programme), so this
  // must still match.
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1500, 2000) == 0);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording returns -1 for a recording spanning multiple programmes with no "
          "single dominant one",
          "[EpgProgramMatch]")
{
  // Two entries, each covering exactly half of the recording -- neither
  // reaches the 80% threshold. Matches Dispatcharr's own "Custom
  // Recording" fallback for this exact case.
  std::vector<EpgEntry> entries = {MakeEntry(1000, 1500), MakeEntry(1500, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == -1);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording picks the entry with the largest overlap when several exist",
          "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {
      MakeEntry(1000, 1100), // tiny sliver of overlap only
      MakeEntry(1000, 2000), // covers the whole recording
  };
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == 1);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording ignores an entry with no overlap at all", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(0, 500)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == -1);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording returns -1 just below the 80% threshold", "[EpgProgramMatch]")
{
  // Recording is 1000-2000 (1000s); entry covers 1000-1790 (790s, 79%).
  std::vector<EpgEntry> entries = {MakeEntry(1000, 1790)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == -1);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording is true exactly at the 80% threshold", "[EpgProgramMatch]")
{
  // Recording is 1000-2000 (1000s); entry covers 1000-1800 (800s, exactly 80%).
  std::vector<EpgEntry> entries = {MakeEntry(1000, 1800)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == 0);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording returns -1 for a zero or negative duration recording",
          "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1500, 1500) == -1);
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1500, 1400) == -1);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording returns -1 for an empty entry list", "[EpgProgramMatch]")
{
  CHECK(FindEpgEntryIndexCoveringRecording({}, 1000, 2000) == -1);
}

TEST_CASE("FindEpgEntryIndexCoveringRecording fails a padded, late-started recording -- the real gap "
          "FindEpgEntryIndexByStartTime exists to cover",
          "[EpgProgramMatch]")
{
  // The programme runs 1000-2000 (1000s); the user pressed Record at
  // 1900 (clamped recStart), and 5 minutes (300s) of post-padding pushes
  // recEnd to 2300. Overlap is only 2000-1900=100s out of a 400s
  // recording -- 25%, nowhere near the 80% threshold, even though the
  // recording genuinely covers the entire remainder of the programme.
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1900, 2300) == -1);
}

// ---------------------------------------------------------------------
// FindEpgEntryIndexByStartTime
// ---------------------------------------------------------------------

TEST_CASE("FindEpgEntryIndexByStartTime matches an exact start time", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, /*toleranceSeconds=*/5) == 0);
}

TEST_CASE("FindEpgEntryIndexByStartTime matches the exact same padded, late-started recording "
          "FindEpgEntryIndexCoveringRecording fails on",
          "[EpgProgramMatch]")
{
  // Recording::programStartTime carries the true, never-padded programme
  // start (1000) regardless of how the recording's own [recStart, recEnd)
  // window was clamped/padded.
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, 5) == 0);
}

TEST_CASE("FindEpgEntryIndexByStartTime tolerates a small drift between the two EPG sources", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1002, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, /*toleranceSeconds=*/5) == 0);
}

TEST_CASE("FindEpgEntryIndexByStartTime returns -1 once the drift exceeds the tolerance", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1010, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, /*toleranceSeconds=*/5) == -1);
}

TEST_CASE("FindEpgEntryIndexByStartTime picks the closest match when several are within tolerance", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1004, 2000), MakeEntry(1001, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, /*toleranceSeconds=*/10) == 1);
}

TEST_CASE("FindEpgEntryIndexByStartTime returns -1 for an empty entry list", "[EpgProgramMatch]")
{
  CHECK(FindEpgEntryIndexByStartTime({}, 1000, 5) == -1);
}

// ---------------------------------------------------------------------
// ResolveEpgOverlapWindow
// ---------------------------------------------------------------------

TEST_CASE("ResolveEpgOverlapWindow prefers the true programme window when both times are valid", "[EpgProgramMatch]")
{
  EpgOverlapWindow window = ResolveEpgOverlapWindow(/*recStartTime=*/1900, /*recEndTime=*/2300,
                                                    /*programStartTime=*/1000, /*programEndTime=*/2000);
  CHECK(window.start == 1000);
  CHECK(window.end == 2000);
}

TEST_CASE("ResolveEpgOverlapWindow falls back to the recording's own window when programStartTime is 0",
          "[EpgProgramMatch]")
{
  EpgOverlapWindow window = ResolveEpgOverlapWindow(/*recStartTime=*/1900, /*recEndTime=*/2300, /*programStartTime=*/0,
                                                    /*programEndTime=*/0);
  CHECK(window.start == 1900);
  CHECK(window.end == 2300);
}

TEST_CASE("ResolveEpgOverlapWindow falls back to the recording's own window when programEndTime doesn't "
          "exceed programStartTime -- the real bug this fixes (all-or-nothing, never a mismatched pairing)",
          "[EpgProgramMatch]")
{
  EpgOverlapWindow zeroEnd = ResolveEpgOverlapWindow(1900, 2300, /*programStartTime=*/1000, /*programEndTime=*/0);
  CHECK(zeroEnd.start == 1900);
  CHECK(zeroEnd.end == 2300);

  EpgOverlapWindow backwards = ResolveEpgOverlapWindow(1900, 2300, /*programStartTime=*/1000,
                                                       /*programEndTime=*/999);
  CHECK(backwards.start == 1900);
  CHECK(backwards.end == 2300);
}

TEST_CASE("ResolveEpgOverlapWindow matches the recording's own window for a recurring-rule occurrence, where "
          "the two are identical",
          "[EpgProgramMatch]")
{
  // Confirmed against Dispatcharr's own real current upstream source:
  // a recurring-rule occurrence's own program.start_time/end_time are
  // never separately padded, so they're always exactly equal to the
  // recording's own top-level start_time/end_time -- preferring the
  // "programme window" here is a no-op for this case.
  EpgOverlapWindow window = ResolveEpgOverlapWindow(1000, 2000, 1000, 2000);
  CHECK(window.start == 1000);
  CHECK(window.end == 2000);
}

// ---------------------------------------------------------------------
// Exact boundaries (the 2026-10-04 fifth hardening sweep's mutation survivors)
// ---------------------------------------------------------------------

TEST_CASE("FindEpgEntryIndexByStartTime matches a start exactly at the tolerance but not one second past it",
          "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1060, 60) == 0);
  CHECK(FindEpgEntryIndexByStartTime(entries, 940, 60) == 0);
  CHECK(FindEpgEntryIndexByStartTime(entries, 1061, 60) == -1);
  CHECK(FindEpgEntryIndexByStartTime(entries, 939, 60) == -1);
}

TEST_CASE("FindEpgEntryIndexByStartTime picks the nearest entry, and the first of two equally near ones",
          "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1000, 2000), MakeEntry(1040, 2040), MakeEntry(1100, 2100)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1030, 60) == 1);
  // 1020 is 20 from the first and 20 from the second: the first wins the tie.
  CHECK(FindEpgEntryIndexByStartTime(entries, 1020, 60) == 0);
}

TEST_CASE("ResolveEpgOverlapWindow falls back to the recording window for an empty or backwards programme window",
          "[EpgProgramMatch]")
{
  CHECK(ResolveEpgOverlapWindow(100, 900, 500, 500).start == 100);
  CHECK(ResolveEpgOverlapWindow(100, 900, 500, 500).end == 900);
  CHECK(ResolveEpgOverlapWindow(100, 900, 500, 400).start == 100);
  CHECK(ResolveEpgOverlapWindow(100, 900, 0, 600).start == 100);
  CHECK(ResolveEpgOverlapWindow(100, 900, 500, 501).start == 500);
  CHECK(ResolveEpgOverlapWindow(100, 900, 500, 501).end == 501);
}

TEST_CASE("the matchers look past entries that do not match to ones further down the guide", "[EpgProgramMatch]")
{
  // A channel's guide is a whole day of programmes and the match is rarely the first entry: stopping
  // at the first non-matching entry would unlink almost every timer and recording from its guide.
  std::vector<EpgEntry> entries = {MakeEntry(0, 500), MakeEntry(500, 1000), MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(entries, 1000, 2000) == 2);
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, 30) == 2);
  // Out of tolerance first, in tolerance later.
  std::vector<EpgEntry> far = {MakeEntry(0, 100), MakeEntry(1010, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(far, 1000, 30) == 1);
  // And an overlapping entry after a non-overlapping one that comes first in the list.
  std::vector<EpgEntry> later = {MakeEntry(5000, 6000), MakeEntry(0, 500), MakeEntry(1000, 2000)};
  CHECK(FindEpgEntryIndexCoveringRecording(later, 1000, 2000) == 2);
}

TEST_CASE("FindEpgEntryIndexByStartTime accepts an entry exactly the tolerance away", "[EpgProgramMatch]")
{
  std::vector<EpgEntry> entries = {MakeEntry(1030, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, 30) == 0);
  CHECK(FindEpgEntryIndexByStartTime(entries, 1000, 29) == -1);
  std::vector<EpgEntry> before = {MakeEntry(970, 2000)};
  CHECK(FindEpgEntryIndexByStartTime(before, 1000, 30) == 0);
}
