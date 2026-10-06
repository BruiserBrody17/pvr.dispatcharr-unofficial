#pragma once

#include "DispatcharrClient.h"

#include <chrono>
#include <optional>

namespace dispatcharr
{

// Shared by PVRDispatcharr::GetRecordingsAmount()/GetRecordings() --
// "listed as a completed/in-progress recording" (as opposed to a
// not-yet-started, upcoming one, which is timer-only -- see
// IsListedAsTimer() below). In-progress recordings belong here too, not
// just already-finished ones: Kodi's own CPVRRecording::IsInProgress()
// cross-references GetRecordings() against the active timer list by
// channel+time overlap to decide whether a *listed recording* is still
// being written, and that's also what makes it clickable/playable while
// recording -- omitting in-progress ones here (a real earlier bug in
// this addon) made them show up only as an uneditable timer entry, with
// nothing to actually click and play. GetRecordingsAmount() and
// GetRecordings() must agree on this exact predicate, or the amount Kodi
// pre-allocates for won't match what GetRecordings() actually returns.
bool IsListedAsRecording(const Recording& rec);

// Shared by PVRDispatcharr::GetTimersAmount()/GetTimers() -- "listed as
// a timer" (as opposed to a completed recording, surfaced via
// GetRecordings()/IsListedAsRecording() instead). Covers both an
// in-progress recording (conceptually still has an active timer) and a
// not-yet-started upcoming one -- except a *missed* one (see
// IsMissedOccurrence() below), added 2026-09-26 (a 20th-pass audit) to
// fix a real, confirmed bug: without this exclusion, a missed
// occurrence stayed listed as a perpetual SCHEDULED timer forever, one
// Kodi itself never expires (its own pruning in `CPVRTimers` only
// applies to a client-owned timer that isn't marked repeating/still
// pending, confirmed against Kodi's real source) and this addon can't
// safely edit either (any edit sends an already-past `end_time`, which
// Dispatcharr's own validation rejects with a 400). It's also then
// wrongly returned by Kodi's own `GetNextActiveTimer()`/
// `GetNextEventTime()` as "the next thing to record" instead of the
// real next occurrence, since Kodi has no server-side concept of
// "missed" of its own to filter it by first.
bool IsListedAsTimer(const Recording& rec, time_t now);

// Whether `rec` is a recurring occurrence whose entire scheduled window
// elapsed before Dispatcharr ever moved it out of `status=="scheduled"`
// -- confirmed against Dispatcharr's own real current upstream source
// (a 20th-pass audit, 2026-09-26, cloned into a scratchpad, never
// committed to this repo -- stronger than the API shape alone, not the
// same standard as a live test): this can genuinely happen (e.g.
// Dispatcharr or its Celery worker down for an occurrence's whole
// window), and nothing server-side ever revisits such a row afterward --
// `purge_recurring_rule_impl` only deletes rows with `start_time >=
// now`, and startup recovery only resumes/finalizes a row still inside
// its window or already flipped to `"recording"`. `ParseRecordingFields()`
// correctly treats `status=="scheduled"` as authoritatively `isUpcoming`
// regardless of the clock (see its own comment) -- this is the
// complementary check for when that authoritative status has run out
// its own clock entirely. `endTime <= 0` (no known end time at all) is
// never treated as missed -- there's nothing to compare `now` against.
bool IsMissedOccurrence(bool isUpcoming, time_t endTime, time_t now);

// Shared by PVRDispatcharr::GetRecordingStreamProperties()/
// OpenRecordedStream() -- whether a recording needs the growing-buffer
// (native-demuxer, still-being-written) read path rather than the
// completed-recording one. hlsDirStillPresent alongside isInProgress:
// see Recording::hlsDirStillPresent's own comment above -- a
// just-stopped recording still needs the growing-buffer path for the
// whole window until Dispatcharr's own HLS-to-MKV concat actually
// finishes (status flips to "not in progress" immediately on stop, well
// before that concat is done). A real reported bug: opening a
// just-stopped recording during that window either errored outright (no
// stable file yet) or played without seeking (a file existed but was
// still being actively written by the concat).
bool ShouldUseGrowingBufferPlayback(const Recording& rec);

// Fix for a real, confirmed bug in
// RefreshInProgressRecordingManifest()'s own fresh in-progress check:
// `finished = !(GetRecordingById(...) && rec.isInProgress)` treated a
// *failed* lookup (a transient network blip, an auth hiccup -- anything
// that makes GetRecordingById() itself return false) identically to a
// genuinely completed recording. ReadInProgressRecordingStream() then
// returned an immediate EOF at the tail for a recording that was still
// actively being written, stopping playback outright over what should
// have been a retry-worthy hiccup -- and the recording's own
// m_inProgressSegmentCache entry was erased along with it.
//
// A failed lookup tells you nothing about whether the recording is
// still in progress -- treat it as "unknown" and keep whatever
// `finished` already was, erring toward *not* finished (the safer
// default: a spurious "still recording" merely means one more
// catch-up-loop iteration before the real answer arrives on a later,
// successful call, while a spurious "finished" ends playback
// outright). The same erring-toward-not-sure-so-don't-act principle
// ShouldRenewRecurringRule() already applies when its own
// GetRecordings() call fails.
//
// allProbesSucceeded closes a second, related real bug (found via code
// reading, not yet reproduced live): RefreshInProgressRecordingManifest()
// can discover the recording's own final segment(s) in the same refresh
// cycle where the server-side lookup above first reports isInProgress
// as false, but a segment whose byte-size HEAD probe fails that same
// cycle isn't merged this round (see CountLeadingProbedSegments()'s own
// comment -- only the leading, successfully-probed prefix is). If
// `finished` were still allowed to flip true in that same cycle,
// ReadInProgressRecordingStream() would short-circuit straight to EOF
// the moment its read position reaches the (now permanently short)
// totalBytes, since it only calls RefreshInProgressRecordingManifest()
// again while !finished. In practice that means this last segment's
// worth of content is stranded for the read that already hit that EOF
// (Kodi typically tears the stream down soon after, well before a later
// GetInProgressRecordingStreamLength()/DurationMs() poll -- which,
// unlike the read path, keeps calling this function unconditionally
// regardless of `finished` -- would otherwise get a chance to pick up
// the retried probe). Pass false here whenever this cycle's own probe
// batch had any failure (regardless of what isInProgress says) to hold
// `finished` at false for one more cycle instead, the same
// erring-toward-not-finished safety margin as the lookupOk case above;
// pass true when there was nothing to probe this cycle at all (the
// normal, most common case) or every probe in it succeeded.
bool ResolveInProgressFinished(bool lookupOk, bool isInProgress, bool previousFinished, bool allProbesSucceeded);

// Holds `finished` back until the playlist carries `#EXT-X-ENDLIST` (added
// 2026-09-30, fixing a real bug reproduced live -- docs/OPEN_ITEMS.md's entry
// on a user Stop stranding the final segment). ResolveInProgressFinished()
// trusts the recording's own status, and Dispatcharr's stop endpoint writes
// `status = "stopped"` synchronously while ffmpeg is torn down afterwards in
// a background thread: measured against the real instance, the playlist
// stayed one segment short, with no ENDLIST, for about three seconds after the
// status flipped, then gained the final segment and the tag in one step. A
// viewer at the live edge was handed EOF in that window -- the addon logged
// finished=1 at its final byte count and probed one more segment
// 2.6s later, after the reader had already stopped asking.
//
// `candidateFinished` is ResolveInProgressFinished()'s answer. While it is
// true but the playlist has no ENDLIST, this returns false and remembers when
// the wait began in `waitingSince`; it returns true once the tag shows up, or
// once `grace` has passed regardless -- a recording that died without the tag
// being written must still end, not hang the reader. A candidate that is false
// clears the wait. Not applied to the path where Dispatcharr has already
// removed the HLS directory (FetchRawInProgressPlaylist() sees a redirect or
// 404): the tag is gone along with the directory, and that path ends the
// stream by its own rule.
constexpr std::chrono::seconds kEndListGrace{15};
bool GateFinishedOnEndList(bool candidateFinished, bool playlistHasEndList,
                           std::optional<std::chrono::steady_clock::time_point>& waitingSince,
                           std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration grace);

// Whether OpenInProgressRecordingStream()'s cold-start loop should keep
// waiting after a *failed* playlist fetch, rather than giving up on the
// first one. Found live 2026-09-30 (docs/OPEN_ITEMS.md): a recording
// opened within roughly its first three seconds failed every time (6 of
// 6) with "HTTP 404 fetching in-progress playlist" after a single
// attempt -- Dispatcharr's HLS directory (and its `index.m3u8`) simply
// doesn't exist yet that soon after the recording task starts, and
// `RecordingViewSet.hls()` (apps/channels/api_views.py) answers 404 for
// both "no `_hls_dir` yet" and "no playlist file in it yet". The loop's
// own 45s budget (see its comment: confirmed live as necessary for the
// slower first-segment case) only ever applied to a fetch that *succeeded*
// with no segments in it -- a fetch that failed outright skipped it.
//
// Deliberately narrow: only an HTTP 404 (not a 3xx, which means the
// directory was already removed and the file is complete; not a
// transport failure, a 401, or a 5xx, none of which say the recording
// is merely young) AND a lookup that succeeded and says the recording is
// in progress right now -- a recording that has finished, failed or been
// deleted must still fail fast, not burn the whole budget. A failed
// lookup is treated as "unknown, don't wait", the same
// erring-toward-not-assuming principle ResolveInProgressFinished() above
// applies in the opposite direction.
bool ShouldRetryInProgressColdStart(long playlistHttpStatus, bool lookupOk, bool isInProgress);

// What OpenInProgressRecordingStream()'s cold-start loop does after one manifest
// refresh, given how it went. The loop waits for a first segment with a bounded
// budget; the order of these cases is what the live-confirmed behaviour depends
// on and what the test pins:
//   kReady         -- at least one segment is known: open, whatever the status.
//   kFinishedEmpty -- the refresh worked, there is no segment, and the recording
//                     is already finished: nothing will ever arrive, so stop
//                     waiting (the open still succeeds, and the first read is
//                     EOF -- a zero-length recording, not an error).
//   kWait          -- the refresh worked but nothing is there yet, or it failed in
//                     the one way ShouldRetryInProgressColdStart() says is a
//                     young recording's missing HLS directory: sleep and retry.
//   kFail          -- the refresh failed for any other reason: give up now rather
//                     than burn the budget (a finished, failed or deleted
//                     recording).
// `haveSegments`/`finished` are only consulted after a successful refresh: a
// failed one has not updated them.
enum class InProgressColdStartStep
{
  kReady,
  kWait,
  kFail,
  kFinishedEmpty,
};

InProgressColdStartStep DecideInProgressColdStartStep(bool refreshOk, bool retryable, bool haveSegments, bool finished);

// Whether an in-progress-recording stream's server-side HLS content is
// permanently gone -- nothing more will ever be readable through it, so the
// reader should end playback rather than keep asking. Found live 2026-09-30
// (docs/OPEN_ITEMS.md): a recording deleted on the server while a viewer sat
// at its tail with real-time updates off (the default -- Kodi can't learn of
// the deletion any other way) left the addon polling a recording that no
// longer existed, ~7.8 requests/sec, every one a 404 (an alternating
// recording lookup and playlist fetch), until Kodi's own no-data timeout
// finally ended playback ~100s later. ResolveInProgressFinished() couldn't
// help: its "unknown means don't assume finished" rule for a failed lookup is
// right for a network blip, but a lookup that answered 404 is not unknown.
//
// Two situations qualify, both requiring that the stream already had
// segments (`hadSegments`) so a young recording whose HLS directory simply
// isn't there *yet* -- the cold-start case ShouldRetryInProgressColdStart()
// waits out -- is never mistaken for a gone one, and a playlist that
// definitively answered 404 or a redirect (`playlistWasNotFound`,
// FetchRawInProgressPlaylist()'s own signal for "the HLS directory is
// gone"):
//   - the recording lookup succeeded and says it is no longer in progress:
//     Dispatcharr finished it and removed the HLS directory (the file is
//     complete, but this byte stream can't be carried over to it);
//   - the recording lookup itself answered 404: the recording was deleted
//     outright. Both endpoints agreeing the resource doesn't exist is
//     stronger evidence than either alone -- a lookup that merely failed
//     (a 5xx, a transport error) says nothing and stays "unknown".
// A lookup that says the recording is *still* in progress never qualifies,
// even alongside a 404/redirect playlist: a naturally completing
// recording's status is only flipped after its directory is removed, and
// that brief window resolves itself on the next refresh.
bool IsInProgressContentGone(bool hadSegments, bool playlistWasNotFound, bool lookupOk, long lookupHttpStatus,
                             bool isInProgress);

// Fix for a real, confirmed data-loss risk in PVRDispatcharr::DeleteTimer()'s
// one-time-recording branch: `forceDelete` (Kodi's own signal for "this
// timer is still actively recording," confirmed against Kodi's source --
// see DeleteTimer()'s own comment) comes from Kodi's *cached* copy of the
// timer, only refreshed by this addon's own GetTimers() -- up to
// recording_refresh_minutes stale (5 minutes by default). A recording
// that had genuinely already started within that stale window still
// read as "scheduled" to Kodi, so `forceDelete` could come back false
// even though the recording was actually in progress server-side --
// routing to DeleteRecording() (removes the file entirely) instead of
// StopRecording() (stops it while keeping the partial content), exactly
// the confirmed-by-testing failure DeleteTimer()'s own comment already
// describes, just reached via stale Kodi state rather than never
// routing there at all.
//
// Errs toward the safer action (stop, not delete) whenever *either*
// signal says "in progress": true if forceDelete is true, or if the
// lookup succeeded and the server's own isInProgress says so -- a
// failed lookup contributes nothing either way (matches
// ResolveInProgressFinished()'s own "unknown means don't assume
// finished" principle above), leaving forceDelete as the sole answer in
// that case.
bool ShouldStopInsteadOfDelete(bool forceDelete, bool lookupOk, bool serverSaysInProgress);

// Fix for a second real, confirmed data-loss risk in the same
// DeleteTimer() one-time-recording branch as ShouldStopInsteadOfDelete()
// above, found via a project-wide review, not itself independently
// reproduced. That fix closed the "recording started within the stale
// window" half (Kodi's cached timer said "not recording" when the
// server said otherwise, routing what should have been a Stop to a
// destructive Delete instead) -- but not the "finished within the stale
// window" half: a recording short enough to both start AND finish
// inside a single recording_refresh_minutes interval (any recording
// shorter than that setting -- default 5 minutes, up to 60 -- e.g. a
// 30-minute show against the default) can still show as a SCHEDULED
// timer to Kodi's own stale cache even though the server-side recording
// already completed normally. Without this, ShouldStopInsteadOfDelete()
// sees forceDelete=false (Kodi's cache never thought it was recording)
// and the fresh isInProgress=false (correctly -- it's done) and returns
// false, routing to DeleteRecording() -- permanently deleting the
// just-completed recording's own file, not merely cancelling a timer
// that never actually ran.
//
// Checked by the caller *before* ShouldStopInsteadOfDelete(): true only
// when the fresh lookup succeeded and definitively shows the recording
// is neither in progress nor upcoming (the same condition
// IsListedAsRecording() above already uses to decide this item belongs
// in Recordings, not Timers, now) -- a failed lookup contributes
// nothing here, same "unknown means don't assume" principle
// ShouldStopInsteadOfDelete() itself already applies, so DeleteTimer()
// falls back to its existing Stop-or-Delete decision in that case
// rather than a new, less-certain one. When true, the caller does
// neither: nothing to stop (already finished) or delete (the recording
// is real, wanted content, not an empty timer) -- Kodi's own next
// refresh naturally drops the now-stale Timers entry once this addon's
// own cache catches up.
bool IsAlreadyFinishedRecording(bool lookupOk, bool isInProgress, bool isUpcoming);

// What DeleteTimer() does for a one-time recording, folding the three decisions
// above (already finished, stop instead of delete) together with the one they
// left open: a server lookup that *failed*.
//
// With a failed lookup nothing says whether the recording is running, finished or
// not yet started, and both guesses were wrong in a real way (docs/OPEN_ITEMS.md,
// "DeleteTimer(): failed-but-not-confirmed-gone lookup"): Delete on a recording
// that is actually in progress destroys its file, and Stop on one that has not
// started leaves a permanent terminal-status row with no file
// (RecordingViewSet.stop() answers 200 for everything but completed/interrupted/
// failed). So a failed lookup is no longer guessed past:
//   * a 404 means it is already gone -- nothing to do, success;
//   * any other failure with Kodi's own forceDelete set (Kodi believes it is
//     recording, which is what that flag means) keeps the existing Stop;
//   * any other failure otherwise refuses, and the user is told to try again. The
//     delete or stop would most likely have failed on the same server trouble
//     anyway; this just makes sure the one case where it does not cannot destroy
//     a running recording.
enum class DeleteTimerAction
{
  kDelete,
  kStop,
  kAlreadyDone,
  kRefuseUnverified,
};
DeleteTimerAction DecideDeleteTimerAction(bool forceDelete, bool lookupOk, long lookupHttpStatus, bool isInProgress,
                                          bool isUpcoming);

// The action PVRDispatcharr::UpdateTimer()'s own one-time-recording
// branch should take for a fresh Recording lookup.
enum class OneTimeTimerEditAction
{
  Extend,     // rec.isInProgress -- route to the dedicated extend endpoint,
              // never a plain reschedule PATCH (see UpdateTimer()'s own
              // ExtendRecording() comment for why a bare PATCH here would
              // actually revoke the running Celery task instead).
  RenameOnly, // a missed occurrence, or an already-finished recording --
              // nothing left to (re)schedule; only a title change applies.
  Reschedule  // not yet started -- a normal start/end-time PATCH applies.
};

// Pure classification core of UpdateTimer()'s own one-time-recording
// branch -- an inline, order-sensitive 4-way decision (extended in
// progress -> missed occurrence -> already finished -> not yet started)
// that TWO separate passes (20 and 21) each added a new case to, each
// time inserting it at a specific point in the existing if/else-if
// chain to get the ordering right relative to the others. Pulled out
// specifically so that ordering is locked in by a test instead of
// resting on each future change getting it right again by inspection --
// see ../tests/test_recording_visibility.cpp for the ordering tests
// (e.g. isInProgress must win over IsMissedOccurrence()/
// IsAlreadyFinishedRecording() even when their own conditions would
// otherwise also match, since neither "missed" nor "finished" can be
// true for a recording that's actually still in progress, but a test
// pins that invariant down explicitly rather than relying on it holding
// by construction).
inline OneTimeTimerEditAction ClassifyOneTimeTimerEdit(bool isInProgress, bool isUpcoming, time_t endTime, time_t now)
{
  if (isInProgress)
    return OneTimeTimerEditAction::Extend;
  if (IsMissedOccurrence(isUpcoming, endTime, now))
    return OneTimeTimerEditAction::RenameOnly;
  if (IsAlreadyFinishedRecording(/*lookupOk=*/true, isInProgress, isUpcoming))
    return OneTimeTimerEditAction::RenameOnly;
  return OneTimeTimerEditAction::Reschedule;
}

// Whether a one-time-recording edit would move one of a recurring rule's own
// materialized occurrences (recurringRuleId != 0) to a different time. Such an
// edit can't hold on the server, confirmed live 2026-09-30: Dispatcharr's hourly
// maintain_recurring_recordings task only checks for a recording at the rule's
// *original* slot, finds it empty, and recreates it beside the edited one (seen
// 48 minutes after an edit: 31 recordings where there had been 30), and any
// rule-level PATCH -- this addon's own end_date renewal included -- purges and
// regenerates every future occurrence from the rule's base schedule, silently
// reverting the edit (seen when a renewal landed four minutes after one). So
// UpdateTimer() refuses it rather than appear to succeed. A title or channel
// change doesn't move the slot and isn't caught here.
inline bool IsRecurringOccurrenceReschedule(int recurringRuleId, time_t recStart, time_t recEnd, time_t newStart,
                                            time_t newEnd)
{
  return recurringRuleId != 0 && (newStart != recStart || newEnd != recEnd);
}

} // namespace dispatcharr
