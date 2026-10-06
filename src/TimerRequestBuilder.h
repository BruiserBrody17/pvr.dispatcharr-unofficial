#pragma once

#include <nlohmann/json_fwd.hpp>

#include "RecurringRuleEdit.h"

#include <ctime>
#include <string>
#include <vector>

namespace dispatcharr
{

// Pure request-body/query-string assembly for the timer-CRUD HTTP calls
// in DispatcharrClient.cpp, pulled out specifically so each one's own
// documented, incident-backed invariant is locked in with a test rather
// than resting on a future edit getting it right again by inspection
// (a 32nd-pass audit, found via a project-wide review -- none of these
// had any test coverage before this). Zero Kodi/curl/member-state
// dependency; see ../tests/test_timer_request_builder.cpp.

// DispatcharrClient::CreateSeriesRule()'s own POST/upsert body.
// `channelId <= 0` and an empty `tvgId` both omit their own key entirely
// rather than sending an explicit sentinel/empty value -- confirmed
// against Dispatcharr's own real current upstream source that
// `channel_id`/`tvg_id` are both genuinely optional ("omit to match
// across all channels"/"pin recordings to"), and that sending
// `channel_id` literally as this addon's own "no channel" sentinel used
// to trip a 400 outright (an 18th-pass fix -- see this call's own header
// comment). `mode` is only sent as "new" when `recordNewOnly` is true;
// Dispatcharr's own default ("all", every matching episode including
// reruns) is left to apply itself otherwise, matching the live schema.
//
// titleMode/description/descriptionMode/untaggedIsNew/epgSourceId added
// 2026-09-29, fixing a real, confirmed bug found live (see
// docs/OPEN_ITEMS.md): Dispatcharr's own SeriesRulesAPIView.post() does
// a full existing.clear(); existing.update(rule_record) on match, and
// rule_record only ever includes these fields when the request itself
// sends them -- so any edit from this addon that omitted them (as every
// call used to, unconditionally) silently reset a rule's own
// title_mode/description/description_mode customized via Dispatcharr's
// own web UI back to their plain server-side defaults, live-confirmed
// with a real round trip. None of these five have any Kodi-side UI of
// their own (Kodi's timer dialog has no "how to match the title" field,
// etc.), so this addon can never *set* them on the user's behalf -- the
// only correct behavior is to echo back whatever was last cached for
// this exact rule (UpdateTimer()'s job) so an edit to an unrelated field
// doesn't collaterally reset them. `titleMode`/`descriptionMode` follow
// the same "empty omits the key" convention as `tvgId` above -- an empty
// string is never a real value for either (both are always one of a
// small fixed set of choices server-side) -- and `description` reuses
// it too: an empty description is indistinguishable from, and produces
// the exact same server-side outcome as, the field being omitted
// entirely (Dispatcharr's own default is also ""), so this is safe for
// both AddTimer()'s create path (nothing cached yet to echo -- pass
// empty strings/0/false, exactly matching this function's own prior,
// narrower signature's behavior) and a genuinely-blank cached value.
// `epgSourceId <= 0` omits `epg_source_id` the same way `channelId <= 0`
// already omits `channel_id`. `untaggedIsNew` is only ever sent when
// `recordNewOnly` is also true, mirroring Dispatcharr's own
// `if mode == "new" and untagged_is_new` gating -- sending it standalone
// would be meaningless server-side (and NOT sending it when true would
// silently reset it the exact same way this whole fix exists to avoid).
nlohmann::json BuildSeriesRuleRequestBody(int channelId, const std::string& tvgId, const std::string& titlePattern,
                                          bool recordNewOnly, const std::string& titleMode,
                                          const std::string& description, const std::string& descriptionMode,
                                          bool untaggedIsNew, int epgSourceId);

// DispatcharrClient::UpdateRecurringRule()'s own PATCH body: exactly the fields in
// `patch`, and nothing else (see RecurringRuleEdit.h for why an edit sends only
// what changed). `end_date` appears only when the patch carries one -- which it
// does only to give an open-ended rule an end date it needs to be editable at all
// (ComputeEndDateForOpenEndedRuleEdit()); every other edit omits it, so
// RecurringRecordingRuleSerializer's partial-update fallback preserves the
// instance's existing value, which is what lets the whole separate family of
// end_date-preserving fixes in this addon (RecurringRuleRenewal.h) work without this
// call needing to know or protect that value itself.
nlohmann::json BuildRecurringRuleUpdateBody(const RecurringRuleEditPatch& patch);

// DispatcharrClient::UpdateOneTimeRecording()'s own PATCH body. Always
// includes both start_time and end_time -- confirmed live that a PATCH
// omitting either one crashes server-side.
nlohmann::json BuildOneTimeRecordingPatchBody(time_t start, time_t end, int channelId = 0);

// The channel id an edit of a not-yet-started one-time recording should send,
// or 0 for "leave it alone": Kodi's own timer dialog lets the user change a
// scheduled recording's channel (GetTimerTypes() declares
// PVR_TIMER_TYPE_SUPPORTS_CHANNELS), and the PATCH used to drop that change
// silently. Only a real, different channel is sent, so an edit that doesn't
// touch the channel never rewrites it, and Kodi's "no channel" sentinel
// (PVR_CHANNEL_INVALID_UID) never reaches Dispatcharr.
int ChannelToSendOnOneTimeEdit(int kodiChannelUid, int currentChannelId);

// DispatcharrClient::CreateOneTimeRecording()'s own POST body. Maps a
// non-positive `start` to `now` before sending it -- fix for a real,
// confirmed, previously-undocumented dependency found via a project-wide
// review (a 39th-pass audit, confirmed against Kodi's own real current
// SDK source, not itself independently reproduced): Kodi's own instant
// recording (record the current live channel right now, not from the
// guide) genuinely sends a start time of exactly 0, not the real current
// time. `CPVRTimerInfoTag::CreateFromDate()` (PVRTimerInfoTag.cpp)
// briefly sets the timer's own start to the real current UTC time
// *only* to build its "Instant recording: <summary>" display string,
// then deliberately re-sets it back to the raw 0 sentinel immediately
// afterward ("now that we have a nice summary, we can set the 'special'
// start time value that indicates an instant recording") -- that 0 is
// what actually reaches this addon's own AddTimer(), not a real
// timestamp. This has only ever worked because
// RecordingSerializer.validate() silently rewrites any past start_time
// to now server-side (confirmed against Dispatcharr's own real current
// upstream source) -- an undocumented reliance on that same clamp this
// addon's own duplicate-recording fixes elsewhere (EpgProgramMatch.h)
// already had to work around the *consequences* of, not something this
// addon ever did on purpose. Mapping it here instead means an instant
// recording keeps working correctly even if Dispatcharr ever tightens
// that validation to reject a genuinely-past start_time outright.
//
// includeEpgProgramWindow (added 2026-09-29, fixing a real, confirmed,
// live-verified bug -- see docs/OPEN_ITEMS.md's "One-time recordings from
// Kodi miss Dispatcharr's pre/post padding" entry): when true, adds `custom_properties: {"program":
// {"start_time", "end_time"}}` -- title/id deliberately omitted so
// Dispatcharr's own later auto-enrichment still runs (confirmed live,
// not just from source: two real recordings, one with this exact shape
// and one with none, both came back fully enriched within seconds of
// the same async task, with this one's own start_time/end_time merged
// in alongside the new fields rather than replaced).
// `RecordingSerializer.validate()` only applies the instance's own
// configured pre/post padding when `custom_properties.get("program")`
// is a dict -- CreateOneTimeRecording() previously never sent one at
// all, so a recording created by pressing "Record" in Kodi's own EPG
// guide got exactly the raw EPG start/end times, no padding, regardless
// of this addon's own recording_pre/post_offset_minutes settings.
// Callers should only pass true for a genuinely EPG-based timer
// (`timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID`) -- a manual, non-EPG
// recording has no real programme window to associate with the padding
// mechanism in the first place, matching this fix's own original,
// deliberately narrower scope.
nlohmann::json BuildOneTimeRecordingCreateBody(int channelId, time_t start, time_t end, time_t now,
                                               bool includeEpgProgramWindow);

// DispatcharrClient::DeleteSeriesRule()'s own query-string suffix
// (everything after the base path) -- confirmed against the live schema
// that a series rule is deleted by title + tvg_id query params, not a
// path id (there is no /api/channels/series-rules/{id}/ route). Values
// are URL-encoded via the already-tested UrlEncode() (UrlEncode.h);
// `tvgId` is only appended when non-empty.
//
// `epgSourceId` added 2026-09-29, fixing a real, confirmed data-loss bug
// found via a project-wide review, confirmed against Dispatcharr's own
// real current upstream source (see docs/OPEN_ITEMS.md): its own
// SeriesRulesAPIView.delete() only restricts by epg_source_id when the
// request itself supplies one -- omitting it (as every call used to,
// unconditionally) matches and removes *every* rule sharing title+tvgId
// regardless of which EPG source each one is pinned to, not just the one
// the user actually meant to delete. Only appended when > 0, the same
// "omit rather than send a sentinel" convention `tvgId` above already
// uses.
std::string BuildSeriesRuleDeleteQuery(const std::string& title, const std::string& tvgId, int epgSourceId);

} // namespace dispatcharr
