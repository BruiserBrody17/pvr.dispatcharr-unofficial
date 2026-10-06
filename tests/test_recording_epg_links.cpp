#include "RecordingEpgLinks.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
RecordingEpgLink Link(int channel = 7, time_t recStart = 1767225600, time_t progStart = 1767225000)
{
  RecordingEpgLink l;
  l.channelId = channel;
  l.recordingStartTime = recStart;
  l.programStartTime = progStart;
  return l;
}
} // namespace

TEST_CASE("links survive a serialize/parse round trip", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap links;
  links[12] = Link(7, 1767225600, 1767225000);
  links[40] = Link(9, 1767300000, 1767299400);

  RecordingEpgLinkMap parsed;
  REQUIRE(ParseRecordingEpgLinks(SerializeRecordingEpgLinks(links), parsed));
  REQUIRE(parsed.size() == 2);
  CHECK(parsed[12].channelId == 7);
  CHECK(parsed[12].recordingStartTime == 1767225600);
  CHECK(parsed[12].programStartTime == 1767225000);
  CHECK(parsed[40].programStartTime == 1767299400);
}

TEST_CASE("an empty map round-trips", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap parsed;
  parsed[1] = Link();
  REQUIRE(ParseRecordingEpgLinks(SerializeRecordingEpgLinks({}), parsed));
  CHECK(parsed.empty());
}

TEST_CASE("unreadable text yields an empty map and false, never a half-read one", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap parsed;
  parsed[1] = Link();
  CHECK_FALSE(ParseRecordingEpgLinks("", parsed));
  CHECK(parsed.empty());
  CHECK_FALSE(ParseRecordingEpgLinks("{\"version\":1,\"links\":[{\"id\":", parsed)); // truncated by a crash
  CHECK_FALSE(ParseRecordingEpgLinks("not json", parsed));
  CHECK_FALSE(ParseRecordingEpgLinks("[]", parsed));
  CHECK_FALSE(ParseRecordingEpgLinks("{\"version\":2,\"links\":[]}", parsed)); // a format this build does not know
  CHECK_FALSE(ParseRecordingEpgLinks("{\"version\":1}", parsed));
  CHECK(parsed.empty());
}

TEST_CASE("a malformed entry is skipped and the rest kept", "[RecordingEpgLinks]")
{
  const std::string text = "{\"version\":1,\"links\":["
                           "{\"id\":5,\"channel\":7,\"recording_start\":100,\"program_start\":90},"
                           "\"junk\","
                           "{\"id\":0,\"channel\":7,\"recording_start\":100,\"program_start\":90},"
                           "{\"id\":6,\"channel\":7,\"recording_start\":100},"
                           "{\"id\":8,\"channel\":-1,\"recording_start\":100,\"program_start\":90},"
                           "{\"id\":9,\"channel\":3,\"recording_start\":200,\"program_start\":190}]}";
  RecordingEpgLinkMap parsed;
  REQUIRE(ParseRecordingEpgLinks(text, parsed));
  CHECK(parsed.size() == 2);
  CHECK(parsed.count(5));
  CHECK(parsed.count(9));
}

TEST_CASE("RememberRecordingEpgLink reports whether anything changed", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap links;
  CHECK(RememberRecordingEpgLink(links, 12, Link()));
  CHECK_FALSE(RememberRecordingEpgLink(links, 12, Link()));                    // same again: nothing to save
  CHECK(RememberRecordingEpgLink(links, 12, Link(7, 1767225600, 1767225300))); // a different programme
  CHECK(links[12].programStartTime == 1767225300);
}

TEST_CASE("RememberRecordingEpgLink refuses entries it could never use", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap links;
  CHECK_FALSE(RememberRecordingEpgLink(links, 0, Link()));
  CHECK_FALSE(RememberRecordingEpgLink(links, 12, Link(0)));
  CHECK_FALSE(RememberRecordingEpgLink(links, 12, Link(7, 0, 100)));
  CHECK_FALSE(RememberRecordingEpgLink(links, 12, Link(7, 100, 0)));
  CHECK(links.empty());
}

TEST_CASE("the map never grows past its cap", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap links;
  for (std::size_t i = 1; i <= kMaxRecordingEpgLinks; ++i)
    links[static_cast<int>(i)] = Link();
  CHECK_FALSE(RememberRecordingEpgLink(links, static_cast<int>(kMaxRecordingEpgLinks) + 1, Link()));
  CHECK(links.size() == kMaxRecordingEpgLinks);
  // An existing entry can still be updated at the cap.
  CHECK(RememberRecordingEpgLink(links, 1, Link(7, 1767225600, 1767225300)));
}

TEST_CASE("a remembered start is returned only for the recording it was made for", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap links;
  links[12] = Link(7, 1767225600, 1767225000);
  CHECK(LookupRememberedProgramStart(links, 12, 7, 1767225600) == 1767225000);
  CHECK(LookupRememberedProgramStart(links, 13, 7, 1767225600) == 0); // unknown recording
  CHECK(LookupRememberedProgramStart(links, 12, 8, 1767225600) == 0); // moved to another channel
  CHECK(LookupRememberedProgramStart(links, 12, 7, 1767225700) == 0); // rescheduled / id reused
}

TEST_CASE("pruning drops only recordings that no longer exist", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap links;
  links[1] = Link();
  links[2] = Link();
  links[3] = Link();
  CHECK(PruneRecordingEpgLinks(links, {1, 3, 99}) == 1);
  CHECK(links.count(1));
  CHECK_FALSE(links.count(2));
  CHECK(links.count(3));
  CHECK(PruneRecordingEpgLinks(links, {1, 3}) == 0);
}

TEST_CASE("a links member that is not an array is unreadable", "[RecordingEpgLinks]")
{
  RecordingEpgLinkMap parsed;
  parsed[1] = Link();
  CHECK_FALSE(ParseRecordingEpgLinks("{\"version\":1,\"links\":{}}", parsed));
  CHECK_FALSE(ParseRecordingEpgLinks("{\"version\":1,\"links\":null}", parsed));
  CHECK(parsed.empty());
}

TEST_CASE("a later entry for the same recording id replaces an earlier one", "[RecordingEpgLinks]")
{
  const std::string text = "{\"version\":1,\"links\":["
                           "{\"id\":5,\"channel\":7,\"recording_start\":100,\"program_start\":90},"
                           "{\"id\":5,\"channel\":8,\"recording_start\":200,\"program_start\":190}]}";
  RecordingEpgLinkMap parsed;
  REQUIRE(ParseRecordingEpgLinks(text, parsed));
  REQUIRE(parsed.size() == 1);
  CHECK(parsed[5].channelId == 8);
  CHECK(parsed[5].recordingStartTime == 200);
}

TEST_CASE("a file with more entries than the cap loads only the cap's worth", "[RecordingEpgLinks]")
{
  // A hand-edited or corrupt file must not be able to grow the in-memory map without bound.
  std::string text = "{\"version\":1,\"links\":[";
  for (std::size_t i = 1; i <= kMaxRecordingEpgLinks + 5; ++i)
  {
    if (i > 1)
      text += ",";
    text += "{\"id\":" + std::to_string(i) + ",\"channel\":7,\"recording_start\":100,\"program_start\":90}";
  }
  text += "]}";
  RecordingEpgLinkMap parsed;
  REQUIRE(ParseRecordingEpgLinks(text, parsed));
  CHECK(parsed.size() == kMaxRecordingEpgLinks);
  CHECK(parsed.count(1) == 1);
  CHECK(parsed.count(static_cast<int>(kMaxRecordingEpgLinks) + 1) == 0);
}

TEST_CASE("a persisted link for channel 0 or a negative channel is dropped on load", "[RecordingEpgLinks]")
{
  const std::string text = "{\"version\":1,\"links\":["
                           "{\"id\":5,\"channel\":0,\"recording_start\":100,\"program_start\":90},"
                           "{\"id\":6,\"channel\":-3,\"recording_start\":100,\"program_start\":90},"
                           "{\"id\":7,\"channel\":1,\"recording_start\":100,\"program_start\":90}]}";
  RecordingEpgLinkMap parsed;
  REQUIRE(ParseRecordingEpgLinks(text, parsed));
  CHECK(parsed.size() == 1);
  CHECK(parsed.count(7) == 1);
}
