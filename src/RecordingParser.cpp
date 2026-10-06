#include "RecordingParser.h"

#include "DateTimeFormat.h"
#include "JsonFieldUtil.h"
#include "ManagedRecurringRule.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace dispatcharr
{

bool IsRecordingResponseFor(const nlohmann::json& item, int expectedId)
{
  return item.is_object() && item.contains("id") && item["id"].is_number_integer() &&
         FieldOr(item, "id", 0) == expectedId && expectedId != 0;
}

int ClampedDurationSeconds(time_t start, time_t end)
{
  if (end <= start)
    return 0;
  const long long seconds = static_cast<long long>(end) - static_cast<long long>(start);
  return seconds > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(seconds);
}

Recording ParseRecordingFields(const nlohmann::json& item, time_t now)
{
  Recording r;
  r.id = FieldOr(item, "id", 0);
  r.channelId = FieldOr(item, "channel", FieldOr(item, "channel_id", 0));
  r.startTime = TimeFromIso(FieldOr<std::string>(item, "start_time", ""));
  r.endTime = TimeFromIso(FieldOr<std::string>(item, "end_time", ""));
  r.durationSeconds = ClampedDurationSeconds(r.startTime, r.endTime);
  r.isInProgress = r.startTime > 0 && r.startTime <= now && now < r.endTime;
  r.isUpcoming = r.startTime > now;

  const nlohmann::json& custom = item.contains("custom_properties") ? item["custom_properties"] : nlohmann::json();
  if (custom.is_object())
  {
    // custom_properties.status is a more authoritative signal than the
    // start/end time window above when present: a recording stopped
    // early (see DispatcharrClient::StopRecording()) keeps its
    // originally-scheduled end_time untouched, so the time-window check
    // alone would keep reporting it as in-progress for the rest of that
    // original duration even though it finished the moment it was
    // stopped. Confirmed values, confirmed directly against Dispatcharr's
    // own real source (apps/channels/tasks.py/api_views.py, not just its
    // API shape): "scheduled" (a genuinely not-yet-started occurrence --
    // sync_recurring_rule_impl() only ever sets this for a start_time
    // still in the future at creation time, gated by its own `if
    // start_dt <= now: continue`), "recording" (currently active), and
    // "completed"/"stopped"/"interrupted" (all finished, one way or
    // another -- this exact terminal set is api_views.py's own extend()
    // action's own check, `if cp.get("status") in ("completed",
    // "stopped", "interrupted")`) -- exact enum still not assumed closed
    // beyond these, so an unrecognized non-empty value falls back to the
    // same "finished" treatment a terminal status gets, rather than the
    // time window (any status Dispatcharr bothers to set at all that
    // isn't "scheduled" or "recording" is far more likely to be some
    // future terminal-status variant this addon hasn't been updated to
    // recognize yet than a novel "not started" spelling).
    std::string status = FieldOr<std::string>(custom, "status", "");
    if (status == "recording")
    {
      r.isInProgress = true;
      r.isUpcoming = false;
    }
    else if (status == "scheduled")
    {
      // Fix for a real, confirmed regression on this branch (not yet
      // merged, caught and fixed the same day it was introduced): an
      // earlier version of this block treated ANY non-empty status,
      // including "scheduled", as clearing isUpcoming -- since every
      // future recurring-rule occurrence is created with this exact
      // status well before its own start_time, that made every such
      // occurrence vanish from Kodi's Timers list
      // (IsListedAsTimer() needs isInProgress || isUpcoming) and
      // incorrectly appear in Recordings instead
      // (IsListedAsRecording() returns !isUpcoming), immediately on
      // creation, for every recurring rule.
      r.isUpcoming = true;
      r.isInProgress = false;
    }
    else if (!status.empty())
    {
      r.isInProgress = false;
      r.isUpcoming = false;
    }

    // Fix for a real, confirmed bug found via a project-wide review (a
    // 26th-pass audit), confirmed against Dispatcharr's own real current
    // upstream source, not itself independently reproduced:
    // RecordingViewSet.stop() writes status="stopped" and
    // stopped_at=str(timezone.now()), but deliberately never touches
    // end_time -- so a recording stopped early kept reporting its full,
    // originally-scheduled duration (r.durationSeconds above, computed
    // from the untouched end_time) instead of how long it actually
    // recorded, the same class of bug as isInProgress's own
    // status-override fix above, just for duration instead of the
    // in-progress flag. `stopped_at`'s own string format
    // ("YYYY-MM-DD HH:MM:SS.ffffff+00:00", confirmed against
    // Dispatcharr's own settings.py: TIME_ZONE="UTC", USE_TZ=True) is
    // already handled by TimeFromIso()'s own fixed-digit-position parse,
    // indifferent to the 'T'-vs-space separator -- and, since a 35th-pass
    // audit (2026-09-26), its own trailing "+00:00" is
    // read and applied too, not just skipped; a genuine zero offset, so
    // this still parses the same real instant either way.
    // `stoppedAt > startTime && stoppedAt < endTime` is a sanity check,
    // not just a presence check: an unparseable/missing stopped_at
    // parses to 0 (never > startTime), and a stoppedAt at or past
    // endTime (clock skew, or a stop landing right as it finished
    // naturally) safely keeps the original endTime-based duration
    // instead of a nonsensical or redundant adjustment.
    if (status == "stopped")
    {
      time_t stoppedAt = TimeFromIso(FieldOr<std::string>(custom, "stopped_at", ""));
      if (stoppedAt > r.startTime && stoppedAt < r.endTime)
        r.durationSeconds = ClampedDurationSeconds(r.startTime, stoppedAt);
    }
    // Same fix, same reasoning, for "interrupted" (added 2026-09-26, a
    // 27th-pass audit, fixing a real, confirmed gap in the fix above
    // found via a project-wide review, confirmed against Dispatcharr's
    // own real current upstream source, not itself independently
    // reproduced): a recording whose own stream died partway through
    // (worker shutdown, a `no_stream_data` detection, or a handful of
    // other paths, all in apps/channels/tasks.py) is marked
    // "interrupted", not "stopped" -- also never touching end_time --
    // but recording its own stop time in a *different* field,
    // `custom_properties.ended_at` (also written for every other
    // terminal status as a final metadata step, harmlessly redundant
    // with `stopped_at` there since both would agree). `ended_at` is
    // built from a plain, timezone-naive `datetime.now()`, unlike
    // `stopped_at`'s own timezone-aware `timezone.now()` -- but
    // Dispatcharr's own settings.py re-stamps the process's own `TZ`
    // environment variable to its configured `TIME_ZONE` ("UTC") at
    // startup whenever `USE_TZ=True` (confirmed against Dispatcharr's
    // own real source), so this naive call still returns UTC wall-clock
    // time in practice, just without the `stopped_at`'s own trailing
    // `+00:00`. `TimeFromIso()` reads and applies a trailing offset when
    // present (since a 35th-pass audit) but leaves a
    // value with none as UTC unchanged, so a naive string like this one
    // still parses correctly either way. Same sanity guard as
    // `stopped_at` above.
    else if (status == "interrupted")
    {
      time_t endedAt = TimeFromIso(FieldOr<std::string>(custom, "ended_at", ""));
      if (endedAt > r.startTime && endedAt < r.endTime)
        r.durationSeconds = ClampedDurationSeconds(r.startTime, endedAt);
    }

    // See Recording::hlsDirStillPresent's own comment: present for the
    // whole window between "user stopped it" and "concat + viewer-wait
    // actually finished," regardless of what status already says.
    r.hlsDirStillPresent = custom.contains("_hls_dir") && !custom["_hls_dir"].is_null();

    // custom_properties.bytes_written is only written by Dispatcharr's
    // recording task at finalization (confirmed against its source,
    // apps/channels/tasks.py: summed from HLS segment file sizes and
    // stored into custom_properties only once the task reaches its
    // post-processing step) -- absent while genuinely still recording,
    // hence the 0 default here rather than treating absence as an error.
    r.bytesWritten = FieldOr<int64_t>(custom, "bytes_written", 0);

    const nlohmann::json& program = custom.contains("program") ? custom["program"] : nlohmann::json();
    if (program.is_object())
    {
      r.title = FieldOr<std::string>(program, "title", "");
      r.subtitle = FieldOr<std::string>(program, "sub_title", "");
      r.description = FieldOr<std::string>(program, "description", "");
      // See Recording::programStartTime's own comment (DispatcharrClient.h)
      // -- absent for a recording whose custom_properties.program was
      // never populated with times (this addon's own recordings, both
      // Kodi-created ones and their later server-side enrichment).
      r.programStartTime = TimeFromIso(FieldOr<std::string>(program, "start_time", ""));
      r.programEndTime = TimeFromIso(FieldOr<std::string>(program, "end_time", ""));
      r.programTvgId = FieldOr<std::string>(program, "tvg_id", "");
    }
    if (r.title.empty())
      r.title = FieldOr<std::string>(custom, "title", "");
    if (r.subtitle.empty())
      r.subtitle = FieldOr<std::string>(custom, "sub_title", "");
    if (r.description.empty())
      r.description = FieldOr<std::string>(custom, "description", "");

    // Tagged by Dispatcharr's own recurring-rule scheduler (confirmed
    // against its source: custom_properties.rule =
    // {"type": "recurring", "id": <rule id>, ...}) -- see RecurringRule's
    // own comment for how this links back to its parent rule as a Kodi
    // timer.
    const nlohmann::json& rule = custom.contains("rule") ? custom["rule"] : nlohmann::json();
    if (rule.is_object() && FieldOr<std::string>(rule, "type", "") == "recurring")
      r.recurringRuleId = FieldOr(rule, "id", 0);
  }
  // An occurrence takes its parent rule's name as its title, so a rule the addon owns
  // would put its ownership tag into every recording and timer it generates -- Kodi is
  // shown the name without it (ManagedRecurringRule.h). Only the title shown: the file
  // and folder Dispatcharr writes on the server still carry whatever the rule is called.
  if (r.recurringRuleId != 0)
    r.title = StripManagedRuleMarker(r.title);
  return r;
}

bool ParseRecordingEdlEntryJson(const nlohmann::json& item, RecordingEdlEntry& out)
{
  RecordingEdlEntry entry;
  entry.startMs = FieldOr<int64_t>(item, "start", 0);
  entry.endMs = FieldOr<int64_t>(item, "end", 0);
  entry.type = FieldOr(item, "type", 3);
  // Kodi's own PVR_EDL_TYPE (an unscoped enum with no fixed underlying
  // type) only defines 0-3 -- PVRDispatcharr::GetRecordingEdl() casts
  // this value straight into it via static_cast<PVR_EDL_TYPE>(), which
  // is undefined behavior in C++ for a value outside an unscoped enum's
  // defined range. This addon's own recording_edl companion plugin is
  // the only realistic producer of this field, but a parser reading
  // externally-sourced data (an .edl sidecar file, ultimately) shouldn't
  // assume it's always well-formed -- coerce anything outside 0-3 to 3
  // (COMBREAK), matching the same default this field already falls back
  // to when it's absent entirely. Found via a project-wide review, not
  // reproduced live.
  if (entry.type < 0 || entry.type > 3)
    entry.type = 3;
  if (entry.endMs <= entry.startMs)
    return false;
  out = entry;
  return true;
}

} // namespace dispatcharr
