#include "XmlTvParser.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{

const char* kFullyPopulatedDocument = R"(<?xml version="1.0" encoding="UTF-8"?>
<tv>
  <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
    <title>Channel A News</title>
    <sub-title>Evening Edition</sub-title>
    <desc>A description of the programme.</desc>
    <category>News</category>
    <category>Talk</category>
    <icon src="https://example.invalid/icon.png"/>
    <credits>
      <director>Alice Director</director>
      <writer>Bob Writer</writer>
      <adapter>Carol Adapter</adapter>
      <actor>Dave Actor</actor>
      <presenter>Eve Presenter</presenter>
    </credits>
    <date>2025-12-25</date>
    <new/>
    <premiere/>
    <live/>
    <episode-num system="xmltv_ns">2.4.0/2</episode-num>
  </programme>
</tv>
)";

} // namespace

TEST_CASE("Parses a fully-populated programme entry", "[XmlTvParser]")
{
  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(kFullyPopulatedDocument, out, error));
  CHECK(error.empty());
  REQUIRE(out.size() == 1);
  REQUIRE(out.count("1") == 1);
  REQUIRE(out.at("1").size() == 1);

  const EpgEntry& entry = out.at("1")[0];
  CHECK(entry.channelXmltvId == "1");
  CHECK(entry.title == "Channel A News");
  CHECK(entry.subtitle == "Evening Edition");
  CHECK(entry.description == "A description of the programme.");
  REQUIRE(entry.categories.size() == 2);
  CHECK(entry.categories[0] == "News");
  CHECK(entry.categories[1] == "Talk");
  CHECK(entry.iconPath == "https://example.invalid/icon.png");
  CHECK(entry.director == "Alice Director");
  // <writer> and <adapter> both fold into "writer", comma-joined in document order.
  CHECK(entry.writer == "Bob Writer,Carol Adapter");
  // actor/presenter/guest/producer/commentator/composer/editor all fold into "cast".
  CHECK(entry.cast == "Dave Actor,Eve Presenter");
  CHECK(entry.year == 2025);
  CHECK(entry.firstAired == "2025-12-25");
  CHECK(entry.isNew);
  CHECK(entry.isPremiere);
  CHECK(entry.isLive);
  // xmltv_ns "2.4.0/2": 0-indexed season/episode, "/2" part-of-multipart suffix
  // stripped, so season 2->3 and episode 4->5 once converted to 1-indexed.
  CHECK(entry.seasonNumber == 3);
  CHECK(entry.episodeNumber == 5);
  CHECK(entry.startTime == 1767268800); // 2026-01-01 12:00:00 UTC
  CHECK(entry.endTime == 1767272400);   // 2026-01-01 13:00:00 UTC
}

TEST_CASE("Applies a non-UTC timezone offset to start/stop times", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 -0500" stop="20260101130000 -0500">
      <title>Channel B Show</title>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  REQUIRE(out.at("1").size() == 1);
  // "12:00:00 -0500" is 17:00:00 UTC.
  CHECK(out.at("1")[0].startTime == 1767286800);
}

TEST_CASE("Skips a programme with no channel attribute", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>No Channel</title>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  CHECK(out.empty());
}

TEST_CASE("Skips a programme with an unparseable start/stop time", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="not-a-time" stop="20260101130000 +0000">
      <title>Bad Start</title>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  CHECK(out.empty());
}

TEST_CASE("Fails with an error when the document has no <tv> root", "[XmlTvParser]")
{
  const char* doc = R"(<notatv></notatv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  CHECK_FALSE(XmlTvParser::Parse(doc, out, error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("Fails with an error on malformed XML", "[XmlTvParser]")
{
  const char* doc = "<tv><programme channel=\"1\">";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  CHECK_FALSE(XmlTvParser::Parse(doc, out, error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("Groups programmes from multiple channels separately", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Channel A Show</title>
    </programme>
    <programme channel="2" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Channel B Show</title>
    </programme>
    <programme channel="1" start="20260101130000 +0000" stop="20260101140000 +0000">
      <title>Channel A Second Show</title>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  REQUIRE(out.size() == 2);
  REQUIRE(out.at("1").size() == 2);
  REQUIRE(out.at("2").size() == 1);
  CHECK(out.at("1")[0].title == "Channel A Show");
  CHECK(out.at("1")[1].title == "Channel A Second Show");
}

TEST_CASE("Ignores an episode-num whose system isn't xmltv_ns", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Onscreen Numbered</title>
      <episode-num system="onscreen">S03E05</episode-num>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  const EpgEntry& entry = out.at("1")[0];
  CHECK(entry.seasonNumber == -1);
  CHECK(entry.episodeNumber == -1);
}

TEST_CASE("Parses partial xmltv_ns episode-num values", "[XmlTvParser]")
{
  SECTION("missing season")
  {
    const char* doc = R"(<tv>
      <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
        <title>T</title>
        <episode-num system="xmltv_ns">.4.</episode-num>
      </programme>
    </tv>)";

    std::unordered_map<std::string, std::vector<EpgEntry>> out;
    std::string error;
    REQUIRE(XmlTvParser::Parse(doc, out, error));
    CHECK(out.at("1")[0].seasonNumber == -1);
    CHECK(out.at("1")[0].episodeNumber == 5);
  }

  SECTION("missing episode")
  {
    const char* doc = R"(<tv>
      <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
        <title>T</title>
        <episode-num system="xmltv_ns">2..</episode-num>
      </programme>
    </tv>)";

    std::unordered_map<std::string, std::vector<EpgEntry>> out;
    std::string error;
    REQUIRE(XmlTvParser::Parse(doc, out, error));
    CHECK(out.at("1")[0].seasonNumber == 3);
    CHECK(out.at("1")[0].episodeNumber == -1);
  }
}
