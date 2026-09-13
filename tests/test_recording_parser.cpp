#include "RecordingParser.h"

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
