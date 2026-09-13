#include "RealtimeUpdateParser.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ParseRelevantRealtimeUpdateEventType returns the event type for every relevant event name",
          "[RealtimeUpdateParser]")
{
  for (const std::string& eventType :
       {"recording_started", "recording_ended", "recording_stopped", "recording_extended", "recording_updated",
        "recording_cancelled", "recordings_refreshed"})
  {
    std::string message = R"({"type": "update", "data": {"type": ")" + eventType + R"("}})";
    CHECK(ParseRelevantRealtimeUpdateEventType(message) == eventType);
  }
}

TEST_CASE("ParseRelevantRealtimeUpdateEventType returns empty for an irrelevant event on the same channel",
          "[RealtimeUpdateParser]")
{
  // Real irrelevant event names sharing Dispatcharr's "updates" channel:
  // EPG matching progress, M3U refresh, stream stats.
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": {"type": "epg_match_progress"}})").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": {"type": "m3u_refresh"}})").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": {"type": "stream_stats"}})").empty());
}

TEST_CASE("ParseRelevantRealtimeUpdateEventType returns empty for malformed JSON", "[RealtimeUpdateParser]")
{
  CHECK(ParseRelevantRealtimeUpdateEventType("").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType("not json").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType("{").empty());
}

TEST_CASE("ParseRelevantRealtimeUpdateEventType returns empty when data is missing or not an object",
          "[RealtimeUpdateParser]")
{
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update"})").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": "not an object"})").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": null})").empty());
}

TEST_CASE("ParseRelevantRealtimeUpdateEventType returns empty when data.type is missing or not a string",
          "[RealtimeUpdateParser]")
{
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": {}})").empty());
  CHECK(ParseRelevantRealtimeUpdateEventType(R"({"type": "update", "data": {"type": 42}})").empty());
}
