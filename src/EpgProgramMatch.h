#pragma once

#include "XmlTvParser.h"

#include <ctime>
#include <vector>

namespace dispatcharr
{

// Finds the index of the EPG entry in `entries` that a recording spanning
// [recStart, recEnd) was actually recorded from, mirroring Dispatcharr's
// own server-side match (`_match_epg_program_by_timeslot()`,
// apps/channels/tasks.py -- confirmed against its own real current
// upstream source, a 32nd-pass audit, not itself independently
// reproduced): the entry with the largest overlap against the recording's
// own window, but only if that overlap covers at least 80% of the
// recording's own duration -- a recording spanning multiple programmes
// with no single dominant one returns no match (-1), same as
// Dispatcharr's own "Custom Recording" fallback for that case.
//
// Why this is needed at all: PVRDispatcharr::GetTimers() never calls
// SetEPGUid() on a one-time recording's timer, so Kodi has no direct
// link back to the EPG tag it was recorded from -- found via a
// project-wide review, confirmed against Kodi's own real current SDK
// source, not itself independently reproduced. Kodi's own fallback probe
// (CPVRTimerInfoTag::GetEpgInfoTag(), PVRTimerInfoTag.cpp) then tries
// GetTagBetween(timerStart-2min, timerEnd+2min) instead, which requires
// the EPG tag's own start time to fall at or after timerStart-2min. This
// addon's own recStart is Dispatcharr's own (possibly clamped) start_time
// -- confirmed against Dispatcharr's own real current upstream source
// that RecordingSerializer.validate() silently rewrites a past
// start_time to now on create/update -- so pressing "Record" on a
// programme already more than 2 minutes into its run sends a start_time
// later than the programme's own real start. Kodi's window-based
// fallback then never matches the real EPG tag at all: the guide keeps
// showing "Record" instead of a recording indicator, and a second press
// creates a second, duplicate Recording server-side (Dispatcharr has no
// overlap guard of its own).
//
// This overlap-ratio match works despite the same clamped recStart
// Dispatcharr's own serializer produces, for the same reason
// Dispatcharr's own server-side auto-enrichment (which populates
// custom_properties.program from this exact same clamped start_time) is
// unaffected by it: both the overlap and the recording's own duration
// are measured from the same clamped recStart, so the ratio is
// self-consistent regardless of how late the recording actually started
// -- unlike Kodi's own fixed +/-2-minute window, which isn't.
//
// Deliberately returns an index into the caller's own already-cached
// entries (this addon's own channel-number-keyed XMLTV cache,
// PVRDispatcharr::m_epgByChannelNumber) rather than an EpgEntry copy, so
// the caller can compute ComputeBroadcastId() (EpgTagUtil.h) from the
// exact same entry GetEPGForChannel() itself would have assigned a
// broadcast id to, for the exact same channel/start time pair, rather
// than duplicating that hash's own inputs here.
//
// Still not sufficient on its own for a recording whose own [recStart,
// recEnd) window is *padded* (not just clamped) -- added 2026-09-26, a
// 34th-pass audit, fixing a real gap found via a project-wide review,
// confirmed against Dispatcharr's own real current upstream source, not
// itself independently reproduced: RecordingSerializer.validate()
// applies the global pre/post-padding offsets directly onto a
// recording's own start_time/end_time whenever its own
// custom_properties.program is present (Dispatcharr's own web UI Guide
// "Record" button, and series-rule-materialized occurrences, both
// always send one; this addon's own CreateOneTimeRecording()
// deliberately never does, so its own recordings are only ever clamped,
// never padded). Post-padding alone can already push the overlap ratio
// below 80% well before the programme is over (fails once post-padding
// exceeds a quarter of the programme's own remaining runtime), and
// combined with the same past-start clamp above, the ratio can fail
// almost immediately after pressing Record on an already-airing,
// padded, close-to-ending programme -- the exact pass-32 symptom
// (duplicate recording on a second press), just reached via a
// Dispatcharr-web-UI-created recording instead of a Kodi-created one.
// See FindEpgEntryIndexByStartTime() below, which a caller should prefer
// whenever a recording's own true, never-padded programme start time is
// available (Recording::programStartTime, RecordingParser.cpp), falling
// back to this function only when it isn't (every recording this addon
// itself ever creates, since Dispatcharr's own enrichment for those
// never backfills programme times, only id/title/sub_title/description).
int FindEpgEntryIndexCoveringRecording(const std::vector<EpgEntry>& entries, time_t recStart, time_t recEnd);

// Finds the index of the EPG entry in `entries` whose own startTime is
// within `toleranceSeconds` of `targetStartTime` (the closest one, if
// more than one qualifies) -- the exact-match counterpart to
// FindEpgEntryIndexCoveringRecording() above, for when a recording's own
// true, never-padded/never-clamped programme start time is already
// known (Recording::programStartTime -- see its own comment,
// DispatcharrClient.h, and FindEpgEntryIndexCoveringRecording()'s own
// comment above for why the padded/clamped recording window alone isn't
// always enough). A small tolerance (rather than requiring exact
// equality) allows for this addon's own XMLTV-parsed guide and
// Dispatcharr's own internal EPG database drifting by a second or two on
// the same underlying programme, without needing them to originate from
// literally the same code path. Returns -1 when nothing qualifies.
int FindEpgEntryIndexByStartTime(const std::vector<EpgEntry>& entries, time_t targetStartTime, int toleranceSeconds);

struct EpgOverlapWindow
{
  time_t start;
  time_t end;
};

// Picks the window PVRDispatcharr::ResolveRecordingBroadcastId()'s own
// overlap-ratio fallback (FindEpgEntryIndexCoveringRecording() above)
// should actually match against, when FindEpgEntryIndexByStartTime()'s
// own exact match has already failed -- fix for a real, confirmed gap
// found via a project-wide review (a 36th-pass audit, not itself
// independently reproduced): that exact match can legitimately fail even
// with a known, non-zero programStartTime (this addon's own EPG cache
// drops an already-ended entry at its next XMLTV refresh -- see
// docs/OPEN_ITEMS.md's own entry on this durability gap), and the
// overlap-ratio fallback previously always ran on the recording's own
// possibly-padded/clamped [recStartTime, recEndTime) regardless -- which
// risks a *wrong* match, not just a missing one, for a padded recording:
// with enough post-padding relative to a short programme's own duration
// (post-padding at or above roughly 4x the programme's own
// pre-padding+duration), the padded window's own >=80% overlap can land
// on the *next* programme instead, linking this recording's timer/
// EPGEventId to an unrelated show for as long as it airs.
//
// Prefers [programStartTime, programEndTime) -- the recording's own
// true, never-padded programme window -- whenever both are valid
// (programEndTime > programStartTime > 0), falling back to
// [recStartTime, recEndTime) only when they aren't. All-or-nothing:
// never pairs one end from the programme window with the other from the
// padded/clamped recording, which would just be its own new source of a
// wrong overlap. A recurring-rule occurrence's own program.start_time/
// end_time are never separately padded in the first place -- confirmed
// against Dispatcharr's own real current upstream source,
// sync_recurring_rule_impl() (apps/channels/tasks.py): both are derived
// from the exact same start_dt/end_dt as the recording's own top-level
// start_time/end_time, via a plain Recording.objects.create() that never
// goes through RecordingSerializer.validate()'s own padding logic at all
// -- so preferring the programme window here is a no-op for that case,
// not a behavior change.
EpgOverlapWindow ResolveEpgOverlapWindow(time_t recStartTime, time_t recEndTime, time_t programStartTime,
                                         time_t programEndTime);

} // namespace dispatcharr
