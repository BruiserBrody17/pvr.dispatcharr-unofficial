#include "RecordingParser.h"

#include "DateTimeFormat.h"
#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

Recording ParseRecordingFields(const nlohmann::json& item, time_t now)
{
  Recording r;
  r.id = FieldOr(item, "id", 0);
  r.channelId = FieldOr(item, "channel", FieldOr(item, "channel_id", 0));
  r.startTime = TimeFromIso(FieldOr<std::string>(item, "start_time", ""));
  r.endTime = TimeFromIso(FieldOr<std::string>(item, "end_time", ""));
  r.durationSeconds = (r.endTime > r.startTime) ? static_cast<int>(r.endTime - r.startTime) : 0;
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
    // stopped. Confirmed values: "recording" (still active), "completed"/
    // "stopped"/"interrupted" (all finished, one way or another) --
    // exact enum not documented, so only treat "recording" as
    // authoritative for in-progress and fall back to the time window for
    // anything else/absent, rather than assuming a closed list.
    std::string status = FieldOr<std::string>(custom, "status", "");
    if (status == "recording")
      r.isInProgress = true;
    else if (!status.empty())
      r.isInProgress = false;

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
  return r;
}

} // namespace dispatcharr
