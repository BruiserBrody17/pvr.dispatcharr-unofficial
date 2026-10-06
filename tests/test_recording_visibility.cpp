#include "RecordingVisibility.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr time_t kNow = 1767225600; // 2026-01-01T00:00:00Z

Recording MakeRecording(bool isInProgress, bool isUpcoming, bool hlsDirStillPresent = false)
{
  Recording rec;
  rec.isInProgress = isInProgress;
  rec.isUpcoming = isUpcoming;
  rec.hlsDirStillPresent = hlsDirStillPresent;
  return rec;
}
} // namespace

// ---------------------------------------------------------------------
// IsListedAsRecording
// ---------------------------------------------------------------------

TEST_CASE("IsListedAsRecording is true for a completed recording", "[RecordingVisibility]")
{
  CHECK(IsListedAsRecording(MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/false)));
}

TEST_CASE("IsListedAsRecording is true for an in-progress recording -- the real bug this guards against",
          "[RecordingVisibility]")
{
  // Omitting in-progress recordings here (an earlier version of this
  // addon) made them show up only as an uneditable timer entry, with
  // nothing to actually click and play.
  CHECK(IsListedAsRecording(MakeRecording(/*isInProgress=*/true, /*isUpcoming=*/false)));
}

TEST_CASE("IsListedAsRecording is false for a not-yet-started upcoming recording", "[RecordingVisibility]")
{
  CHECK_FALSE(IsListedAsRecording(MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/true)));
}

// ---------------------------------------------------------------------
// IsListedAsTimer
// ---------------------------------------------------------------------

TEST_CASE("IsListedAsTimer is true for an in-progress recording", "[RecordingVisibility]")
{
  CHECK(IsListedAsTimer(MakeRecording(/*isInProgress=*/true, /*isUpcoming=*/false), kNow));
}

TEST_CASE("IsListedAsTimer is true for an upcoming recording with no known end time yet", "[RecordingVisibility]")
{
  CHECK(IsListedAsTimer(MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/true), kNow));
}

TEST_CASE("IsListedAsTimer is true for an upcoming recording whose end time hasn't passed yet", "[RecordingVisibility]")
{
  Recording rec = MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/true);
  rec.endTime = kNow + 3600;
  CHECK(IsListedAsTimer(rec, kNow));
}

TEST_CASE("IsListedAsTimer is false for a completed recording -- surfaced via IsListedAsRecording instead",
          "[RecordingVisibility]")
{
  CHECK_FALSE(IsListedAsTimer(MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/false), kNow));
}

TEST_CASE("IsListedAsTimer is false for a missed occurrence stuck upcoming with a past end time -- the real bug "
          "this fixes",
          "[RecordingVisibility]")
{
  // A recurring occurrence Dispatcharr never got to run (e.g. down for
  // its whole window) stays status=="scheduled" (isUpcoming) forever,
  // with an end_time now in the past -- without this exclusion, it
  // showed as a perpetual SCHEDULED timer Kodi itself never expires.
  Recording rec = MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/true);
  rec.endTime = kNow - 86400;
  CHECK_FALSE(IsListedAsTimer(rec, kNow));
}

TEST_CASE("IsListedAsTimer is true for an in-progress recording even with a past end time", "[RecordingVisibility]")
{
  // isInProgress wins outright -- an actively-recording occurrence
  // running past its originally-scheduled end_time (see
  // ParseRecordingFields()'s own status-override handling) is not
  // "missed".
  Recording rec = MakeRecording(/*isInProgress=*/true, /*isUpcoming=*/false);
  rec.endTime = kNow - 86400;
  CHECK(IsListedAsTimer(rec, kNow));
}

TEST_CASE("IsListedAsRecording and IsListedAsTimer agree an in-progress recording belongs in both lists at once",
          "[RecordingVisibility]")
{
  // Deliberate, not a bug: this is the exact real behavior
  // IsListedAsRecording()'s own comment documents -- an in-progress
  // recording is both a still-active timer AND a clickable/playable
  // listed recording simultaneously.
  Recording rec = MakeRecording(/*isInProgress=*/true, /*isUpcoming=*/false);
  CHECK(IsListedAsRecording(rec));
  CHECK(IsListedAsTimer(rec, kNow));
}

TEST_CASE("IsListedAsRecording and IsListedAsTimer are mutually exclusive for a completed or upcoming recording",
          "[RecordingVisibility]")
{
  // Only the in-progress case (covered above) appears in both lists at
  // once -- every other real state belongs to exactly one.
  Recording completed = MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/false);
  CHECK(IsListedAsRecording(completed) != IsListedAsTimer(completed, kNow));

  Recording upcoming = MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/true);
  CHECK(IsListedAsRecording(upcoming) != IsListedAsTimer(upcoming, kNow));
}

// ---------------------------------------------------------------------
// IsMissedOccurrence
// ---------------------------------------------------------------------

TEST_CASE("IsMissedOccurrence is false when not upcoming at all", "[RecordingVisibility]")
{
  CHECK_FALSE(IsMissedOccurrence(/*isUpcoming=*/false, /*endTime=*/kNow - 86400, kNow));
}

TEST_CASE("IsMissedOccurrence is false when upcoming with no known end time (endTime <= 0)", "[RecordingVisibility]")
{
  CHECK_FALSE(IsMissedOccurrence(/*isUpcoming=*/true, /*endTime=*/0, kNow));
}

TEST_CASE("IsMissedOccurrence is false when upcoming and the end time hasn't passed yet", "[RecordingVisibility]")
{
  CHECK_FALSE(IsMissedOccurrence(/*isUpcoming=*/true, /*endTime=*/kNow + 1, kNow));
}

TEST_CASE("IsMissedOccurrence is true when upcoming and the end time has already passed", "[RecordingVisibility]")
{
  CHECK(IsMissedOccurrence(/*isUpcoming=*/true, /*endTime=*/kNow, kNow));
  CHECK(IsMissedOccurrence(/*isUpcoming=*/true, /*endTime=*/kNow - 86400, kNow));
}

// ---------------------------------------------------------------------
// ShouldUseGrowingBufferPlayback
// ---------------------------------------------------------------------

TEST_CASE("ShouldUseGrowingBufferPlayback is true while genuinely in progress", "[RecordingVisibility]")
{
  CHECK(ShouldUseGrowingBufferPlayback(
      MakeRecording(/*isInProgress=*/true, /*isUpcoming=*/false, /*hlsDirStillPresent=*/false)));
}

TEST_CASE("ShouldUseGrowingBufferPlayback stays true just after stop while the HLS dir still exists",
          "[RecordingVisibility]")
{
  // The exact real reported bug: isInProgress flips false immediately on
  // stop, well before Dispatcharr's HLS-to-MKV concat actually finishes.
  CHECK(ShouldUseGrowingBufferPlayback(
      MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/false, /*hlsDirStillPresent=*/true)));
}

TEST_CASE("ShouldUseGrowingBufferPlayback is false once the HLS dir is gone and it's not in progress",
          "[RecordingVisibility]")
{
  CHECK_FALSE(ShouldUseGrowingBufferPlayback(
      MakeRecording(/*isInProgress=*/false, /*isUpcoming=*/false, /*hlsDirStillPresent=*/false)));
}

// ---------------------------------------------------------------------
// ResolveInProgressFinished
// ---------------------------------------------------------------------

TEST_CASE("ResolveInProgressFinished trusts a successful lookup that says still in progress", "[RecordingVisibility]")
{
  CHECK_FALSE(ResolveInProgressFinished(/*lookupOk=*/true, /*isInProgress=*/true, /*previousFinished=*/false,
                                        /*allProbesSucceeded=*/true));
  // isInProgress=true wins regardless of this cycle's own probe outcome.
  CHECK_FALSE(ResolveInProgressFinished(/*lookupOk=*/true, /*isInProgress=*/true, /*previousFinished=*/false,
                                        /*allProbesSucceeded=*/false));
}

TEST_CASE("ResolveInProgressFinished trusts a successful lookup that says finished, when every probe succeeded",
          "[RecordingVisibility]")
{
  CHECK(ResolveInProgressFinished(/*lookupOk=*/true, /*isInProgress=*/false, /*previousFinished=*/false,
                                  /*allProbesSucceeded=*/true));
}

TEST_CASE("ResolveInProgressFinished stays not-finished when this cycle's own probe batch had a failure -- the "
          "real bug this fixes",
          "[RecordingVisibility]")
{
  // The recording's own final segment(s) can be discovered in the same
  // cycle isInProgress first flips false; if that segment's byte-size
  // probe itself failed this cycle, it isn't merged yet (see
  // CountLeadingProbedSegments()'s own comment) -- marking finished here
  // would strand it permanently, since ReadInProgressRecordingStream()
  // never refreshes again once finished is true and its read position
  // has already reached the (now permanently short) totalBytes.
  CHECK_FALSE(ResolveInProgressFinished(/*lookupOk=*/true, /*isInProgress=*/false, /*previousFinished=*/false,
                                        /*allProbesSucceeded=*/false));
}

TEST_CASE("ResolveInProgressFinished keeps the previous value on a failed lookup -- the real bug this fixes",
          "[RecordingVisibility]")
{
  // A transient network blip or auth hiccup must not end playback of a
  // recording that's actually still being written -- isInProgress here
  // is meaningless (the lookup itself failed) and must be ignored.
  CHECK_FALSE(ResolveInProgressFinished(/*lookupOk=*/false, /*isInProgress=*/false, /*previousFinished=*/false,
                                        /*allProbesSucceeded=*/true));
  CHECK_FALSE(ResolveInProgressFinished(/*lookupOk=*/false, /*isInProgress=*/true, /*previousFinished=*/false,
                                        /*allProbesSucceeded=*/true));
}

TEST_CASE("ResolveInProgressFinished does not un-finish a recording on a failed lookup either", "[RecordingVisibility]")
{
  CHECK(ResolveInProgressFinished(/*lookupOk=*/false, /*isInProgress=*/false, /*previousFinished=*/true,
                                  /*allProbesSucceeded=*/true));
  CHECK(ResolveInProgressFinished(/*lookupOk=*/false, /*isInProgress=*/true, /*previousFinished=*/true,
                                  /*allProbesSucceeded=*/true));
}

// ---------------------------------------------------------------------
// ShouldStopInsteadOfDelete
// ---------------------------------------------------------------------

TEST_CASE("ShouldStopInsteadOfDelete stops when Kodi says forceDelete, regardless of the server",
          "[RecordingVisibility]")
{
  CHECK(ShouldStopInsteadOfDelete(/*forceDelete=*/true, /*lookupOk=*/false, /*serverSaysInProgress=*/false));
  CHECK(ShouldStopInsteadOfDelete(/*forceDelete=*/true, /*lookupOk=*/true, /*serverSaysInProgress=*/false));
}

TEST_CASE("ShouldStopInsteadOfDelete stops when the server says in progress even if Kodi's own "
          "forceDelete says otherwise -- the real bug this fixes",
          "[RecordingVisibility]")
{
  // Kodi's own forceDelete=false is stale (its cached timer state hasn't
  // caught up to the recording actually having started) -- the fresh
  // server lookup must still win, erring toward not deleting a live file.
  CHECK(ShouldStopInsteadOfDelete(/*forceDelete=*/false, /*lookupOk=*/true, /*serverSaysInProgress=*/true));
}

TEST_CASE("ShouldStopInsteadOfDelete deletes when neither signal says in progress", "[RecordingVisibility]")
{
  CHECK_FALSE(ShouldStopInsteadOfDelete(/*forceDelete=*/false, /*lookupOk=*/true, /*serverSaysInProgress=*/false));
}

TEST_CASE("ShouldStopInsteadOfDelete falls back to forceDelete alone when the lookup fails", "[RecordingVisibility]")
{
  // A failed lookup contributes nothing either way (matches
  // ResolveInProgressFinished()'s own "unknown means don't assume"
  // principle) -- forceDelete is the only signal left.
  CHECK_FALSE(ShouldStopInsteadOfDelete(/*forceDelete=*/false, /*lookupOk=*/false, /*serverSaysInProgress=*/true));
}

// ---------------------------------------------------------------------
// IsAlreadyFinishedRecording
// ---------------------------------------------------------------------

TEST_CASE("IsAlreadyFinishedRecording is true once a fresh lookup confirms neither in-progress nor upcoming -- "
          "the real bug this fixes",
          "[RecordingVisibility]")
{
  // The exact scenario: a recording short enough to start and finish
  // inside one recording_refresh_minutes interval still shows as a
  // SCHEDULED timer to Kodi's own stale cache, even though the fresh
  // lookup confirms it already completed normally.
  CHECK(IsAlreadyFinishedRecording(/*lookupOk=*/true, /*isInProgress=*/false, /*isUpcoming=*/false));
}

TEST_CASE("IsAlreadyFinishedRecording is false while still in progress", "[RecordingVisibility]")
{
  CHECK_FALSE(IsAlreadyFinishedRecording(/*lookupOk=*/true, /*isInProgress=*/true, /*isUpcoming=*/false));
}

TEST_CASE("IsAlreadyFinishedRecording is false while still upcoming (a genuinely not-yet-started timer)",
          "[RecordingVisibility]")
{
  CHECK_FALSE(IsAlreadyFinishedRecording(/*lookupOk=*/true, /*isInProgress=*/false, /*isUpcoming=*/true));
}

TEST_CASE("IsAlreadyFinishedRecording is false when the lookup itself fails -- unknown means don't assume",
          "[RecordingVisibility]")
{
  CHECK_FALSE(IsAlreadyFinishedRecording(/*lookupOk=*/false, /*isInProgress=*/false, /*isUpcoming=*/false));
}

// ---------------------------------------------------------------------
// ClassifyOneTimeTimerEdit
// ---------------------------------------------------------------------

TEST_CASE("ClassifyOneTimeTimerEdit is Extend while genuinely in progress", "[RecordingVisibility]")
{
  CHECK(ClassifyOneTimeTimerEdit(/*isInProgress=*/true, /*isUpcoming=*/false, /*endTime=*/0, kNow) ==
        OneTimeTimerEditAction::Extend);
}

TEST_CASE("ClassifyOneTimeTimerEdit is Extend even with a past end time and isUpcoming both set -- in-progress "
          "wins over every other case",
          "[RecordingVisibility]")
{
  // Neither IsMissedOccurrence() nor IsAlreadyFinishedRecording() can be
  // true for a recording that's actually still in progress, but this
  // pins that ordering down explicitly with a test instead of relying
  // on it holding by construction -- exactly the kind of ordering
  // mistake two earlier passes each had to fix around.
  CHECK(ClassifyOneTimeTimerEdit(/*isInProgress=*/true, /*isUpcoming=*/true, /*endTime=*/kNow - 86400, kNow) ==
        OneTimeTimerEditAction::Extend);
}

TEST_CASE("ClassifyOneTimeTimerEdit is RenameOnly for a missed occurrence", "[RecordingVisibility]")
{
  CHECK(ClassifyOneTimeTimerEdit(/*isInProgress=*/false, /*isUpcoming=*/true, /*endTime=*/kNow - 86400, kNow) ==
        OneTimeTimerEditAction::RenameOnly);
}

TEST_CASE("ClassifyOneTimeTimerEdit is RenameOnly for an already-finished recording", "[RecordingVisibility]")
{
  CHECK(ClassifyOneTimeTimerEdit(/*isInProgress=*/false, /*isUpcoming=*/false, /*endTime=*/kNow - 86400, kNow) ==
        OneTimeTimerEditAction::RenameOnly);
}

TEST_CASE("ClassifyOneTimeTimerEdit is Reschedule for a genuinely not-yet-started recording", "[RecordingVisibility]")
{
  CHECK(ClassifyOneTimeTimerEdit(/*isInProgress=*/false, /*isUpcoming=*/true, /*endTime=*/kNow + 3600, kNow) ==
        OneTimeTimerEditAction::Reschedule);
}

TEST_CASE("ClassifyOneTimeTimerEdit is Reschedule for a not-yet-started recording with no known end time yet",
          "[RecordingVisibility]")
{
  CHECK(ClassifyOneTimeTimerEdit(/*isInProgress=*/false, /*isUpcoming=*/true, /*endTime=*/0, kNow) ==
        OneTimeTimerEditAction::Reschedule);
}

TEST_CASE("ShouldRetryInProgressColdStart waits out a 404 for a recording the server says is in progress",
          "[RecordingVisibility]")
{
  // The case reproduced live: an open within the first ~3s of a recording
  // hits a playlist that doesn't exist yet.
  CHECK(ShouldRetryInProgressColdStart(/*playlistHttpStatus=*/404, /*lookupOk=*/true, /*isInProgress=*/true));
}

TEST_CASE("ShouldRetryInProgressColdStart fails fast when the recording is not in progress", "[RecordingVisibility]")
{
  // A finished, failed or otherwise-terminal recording with no playlist
  // will never get one -- waiting the whole cold-start budget for it
  // would just make the failure slower.
  CHECK_FALSE(ShouldRetryInProgressColdStart(404, /*lookupOk=*/true, /*isInProgress=*/false));
}

TEST_CASE("ShouldRetryInProgressColdStart does not guess when the lookup itself failed", "[RecordingVisibility]")
{
  CHECK_FALSE(ShouldRetryInProgressColdStart(404, /*lookupOk=*/false, /*isInProgress=*/true));
  CHECK_FALSE(ShouldRetryInProgressColdStart(404, /*lookupOk=*/false, /*isInProgress=*/false));
}

TEST_CASE("ShouldRetryInProgressColdStart only treats a 404 as 'not created yet'", "[RecordingVisibility]")
{
  // A redirect means the HLS directory was already removed and the file is
  // complete; the rest say nothing about the recording being young.
  for (long status : {0L, 200L, 301L, 302L, 303L, 400L, 401L, 403L, 410L, 429L, 500L, 502L, 503L})
    CHECK_FALSE(ShouldRetryInProgressColdStart(status, /*lookupOk=*/true, /*isInProgress=*/true));
}

// ---------------------------------------------------------------------
// IsInProgressContentGone -- found live 2026-09-30: a recording deleted on
// the server while a viewer sat at its tail left the addon polling a
// recording that no longer existed (~7.8 requests/sec, all 404s) for ~100s.
// ---------------------------------------------------------------------

TEST_CASE("IsInProgressContentGone treats a deleted recording as gone -- the lookup and the playlist both 404",
          "[RecordingVisibility]")
{
  // The case reproduced live. A lookup that answered 404 is a definitive
  // answer, not the "unknown" a network blip is.
  CHECK(IsInProgressContentGone(/*hadSegments=*/true, /*playlistWasNotFound=*/true, /*lookupOk=*/false,
                                /*lookupHttpStatus=*/404, /*isInProgress=*/false));
  // isInProgress is meaningless for a lookup that failed; it must not veto.
  CHECK(IsInProgressContentGone(true, true, false, 404, /*isInProgress=*/true));
}

TEST_CASE("IsInProgressContentGone does not treat a merely failed lookup as gone", "[RecordingVisibility]")
{
  // ResolveInProgressFinished()'s own "unknown means don't assume finished"
  // still holds for anything that isn't a definitive 404.
  for (long status : {0L, 200L, 401L, 403L, 500L, 502L, 503L})
    CHECK_FALSE(IsInProgressContentGone(true, true, /*lookupOk=*/false, status, false));
}

TEST_CASE("IsInProgressContentGone treats a finished recording whose HLS directory is gone as gone",
          "[RecordingVisibility]")
{
  // Dispatcharr finished it and removed the directory (the playlist answers
  // a redirect or a 404): the file is complete, but this byte stream can't
  // be carried over to it, so a viewer still on the HLS reader is done.
  CHECK(IsInProgressContentGone(/*hadSegments=*/true, /*playlistWasNotFound=*/true, /*lookupOk=*/true,
                                /*lookupHttpStatus=*/200, /*isInProgress=*/false));
}

TEST_CASE("IsInProgressContentGone waits out the window where the status hasn't flipped yet", "[RecordingVisibility]")
{
  // A naturally completing recording's status is only written after its
  // directory is removed, so for a moment the playlist is already gone while
  // the lookup still says in progress. That resolves itself on the next
  // refresh; it must not end playback early.
  CHECK_FALSE(IsInProgressContentGone(true, true, /*lookupOk=*/true, 200, /*isInProgress=*/true));
}

TEST_CASE("IsInProgressContentGone needs a playlist that definitively answered not-found", "[RecordingVisibility]")
{
  // A playlist that failed some other way (a 5xx, a timeout) says nothing,
  // whatever the lookup answered.
  CHECK_FALSE(IsInProgressContentGone(true, /*playlistWasNotFound=*/false, false, 404, false));
  CHECK_FALSE(IsInProgressContentGone(true, /*playlistWasNotFound=*/false, true, 200, false));
}

TEST_CASE("IsInProgressContentGone never fires before the stream has had a segment", "[RecordingVisibility]")
{
  // The cold-start case: a young recording whose HLS directory simply isn't
  // there yet 404s too. ShouldRetryInProgressColdStart() waits that out;
  // this must never mistake it for gone, deleted lookup or not.
  CHECK_FALSE(IsInProgressContentGone(/*hadSegments=*/false, true, true, 200, false));
  CHECK_FALSE(IsInProgressContentGone(/*hadSegments=*/false, true, false, 404, false));
}

TEST_CASE("IsRecurringOccurrenceReschedule refuses a time change on a rule's own occurrence", "[RecordingVisibility]")
{
  CHECK(IsRecurringOccurrenceReschedule(/*ruleId=*/7, 1000, 2000, 1500, 2500));
  CHECK(IsRecurringOccurrenceReschedule(7, 1000, 2000, 1000, 2600)); // end only
  CHECK(IsRecurringOccurrenceReschedule(7, 1000, 2000, 900, 2000));  // start only
}

TEST_CASE("IsRecurringOccurrenceReschedule lets an unchanged time through, which is a title or channel edit",
          "[RecordingVisibility]")
{
  CHECK_FALSE(IsRecurringOccurrenceReschedule(7, 1000, 2000, 1000, 2000));
}

TEST_CASE("IsRecurringOccurrenceReschedule never applies to a recording with no recurring rule",
          "[RecordingVisibility]")
{
  CHECK_FALSE(IsRecurringOccurrenceReschedule(0, 1000, 2000, 1500, 2500));
  CHECK_FALSE(IsRecurringOccurrenceReschedule(0, 1000, 2000, 1000, 2000));
}

// ---------------------------------------------------------------------
// GateFinishedOnEndList
// ---------------------------------------------------------------------

namespace
{
using Clock = std::chrono::steady_clock;
const Clock::time_point kT0 = Clock::time_point(std::chrono::hours(1));
const auto kGrace = std::chrono::seconds(15);
} // namespace

TEST_CASE("GateFinishedOnEndList holds a stopped recording until the playlist has its ENDLIST", "[RecordingVisibility]")
{
  // The window measured live: status says finished, playlist is still one segment short with no tag.
  std::optional<Clock::time_point> waiting;
  CHECK_FALSE(GateFinishedOnEndList(true, false, waiting, kT0, kGrace));
  REQUIRE(waiting.has_value());
  CHECK(*waiting == kT0);
  CHECK_FALSE(GateFinishedOnEndList(true, false, waiting, kT0 + std::chrono::seconds(2), kGrace));
  // The waiting start is the first held cycle, not the latest.
  CHECK(*waiting == kT0);
  // The tag arrives (with the final segment, in the same write).
  CHECK(GateFinishedOnEndList(true, true, waiting, kT0 + std::chrono::seconds(3), kGrace));
  CHECK_FALSE(waiting.has_value());
}

TEST_CASE("GateFinishedOnEndList gives up waiting after the grace period", "[RecordingVisibility]")
{
  // A recording that died without the tag being written must still end.
  std::optional<Clock::time_point> waiting;
  CHECK_FALSE(GateFinishedOnEndList(true, false, waiting, kT0, kGrace));
  CHECK_FALSE(GateFinishedOnEndList(true, false, waiting, kT0 + kGrace - std::chrono::milliseconds(1), kGrace));
  CHECK(GateFinishedOnEndList(true, false, waiting, kT0 + kGrace, kGrace));
  CHECK(GateFinishedOnEndList(true, false, waiting, kT0 + kGrace + std::chrono::seconds(30), kGrace));
}

TEST_CASE("GateFinishedOnEndList never holds back a recording that is still going", "[RecordingVisibility]")
{
  std::optional<Clock::time_point> waiting;
  CHECK_FALSE(GateFinishedOnEndList(false, false, waiting, kT0, kGrace));
  CHECK_FALSE(waiting.has_value());
  // Not even one whose playlist somehow already has the tag.
  CHECK_FALSE(GateFinishedOnEndList(false, true, waiting, kT0, kGrace));
}

TEST_CASE("GateFinishedOnEndList restarts its wait after the candidate flips back", "[RecordingVisibility]")
{
  // ResolveInProgressFinished() isn't sticky: a later cycle can say "not finished" again.
  std::optional<Clock::time_point> waiting;
  CHECK_FALSE(GateFinishedOnEndList(true, false, waiting, kT0, kGrace));
  CHECK_FALSE(GateFinishedOnEndList(false, false, waiting, kT0 + std::chrono::seconds(10), kGrace));
  CHECK_FALSE(waiting.has_value());
  CHECK_FALSE(GateFinishedOnEndList(true, false, waiting, kT0 + std::chrono::seconds(20), kGrace));
  CHECK(*waiting == kT0 + std::chrono::seconds(20));
}

TEST_CASE("GateFinishedOnEndList finishes at once when the tag is already there", "[RecordingVisibility]")
{
  std::optional<Clock::time_point> waiting;
  CHECK(GateFinishedOnEndList(true, true, waiting, kT0, kGrace));
  CHECK_FALSE(waiting.has_value());
}

// ---------------------------------------------------------------------
// DecideDeleteTimerAction
// ---------------------------------------------------------------------

TEST_CASE("DecideDeleteTimerAction with a good lookup keeps the three existing decisions", "[RecordingVisibility]")
{
  // Finished: neither stop nor delete.
  CHECK(DecideDeleteTimerAction(false, true, 200, /*inProgress=*/false, /*upcoming=*/false) ==
        DeleteTimerAction::kAlreadyDone);
  CHECK(DecideDeleteTimerAction(true, true, 200, false, false) == DeleteTimerAction::kAlreadyDone);
  // In progress on the server: stop, even when Kodi's stale copy says forceDelete=false.
  CHECK(DecideDeleteTimerAction(false, true, 200, true, false) == DeleteTimerAction::kStop);
  CHECK(DecideDeleteTimerAction(true, true, 200, true, false) == DeleteTimerAction::kStop);
  // Not started yet: a plain delete.
  CHECK(DecideDeleteTimerAction(false, true, 200, false, true) == DeleteTimerAction::kDelete);
}

TEST_CASE("DecideDeleteTimerAction treats a 404 lookup as already gone", "[RecordingVisibility]")
{
  CHECK(DecideDeleteTimerAction(false, false, 404, false, false) == DeleteTimerAction::kAlreadyDone);
  CHECK(DecideDeleteTimerAction(true, false, 404, false, false) == DeleteTimerAction::kAlreadyDone);
}

TEST_CASE("DecideDeleteTimerAction no longer guesses past a lookup that failed any other way", "[RecordingVisibility]")
{
  // forceDelete=false and nothing known: this used to go straight to the destructive delete.
  for (long status : {0L, 401L, 403L, 500L, 502L, 503L})
    CHECK(DecideDeleteTimerAction(false, false, status, false, false) == DeleteTimerAction::kRefuseUnverified);
  // Kodi itself says it is recording: keep the existing Stop.
  for (long status : {0L, 500L})
    CHECK(DecideDeleteTimerAction(true, false, status, false, false) == DeleteTimerAction::kStop);
}

TEST_CASE("DecideDeleteTimerAction never deletes without having verified the recording", "[RecordingVisibility]")
{
  for (bool force : {false, true})
    for (long status : {0L, 200L, 401L, 404L, 500L})
      for (bool inProgress : {false, true})
        for (bool upcoming : {false, true})
          if (DecideDeleteTimerAction(force, /*lookupOk=*/false, status, inProgress, upcoming) ==
              DeleteTimerAction::kDelete)
            FAIL("deleted on an unverified lookup");
}

// ---------------------------------------------------------------------
// DecideInProgressColdStartStep
// ---------------------------------------------------------------------

TEST_CASE("DecideInProgressColdStartStep opens as soon as a segment is known, whatever the status",
          "[RecordingVisibility]")
{
  CHECK(DecideInProgressColdStartStep(true, false, /*haveSegments=*/true, /*finished=*/false) ==
        InProgressColdStartStep::kReady);
  CHECK(DecideInProgressColdStartStep(true, false, /*haveSegments=*/true, /*finished=*/true) ==
        InProgressColdStartStep::kReady);
}

TEST_CASE("DecideInProgressColdStartStep waits for a first segment while the recording is running",
          "[RecordingVisibility]")
{
  CHECK(DecideInProgressColdStartStep(true, false, /*haveSegments=*/false, /*finished=*/false) ==
        InProgressColdStartStep::kWait);
}

TEST_CASE("DecideInProgressColdStartStep stops waiting on a recording that finished with no segment",
          "[RecordingVisibility]")
{
  CHECK(DecideInProgressColdStartStep(true, false, /*haveSegments=*/false, /*finished=*/true) ==
        InProgressColdStartStep::kFinishedEmpty);
}

TEST_CASE("DecideInProgressColdStartStep retries only the young recording's missing playlist -- the live fix",
          "[RecordingVisibility]")
{
  // The failed refresh's `retryable` is ShouldRetryInProgressColdStart()'s verdict;
  // the stale segment/finished flags of a failed refresh are never consulted.
  CHECK(DecideInProgressColdStartStep(false, /*retryable=*/true, false, false) == InProgressColdStartStep::kWait);
  CHECK(DecideInProgressColdStartStep(false, /*retryable=*/true, true, true) == InProgressColdStartStep::kWait);
  CHECK(DecideInProgressColdStartStep(false, /*retryable=*/false, false, false) == InProgressColdStartStep::kFail);
  CHECK(DecideInProgressColdStartStep(false, /*retryable=*/false, true, true) == InProgressColdStartStep::kFail);
}
