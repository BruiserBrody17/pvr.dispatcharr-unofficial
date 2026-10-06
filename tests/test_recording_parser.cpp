#include "RecordingParser.h"

#include "DateTimeFormat.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

namespace
{
constexpr time_t kStart = 1767225600; // 2026-01-01T00:00:00Z
constexpr time_t kEnd = 1767229200;   // 2026-01-01T01:00:00Z
} // namespace

TEST_CASE("ParseRecordingFields maps id/channel/times/duration from the bare fields", "[RecordingParser]")
{
  json item = {
      {"id", 42}, {"channel", 7}, {"start_time", "2026-01-01T00:00:00Z"}, {"end_time", "2026-01-01T01:00:00Z"}};

  Recording r = ParseRecordingFields(item, kStart + 60);

  CHECK(r.id == 42);
  CHECK(r.channelId == 7);
  CHECK(r.startTime == kStart);
  CHECK(r.endTime == kEnd);
  CHECK(r.durationSeconds == 3600);
}

TEST_CASE("ParseRecordingFields falls back to channel_id when channel is absent", "[RecordingParser]")
{
  json item = {{"channel_id", 9}, {"start_time", "2026-01-01T00:00:00Z"}, {"end_time", "2026-01-01T01:00:00Z"}};

  Recording r = ParseRecordingFields(item, kStart);

  CHECK(r.channelId == 9);
}

TEST_CASE("ParseRecordingFields derives isInProgress/isUpcoming from the time window with no custom_properties",
          "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"}, {"end_time", "2026-01-01T01:00:00Z"}};

  Recording before = ParseRecordingFields(item, kStart - 60);
  CHECK_FALSE(before.isInProgress);
  CHECK(before.isUpcoming);

  Recording during = ParseRecordingFields(item, kStart + 60);
  CHECK(during.isInProgress);
  CHECK_FALSE(during.isUpcoming);

  Recording after = ParseRecordingFields(item, kEnd + 60);
  CHECK_FALSE(after.isInProgress);
  CHECK_FALSE(after.isUpcoming);
}

TEST_CASE("ParseRecordingFields trusts custom_properties.status over a stale time window when stopped early",
          "[RecordingParser]")
{
  // The exact documented incident (docs/RECORDINGS.md): a recording
  // stopped early keeps its originally-scheduled end_time untouched, so
  // `now` still falls inside [start_time, end_time) even though the
  // recording actually finished already -- status must win.
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "stopped"}}}};

  Recording r = ParseRecordingFields(item, kStart + 60); // still inside the window

  CHECK_FALSE(r.isInProgress);
}

TEST_CASE("ParseRecordingFields uses stopped_at for duration when stopped early -- the real bug this fixes",
          "[RecordingParser]")
{
  // RecordingViewSet.stop() writes status="stopped" and
  // stopped_at=str(timezone.now()) but never touches end_time -- without
  // this, durationSeconds kept reporting the full, originally-scheduled
  // length instead of how long it actually recorded.
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "stopped"}, {"stopped_at", "2026-01-01 00:10:00.123456+00:00"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 600); // 10 minutes, not the full scheduled hour
}

TEST_CASE("ParseRecordingFields keeps the scheduled duration when stopped_at is missing", "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "stopped"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 3600);
}

TEST_CASE("ParseRecordingFields keeps the scheduled duration when stopped_at is at or past end_time",
          "[RecordingParser]")
{
  // Clock skew, or a stop landing right as the recording finished
  // naturally -- the sanity check (stoppedAt < endTime) keeps the
  // original, still-correct duration rather than a nonsensical one.
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "stopped"}, {"stopped_at", "2026-01-01 01:05:00+00:00"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 3600);
}

TEST_CASE("ParseRecordingFields does not apply the stopped_at adjustment for a non-stopped status", "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "completed"}, {"stopped_at", "2026-01-01 00:10:00+00:00"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 3600);
}

TEST_CASE("ParseRecordingFields uses ended_at for duration when interrupted -- the real bug this fixes",
          "[RecordingParser]")
{
  // A recording whose stream died partway through is marked
  // "interrupted", not "stopped", but also never gets end_time touched --
  // it records its own stop time in ended_at instead (also never
  // timezone-suffixed, since it's built from a naive datetime.now() --
  // but Dispatcharr's own process re-stamps its TZ env var to UTC at
  // startup, so it's still a UTC wall-clock value in practice).
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "interrupted"}, {"ended_at", "2026-01-01 00:15:00.654321"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 900); // 15 minutes, not the full scheduled hour
}

TEST_CASE("ParseRecordingFields keeps the scheduled duration for interrupted when ended_at is missing",
          "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "interrupted"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 3600);
}

TEST_CASE("ParseRecordingFields does not apply the ended_at adjustment for a stopped status (stopped_at wins there)",
          "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "stopped"}, {"ended_at", "2026-01-01 00:15:00"}}}};

  Recording r = ParseRecordingFields(item, kStart + 3600);

  CHECK(r.durationSeconds == 3600); // stopped_at (absent here) is what "stopped" checks, not ended_at
}

TEST_CASE("ParseRecordingFields trusts custom_properties.status == recording even outside the time window",
          "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "recording"}}}};

  Recording r = ParseRecordingFields(item, kEnd + 60); // past the scheduled end

  CHECK(r.isInProgress);
}

TEST_CASE("ParseRecordingFields leaves isInProgress on the time window for an unrecognized/absent status",
          "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", json::object()}};

  Recording r = ParseRecordingFields(item, kStart + 60);

  CHECK(r.isInProgress); // no status key at all -- falls back to the time window
}

TEST_CASE("ParseRecordingFields clears isUpcoming when status==recording, even if the local clock lags",
          "[RecordingParser]")
{
  // Real, confirmed inconsistency this fixes: a Kodi clock running
  // slightly behind Dispatcharr's own server clock could otherwise
  // leave isUpcoming=true (startTime still reads as in the future per
  // this addon's own lagging `now`) at the exact same moment status ==
  // "recording" forces isInProgress=true -- both flags true at once,
  // which hides an actively-recording item from the Recordings list
  // (IsListedAsRecording() returns !isUpcoming).
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "recording"}}}};

  Recording r = ParseRecordingFields(item, kStart - 60); // "now" lags behind startTime

  CHECK(r.isInProgress);
  CHECK_FALSE(r.isUpcoming);
}

TEST_CASE("ParseRecordingFields clears isUpcoming for any confirmed-terminal status too", "[RecordingParser]")
{
  // The reverse clock-skew direction: a terminal status can't mean
  // "not yet started" regardless of what a stale/skewed clock says
  // about startTime.
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "stopped"}}}};

  Recording r = ParseRecordingFields(item, kStart - 60); // "now" is before startTime

  CHECK_FALSE(r.isInProgress);
  CHECK_FALSE(r.isUpcoming);
}

TEST_CASE("ParseRecordingFields leaves isUpcoming on the time window for an absent status", "[RecordingParser]")
{
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", json::object()}};

  Recording r = ParseRecordingFields(item, kStart - 60);

  CHECK(r.isUpcoming); // no status key at all -- falls back to the time window
}

TEST_CASE("ParseRecordingFields treats status==scheduled as authoritatively upcoming, not finished -- the real "
          "regression this fixes",
          "[RecordingParser]")
{
  // Confirmed directly against Dispatcharr's own real source
  // (apps/channels/tasks.py's sync_recurring_rule_impl()): every future
  // recurring-rule occurrence is created with this exact status, well
  // before its own start_time (`if start_dt <= now: continue` gates it).
  // A real, confirmed regression on this branch (not yet merged, caught
  // and fixed the same day it was introduced): an earlier version
  // treated any non-empty status, including "scheduled", as clearing
  // isUpcoming -- making every future occurrence of every recurring rule
  // vanish from Kodi's Timers list (IsListedAsTimer() needs isInProgress
  // || isUpcoming) and incorrectly appear in Recordings instead
  // (IsListedAsRecording() returns !isUpcoming), immediately on
  // creation.
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "scheduled"}}}};

  Recording r = ParseRecordingFields(item, kStart - 60); // well before start_time, as it always is when this is set

  CHECK(r.isUpcoming);
  CHECK_FALSE(r.isInProgress);
}

TEST_CASE("ParseRecordingFields treats status==scheduled as authoritatively upcoming even if the local clock lags "
          "past start_time",
          "[RecordingParser]")
{
  // The same clock-skew direction the status==recording fix above
  // already guards against, applied to "scheduled" instead: a
  // genuinely-not-yet-started occurrence must not read as in-progress
  // just because a lagging Kodi clock already passed its scheduled
  // start_time.
  json item = {{"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"status", "scheduled"}}}};

  Recording r = ParseRecordingFields(item, kStart + 60); // "now" reads past start_time

  CHECK(r.isUpcoming);
  CHECK_FALSE(r.isInProgress);
}

TEST_CASE("ParseRecordingFields sets hlsDirStillPresent only when _hls_dir is a non-null value", "[RecordingParser]")
{
  json present = {{"custom_properties", {{"_hls_dir", "/data/recordings/.dvr_1_hls"}}}};
  json nullValue = {{"custom_properties", {{"_hls_dir", nullptr}}}};
  json absent = {{"custom_properties", json::object()}};

  CHECK(ParseRecordingFields(present, 0).hlsDirStillPresent);
  CHECK_FALSE(ParseRecordingFields(nullValue, 0).hlsDirStillPresent);
  CHECK_FALSE(ParseRecordingFields(absent, 0).hlsDirStillPresent);
}

TEST_CASE("ParseRecordingFields defaults bytesWritten to 0 until Dispatcharr's finalization writes it",
          "[RecordingParser]")
{
  json withBytes = {{"custom_properties", {{"bytes_written", 123456}}}};
  json without = {{"custom_properties", json::object()}};

  CHECK(ParseRecordingFields(withBytes, 0).bytesWritten == 123456);
  CHECK(ParseRecordingFields(without, 0).bytesWritten == 0);
}

TEST_CASE("ParseRecordingFields prefers custom_properties.program's title/subtitle/description", "[RecordingParser]")
{
  json item = {
      {"custom_properties",
       {{"program", {{"title", "Programme Title"}, {"sub_title", "Episode Name"}, {"description", "Synopsis"}}},
        {"title", "Flat Title"}}}};

  Recording r = ParseRecordingFields(item, 0);

  CHECK(r.title == "Programme Title");
  CHECK(r.subtitle == "Episode Name");
  CHECK(r.description == "Synopsis");
}

TEST_CASE("ParseRecordingFields reads the programme's own never-padded start/end time from "
          "custom_properties.program",
          "[RecordingParser]")
{
  // Dispatcharr's own web UI Guide "Record" button always sends
  // custom_properties.program with the raw EPG programme's own
  // start_time/end_time -- confirmed against Dispatcharr's own real
  // current upstream source, not itself independently reproduced. This
  // is the one place this addon can recover the true programme window
  // independent of RecordingSerializer.validate()'s own pre/post-padding
  // and past-start clamping (both applied to start_time/end_time
  // themselves, never to custom_properties.program).
  json item = {{"start_time", "2026-01-01T20:05:00Z"}, // clamped: pressed 5 min into the programme
               {"end_time", "2026-01-01T21:05:00Z"},   // padded: +5 min post-padding
               {"custom_properties",
                {{"program",
                  {{"title", "Programme Title"},
                   {"start_time", "2026-01-01T20:00:00Z"},
                   {"end_time", "2026-01-01T21:00:00Z"}}}}}};

  Recording r = ParseRecordingFields(item, 0);

  CHECK(r.programStartTime == TimeFromIso("2026-01-01T20:00:00Z"));
  CHECK(r.programEndTime == TimeFromIso("2026-01-01T21:00:00Z"));
  // The recording's own (clamped/padded) window is untouched by this.
  CHECK(r.startTime == TimeFromIso("2026-01-01T20:05:00Z"));
  CHECK(r.endTime == TimeFromIso("2026-01-01T21:05:00Z"));
}

TEST_CASE("ParseRecordingFields correctly applies a non-UTC offset in a recurring-rule occurrence's own "
          "custom_properties.program.start_time -- the real bug a 35th-pass audit fixes",
          "[RecordingParser]")
{
  // Confirmed against Dispatcharr's own real current upstream source, not
  // itself independently reproduced: sync_recurring_rule_impl() (apps/
  // channels/tasks.py) builds a recurring-rule occurrence's own
  // custom_properties.program.start_time from a timezone-*aware* Python
  // datetime's own isoformat() call, in that rule's own configured
  // system timezone (America/New_York, EDT -04:00 here) -- not UTC. This
  // must resolve to the same real instant as the equivalent Z-suffixed
  // value, not silently land 4 hours off.
  json item = {{"custom_properties", {{"program", {{"start_time", "2026-09-28T20:00:00-04:00"}}}}}};

  Recording r = ParseRecordingFields(item, 0);

  CHECK(r.programStartTime == TimeFromIso("2026-09-29T00:00:00Z"));
}

TEST_CASE("ParseRecordingFields leaves programStartTime/programEndTime at 0 when program has no times",
          "[RecordingParser]")
{
  // This addon's own recordings: CreateOneTimeRecording() never sends
  // custom_properties.program at all, and Dispatcharr's own later
  // server-side enrichment for those only ever backfills
  // id/title/sub_title/description, never times.
  json item = {{"custom_properties", {{"program", {{"title", "Programme Title"}}}}}};

  Recording r = ParseRecordingFields(item, 0);

  CHECK(r.programStartTime == 0);
  CHECK(r.programEndTime == 0);
}

TEST_CASE("ParseRecordingFields falls back to the flat custom_properties fields when program is absent",
          "[RecordingParser]")
{
  json item = {{"custom_properties",
                {{"title", "Flat Title"}, {"sub_title", "Flat Subtitle"}, {"description", "Flat Description"}}}};

  Recording r = ParseRecordingFields(item, 0);

  CHECK(r.title == "Flat Title");
  CHECK(r.subtitle == "Flat Subtitle");
  CHECK(r.description == "Flat Description");
}

TEST_CASE("ParseRecordingFields leaves title empty when neither source has one -- caller applies its own fallback",
          "[RecordingParser]")
{
  json item = {{"custom_properties", json::object()}};

  Recording r = ParseRecordingFields(item, 0);

  CHECK(r.title.empty());
}

TEST_CASE("ParseRecordingFields sets recurringRuleId only for a rule of type recurring", "[RecordingParser]")
{
  json recurring = {{"custom_properties", {{"rule", {{"type", "recurring"}, {"id", 5}}}}}};
  json otherType = {{"custom_properties", {{"rule", {{"type", "series"}, {"id", 5}}}}}};
  json absent = {{"custom_properties", json::object()}};

  CHECK(ParseRecordingFields(recurring, 0).recurringRuleId == 5);
  CHECK(ParseRecordingFields(otherType, 0).recurringRuleId == 0);
  CHECK(ParseRecordingFields(absent, 0).recurringRuleId == 0);
}

// ---------------------------------------------------------------------
// ParseRecordingEdlEntryJson
// ---------------------------------------------------------------------

TEST_CASE("ParseRecordingEdlEntryJson maps start/end/type", "[RecordingParser]")
{
  json item = {{"start", 1000}, {"end", 5000}, {"type", 1}};

  RecordingEdlEntry entry;
  bool ok = ParseRecordingEdlEntryJson(item, entry);

  REQUIRE(ok);
  CHECK(entry.startMs == 1000);
  CHECK(entry.endMs == 5000);
  CHECK(entry.type == 1);
}

TEST_CASE("ParseRecordingEdlEntryJson defaults type to 3 (Kodi's PVR_EDL_TYPE_COMBREAK) when absent",
          "[RecordingParser]")
{
  json item = {{"start", 1000}, {"end", 5000}};

  RecordingEdlEntry entry;
  ParseRecordingEdlEntryJson(item, entry);

  CHECK(entry.type == 3);
}

TEST_CASE("ParseRecordingEdlEntryJson rejects an entry whose end isn't after its start", "[RecordingParser]")
{
  json equal = {{"start", 1000}, {"end", 1000}};
  json reversed = {{"start", 5000}, {"end", 1000}};

  RecordingEdlEntry entry;
  CHECK_FALSE(ParseRecordingEdlEntryJson(equal, entry));
  CHECK_FALSE(ParseRecordingEdlEntryJson(reversed, entry));
}

TEST_CASE("ParseRecordingEdlEntryJson defaults missing start/end to 0, which is then rejected", "[RecordingParser]")
{
  RecordingEdlEntry entry;
  CHECK_FALSE(ParseRecordingEdlEntryJson(json::object(), entry));
}

TEST_CASE("ParseRecordingEdlEntryJson coerces an out-of-range type to 3 (COMBREAK)", "[RecordingParser]")
{
  // Kodi's own PVR_EDL_TYPE (an unscoped enum with no fixed underlying
  // type) only defines 0-3 -- PVRDispatcharr::GetRecordingEdl() casts
  // this value straight into it, undefined behavior in C++ for a value
  // outside an unscoped enum's defined range. A real, confirmed gap
  // found via a project-wide review, not reproduced live.
  RecordingEdlEntry entry;

  json negative = {{"start", 1000}, {"end", 5000}, {"type", -1}};
  REQUIRE(ParseRecordingEdlEntryJson(negative, entry));
  CHECK(entry.type == 3);

  json tooLarge = {{"start", 1000}, {"end", 5000}, {"type", 999}};
  REQUIRE(ParseRecordingEdlEntryJson(tooLarge, entry));
  CHECK(entry.type == 3);
}

TEST_CASE("ParseRecordingFields strips the addon's ownership tag from an occurrence's title", "[RecordingParser]")
{
  json item = {
      {"id", 5},
      {"channel", 7},
      {"start_time", "2026-01-01T00:00:00Z"},
      {"end_time", "2026-01-01T01:00:00Z"},
      {"custom_properties",
       {{"program", {{"title", "Evening News at 10pm [Kodi]"}}}, {"rule", {{"type", "recurring"}, {"id", 14}}}}}};

  Recording r = ParseRecordingFields(item, kStart);

  CHECK(r.title == "Evening News at 10pm");
  CHECK(r.recurringRuleId == 14);
}

TEST_CASE("ParseRecordingFields leaves a title alone when the recording does not come from a recurring rule",
          "[RecordingParser]")
{
  // A programme that really is called that, recorded by hand or by a series rule.
  json item = {{"id", 5},
               {"channel", 7},
               {"start_time", "2026-01-01T00:00:00Z"},
               {"end_time", "2026-01-01T01:00:00Z"},
               {"custom_properties", {{"program", {{"title", "Show [Kodi]"}}}}}};

  CHECK(ParseRecordingFields(item, kStart).title == "Show [Kodi]");
}

TEST_CASE("ParseRecordingFields leaves an untagged occurrence title alone", "[RecordingParser]")
{
  json item = {
      {"id", 5},
      {"channel", 7},
      {"start_time", "2026-01-01T00:00:00Z"},
      {"end_time", "2026-01-01T01:00:00Z"},
      {"custom_properties", {{"program", {{"title", "Evening News"}}}, {"rule", {{"type", "recurring"}, {"id", 3}}}}}};

  CHECK(ParseRecordingFields(item, kStart).title == "Evening News");
}

TEST_CASE("ParseRecordingFields reads the programme's tvg_id from its snapshot", "[RecordingParser]")
{
  // The identity a series rule with no pinned channel has instead of a channel:
  // MatchRecordingsToSeriesRules() links such a recording back to its rule by it.
  json item = {{"custom_properties", {{"program", {{"title", "Programme Title"}, {"tvg_id", "guide.id.1"}}}}}};
  CHECK(ParseRecordingFields(item, 0).programTvgId == "guide.id.1");

  // Absent, null or not a string: empty, never an error.
  json none = {{"custom_properties", {{"program", {{"title", "Programme Title"}}}}}};
  CHECK(ParseRecordingFields(none, 0).programTvgId.empty());
  json nullId = {{"custom_properties", {{"program", {{"tvg_id", nullptr}}}}}};
  CHECK(ParseRecordingFields(nullId, 0).programTvgId.empty());
  json noProgram = {{"custom_properties", {{"title", "Programme Title"}}}};
  CHECK(ParseRecordingFields(noProgram, 0).programTvgId.empty());
}

TEST_CASE("ClampedDurationSeconds is the whole seconds, never negative, never wrapped", "[RecordingParser]")
{
  CHECK(ClampedDurationSeconds(1000, 4600) == 3600);
  CHECK(ClampedDurationSeconds(1000, 1000) == 0);
  CHECK(ClampedDurationSeconds(1000, 500) == 0);
  CHECK(ClampedDurationSeconds(0, 2147483647) == 2147483647);
  if (sizeof(time_t) > 4)
  {
    // A row ending past about 2094 used to wrap to a negative int.
    CHECK(ClampedDurationSeconds(1000, 4102444800LL * 2) == 2147483647);
    CHECK(ClampedDurationSeconds(0, 2147483648LL) == 2147483647);
  }
}

TEST_CASE("IsRecordingResponseFor accepts only an object describing that exact recording", "[RecordingParser]")
{
  CHECK(IsRecordingResponseFor(nlohmann::json::parse(R"({"id":42,"channel":1})"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse(R"({"id":42})"), 43));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse("{}"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse("[]"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse("[{\"id\":42}]"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse("null"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse(R"({"id":"42"})"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse(R"({"id":null})"), 42));
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse(R"({"id":42.5})"), 42));
  // An id of 0 is never a recording.
  CHECK_FALSE(IsRecordingResponseFor(nlohmann::json::parse(R"({"id":0})"), 0));
}

TEST_CASE("ParseRecordingFields decides in-progress on exact start and end boundaries", "[RecordingParser]")
{
  const time_t start = 1767268800; // 2026-01-01T12:00:00Z
  const time_t end = start + 3600;
  auto item =
      nlohmann::json::parse(R"({"id":1,"start_time":"2026-01-01T12:00:00Z","end_time":"2026-01-01T13:00:00Z"})");
  // now == start: in progress (the window is closed at its start)...
  CHECK(ParseRecordingFields(item, start).isInProgress);
  CHECK_FALSE(ParseRecordingFields(item, start).isUpcoming);
  CHECK_FALSE(ParseRecordingFields(item, start - 1).isInProgress);
  CHECK(ParseRecordingFields(item, start - 1).isUpcoming);
  // ...and now == end: finished (open at its end).
  CHECK(ParseRecordingFields(item, end - 1).isInProgress);
  CHECK_FALSE(ParseRecordingFields(item, end).isInProgress);
  CHECK_FALSE(ParseRecordingFields(item, end).isUpcoming);
}

TEST_CASE("ParseRecordingFields never reads an unparseable start time as in progress", "[RecordingParser]")
{
  auto item = nlohmann::json::parse(R"({"id":1,"start_time":"garbage","end_time":"2026-01-01T13:00:00Z"})");
  const Recording r = ParseRecordingFields(item, 1767268800);
  CHECK(r.startTime == 0);
  CHECK_FALSE(r.isInProgress); // a start of 0 is "unknown", not "started long ago"
}

TEST_CASE("ParseRecordingFields keeps the scheduled duration when stopped_at or ended_at equals the start",
          "[RecordingParser]")
{
  // A stop timestamp that is not after the start is no real recording time: the scheduled length stays
  // rather than collapsing to a zero-length recording.
  json stopped = {{"start_time", "2026-01-01T00:00:00Z"},
                  {"end_time", "2026-01-01T01:00:00Z"},
                  {"custom_properties", {{"status", "stopped"}, {"stopped_at", "2026-01-01 00:00:00+00:00"}}}};
  CHECK(ParseRecordingFields(stopped, kStart + 3600).durationSeconds == 3600);
  json interrupted = {{"start_time", "2026-01-01T00:00:00Z"},
                      {"end_time", "2026-01-01T01:00:00Z"},
                      {"custom_properties", {{"status", "interrupted"}, {"ended_at", "2026-01-01 00:00:00+00:00"}}}};
  CHECK(ParseRecordingFields(interrupted, kStart + 3600).durationSeconds == 3600);
}

TEST_CASE("ParseRecordingEdlEntryJson keeps a cut (type 0) and turns an out-of-range type into a commercial break",
          "[RecordingParser]")
{
  RecordingEdlEntry entry;
  REQUIRE(ParseRecordingEdlEntryJson(json{{"start", 1000}, {"end", 5000}, {"type", 0}}, entry));
  CHECK(entry.type == 0); // a cut, not a commercial break
  REQUIRE(ParseRecordingEdlEntryJson(json{{"start", 1000}, {"end", 5000}, {"type", 3}}, entry));
  CHECK(entry.type == 3);
  REQUIRE(ParseRecordingEdlEntryJson(json{{"start", 1000}, {"end", 5000}, {"type", -1}}, entry));
  CHECK(entry.type == 3);
  REQUIRE(ParseRecordingEdlEntryJson(json{{"start", 1000}, {"end", 5000}, {"type", 4}}, entry));
  CHECK(entry.type == 3);
}
