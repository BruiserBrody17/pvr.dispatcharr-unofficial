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

TEST_CASE("Parses a raw unhyphenated <date> (the XMLTV DTD's own digit-string form)", "[XmlTvParser]")
{
  // The real bug this guards against: Kodi's own SetFromW3CDate() only
  // reads month/day when the date string is hyphenated ("YYYY-MM-DD"),
  // so a fully spec-compliant "20251225" would otherwise display as
  // January 1st in Kodi despite carrying a real month/day.
  const char* doc = R"(<?xml version="1.0" encoding="UTF-8"?>
<tv>
  <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
    <title>Channel A News</title>
    <date>20251225</date>
  </programme>
</tv>
)";
  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  REQUIRE(out.at("1").size() == 1);
  CHECK(out.at("1")[0].firstAired == "2025-12-25");
  CHECK(out.at("1")[0].year == 2025);
}

TEST_CASE("Applies a non-UTC timezone offset to start/stop times", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 -0700" stop="20260101130000 -0700">
      <title>Channel B Show</title>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  REQUIRE(out.at("1").size() == 1);
  // "12:00:00 -0700" is 19:00:00 UTC.
  CHECK(out.at("1")[0].startTime == 1767294000);
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

TEST_CASE("Ignores an episode-num whose system isn't xmltv_ns or onscreen", "[XmlTvParser]")
{
  // dd_progid is a real XMLTV system value some providers use for a
  // program-identifier string, not a season/episode number at all --
  // this addon has no coverage for it and shouldn't guess.
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Program-Id Numbered</title>
      <episode-num system="dd_progid">EP01234567.0001</episode-num>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  const EpgEntry& entry = out.at("1")[0];
  CHECK(entry.seasonNumber == -1);
  CHECK(entry.episodeNumber == -1);
}

TEST_CASE("Falls back to an onscreen episode-num when no xmltv_ns entry is present", "[XmlTvParser]")
{
  // The real, confirmed-from-source gap this fallback fixes: Dispatcharr's
  // own XMLTV export only emits xmltv_ns when both season and episode are
  // known, an episode-only programme instead exported solely as onscreen.
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Onscreen Numbered</title>
      <episode-num system="onscreen">E12</episode-num>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  const EpgEntry& entry = out.at("1")[0];
  CHECK(entry.seasonNumber == -1);
  CHECK(entry.episodeNumber == 12);
}

TEST_CASE("Prefers xmltv_ns over onscreen when both are present on the same programme", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Both Present</title>
      <episode-num system="onscreen">S99E99</episode-num>
      <episode-num system="xmltv_ns">2.4.</episode-num>
    </programme>
  </tv>)";

  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  const EpgEntry& entry = out.at("1")[0];
  CHECK(entry.seasonNumber == 3);
  CHECK(entry.episodeNumber == 5);
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

// ---------------------------------------------------------------------
// ParseEpisodeNum
// ---------------------------------------------------------------------

TEST_CASE("ParseEpisodeNum parses a plain season.episode value", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseEpisodeNum("2.4", season, episode);
  CHECK(season == 3);
  CHECK(episode == 5);
}

TEST_CASE("ParseEpisodeNum strips a trailing part-of-multipart suffix", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseEpisodeNum("2.4.0/2", season, episode);
  CHECK(season == 3);
  CHECK(episode == 5);
}

TEST_CASE("ParseEpisodeNum keeps the episode number when the season field carries its own /total -- "
          "the real bug this fixes",
          "[XmlTvParser]")
{
  // Per the XMLTV DTD, each dot-separated field can independently carry
  // its own "/total" -- not just the trailing part-number field. The
  // previous implementation dropped everything after the very first '/'
  // in the whole value, truncating "1/3.4/10." down to just "1" and
  // losing the episode entirely.
  int season = 0, episode = 0;
  ParseEpisodeNum("1/3.4/10.", season, episode);
  CHECK(season == 2);
  CHECK(episode == 5);
}

TEST_CASE("ParseEpisodeNum strips /total from both the season and episode fields at once", "[XmlTvParser]")
{
  // Season 0 of 3 total, episode 0 of 10 total (both 0-indexed).
  int season = 0, episode = 0;
  ParseEpisodeNum("0/3.0/10.", season, episode);
  CHECK(season == 1);
  CHECK(episode == 1);
}

TEST_CASE("ParseEpisodeNum leaves both unknown for an empty value", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseEpisodeNum("", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

TEST_CASE("ParseEpisodeNum correctly produces season 0 for a real \"specials\" episode", "[XmlTvParser]")
{
  // Dispatcharr's own XMLTV export (apps/output/epg.py) emits xmltv_ns
  // as "-1.<ep>." for a stored season of 0 -- its own 0-indexed
  // convention (season - 1) underflowing for a genuine specials
  // episode, not a sentinel for "unknown" the way an empty value is.
  // Confirmed against Dispatcharr's own real current upstream source (a
  // 25th-pass audit). season == 0 here is a real, legitimate value --
  // distinct from -1 (the actual "unknown" case, see the test above) --
  // and PVRDispatcharr::GetEPGForChannel()'s own `>= 0` check (not `>
  // 0`) is what actually propagates it to Kodi.
  int season = 0, episode = 0;
  ParseEpisodeNum("-1.4.", season, episode);
  CHECK(season == 0);
  CHECK(episode == 5);
}

TEST_CASE("ParseEpisodeNum leaves a field unknown rather than signed-overflow on parsed+1 -- a real UB fix",
          "[XmlTvParser]")
{
  // std::stoi clamps an overlong digit string to INT_MAX rather than
  // throwing (a provider EPG field is untrusted input, so this is
  // reachable) -- parsed + 1 at INT_MAX is signed-integer overflow,
  // undefined behavior in C++. Found via a project-wide UB review, not
  // reproduced live. No real season/episode number is ever legitimately
  // this large, so this is treated the same as any other unparseable
  // value.
  int season = 0, episode = 0;
  ParseEpisodeNum("2147483647.2147483647", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

// ---------------------------------------------------------------------
// ParseOnscreenEpisodeNum
// ---------------------------------------------------------------------

TEST_CASE("ParseOnscreenEpisodeNum parses a plain SxEy value", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("S3E10", season, episode);
  CHECK(season == 3);
  CHECK(episode == 10);
}

TEST_CASE("ParseOnscreenEpisodeNum is case-insensitive", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("s3e10", season, episode);
  CHECK(season == 3);
  CHECK(episode == 10);
}

TEST_CASE("ParseOnscreenEpisodeNum parses a bare episode-only value, leaving season unknown", "[XmlTvParser]")
{
  // The real, confirmed-from-source case this fallback exists for --
  // Dispatcharr's own XMLTV export emits exactly this shape for an
  // episode-only programme with no season.
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("E12", season, episode);
  CHECK(season == -1);
  CHECK(episode == 12);
}

TEST_CASE("ParseOnscreenEpisodeNum does not apply xmltv_ns's +1 adjustment", "[XmlTvParser]")
{
  // Onscreen numbers are already exactly what would appear on screen --
  // unlike xmltv_ns's 0-indexed convention, "E1" means episode 1, not 2.
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("E1", season, episode);
  CHECK(episode == 1);
}

TEST_CASE("ParseOnscreenEpisodeNum ignores leading/trailing whitespace", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("  S3E10  ", season, episode);
  CHECK(season == 3);
  CHECK(episode == 10);
}

TEST_CASE("ParseOnscreenEpisodeNum leaves both unknown for an empty value", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

TEST_CASE("ParseOnscreenEpisodeNum leaves both unknown for a value with no episode marker at all", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("Special", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

TEST_CASE("ParseOnscreenEpisodeNum leaves both unknown for \"S\" with no digits after it", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("SE10", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

TEST_CASE("ParseOnscreenEpisodeNum leaves both unknown for \"E\" with no digits after it", "[XmlTvParser]")
{
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("S3E", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

TEST_CASE("ParseOnscreenEpisodeNum leaves both unknown for trailing garbage after the episode digits", "[XmlTvParser]")
{
  // "S3E10x" -- don't guess at a form this fallback doesn't fully
  // recognize, the same "don't guess" convention ParseEpisodeNum() uses.
  int season = 0, episode = 0;
  ParseOnscreenEpisodeNum("S3E10x", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

// ---------------------------------------------------------------------
// NormalizeXmlTvDateToW3C
// ---------------------------------------------------------------------

TEST_CASE("NormalizeXmlTvDateToW3C inserts hyphens into a raw YYYYMMDD value", "[XmlTvParser]")
{
  CHECK(NormalizeXmlTvDateToW3C("20251225") == "2025-12-25");
}

TEST_CASE("NormalizeXmlTvDateToW3C inserts a hyphen into a raw YYYYMM value", "[XmlTvParser]")
{
  CHECK(NormalizeXmlTvDateToW3C("202512") == "2025-12");
}

TEST_CASE("NormalizeXmlTvDateToW3C leaves a bare year-only value unchanged", "[XmlTvParser]")
{
  // Already round-trips correctly through Kodi's own SetFromW3CDate() as
  // year-only (month/day intentionally default to January 1st).
  CHECK(NormalizeXmlTvDateToW3C("2025") == "2025");
}

TEST_CASE("NormalizeXmlTvDateToW3C leaves an already-hyphenated value unchanged", "[XmlTvParser]")
{
  CHECK(NormalizeXmlTvDateToW3C("2025-12-25") == "2025-12-25");
  CHECK(NormalizeXmlTvDateToW3C("2025-12") == "2025-12");
}

TEST_CASE("NormalizeXmlTvDateToW3C leaves a malformed value unchanged rather than guessing", "[XmlTvParser]")
{
  CHECK(NormalizeXmlTvDateToW3C("") == "");
  CHECK(NormalizeXmlTvDateToW3C("abc") == "abc");
  CHECK(NormalizeXmlTvDateToW3C("2025122") == "2025122");   // 7 digits: no recognized form
  CHECK(NormalizeXmlTvDateToW3C("2025122X") == "2025122X"); // 8 chars but not all-digit
}

// ---------------------------------------------------------------------
// ParseXmlTvTime
// ---------------------------------------------------------------------

TEST_CASE("ParseXmlTvTime reads a UTC timestamp", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("20260101120000 +0000") == 1767268800);
  CHECK(ParseXmlTvTime("20260101120000") == 1767268800); // no offset at all: read as UTC
  CHECK(ParseXmlTvTime("19700101000000 +0000") == 0);    // the epoch itself is 0, same as a failure
}

TEST_CASE("ParseXmlTvTime applies a positive or negative offset, including a half-hour one", "[XmlTvParser]")
{
  // 12:00 at +0530 is 06:30 UTC; 12:00 at -0400 is 16:00 UTC.
  CHECK(ParseXmlTvTime("20260101120000 +0530") == 1767268800 - 5 * 3600 - 30 * 60);
  CHECK(ParseXmlTvTime("20260101120000 -0400") == 1767268800 + 4 * 3600);
  CHECK(ParseXmlTvTime("20260101120000 -0000") == 1767268800);
}

TEST_CASE("ParseXmlTvTime ignores a malformed offset and keeps the UTC reading", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("20260101120000 +05") == 1767268800);    // too short to be an offset
  CHECK(ParseXmlTvTime("20260101120000 0530") == 1767268800);   // no sign
  CHECK(ParseXmlTvTime("20260101120000 +ab:cd") == 1767268800); // not digits
}

TEST_CASE("ParseXmlTvTime rejects a field outside its real range instead of normalizing it", "[XmlTvParser]")
{
  // PortableTimeGm() normalizes like timegm(): "month 13, day 45, 25:61:61" came back as a plausible time.
  CHECK(ParseXmlTvTime("20261345256161 +0000") == 0);
  CHECK(ParseXmlTvTime("20261301120000 +0000") == 0); // month 13
  CHECK(ParseXmlTvTime("20260100120000 +0000") == 0); // day 0
  CHECK(ParseXmlTvTime("20260132120000 +0000") == 0); // day 32
  CHECK(ParseXmlTvTime("20260101240000 +0000") == 0); // hour 24
  CHECK(ParseXmlTvTime("20260101250000 +0000") == 0); // hour 25
  CHECK(ParseXmlTvTime("20260101126000 +0000") == 0); // minute 60
  CHECK(ParseXmlTvTime("20260101120061 +0000") == 0); // second 61
  CHECK(ParseXmlTvTime("20260101235959 +0000") != 0);
  CHECK(ParseXmlTvTime("20261231235960 +0000") != 0); // a leap second is tolerated
}

TEST_CASE("ParseXmlTvTime ignores an offset beyond +-23:59 and applies the largest real ones", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("20260101120000 +9999") == 1767268800);
  CHECK(ParseXmlTvTime("20260101120000 +2400") == 1767268800);
  CHECK(ParseXmlTvTime("20260101120000 +0060") == 1767268800);
  CHECK(ParseXmlTvTime("20260101120000 +1400") == 1767268800 - 14 * 3600);
  CHECK(ParseXmlTvTime("20260101120000 -1200") == 1767268800 + 12 * 3600);
}

TEST_CASE("ParseXmlTvTime returns 0 for a value too short or not numeric", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("") == 0);
  CHECK(ParseXmlTvTime("2026010112") == 0);
  CHECK(ParseXmlTvTime("2026-01-01T12:00") == 0);
  CHECK(ParseXmlTvTime("2026010112xx00 +0000") == 0);
}

// ---------------------------------------------------------------------
// Edge cases from the 2026-10-04 hardening sweep's coverage review
// ---------------------------------------------------------------------

namespace
{
std::vector<EpgEntry> ParseOne(const std::string& programmeXml, bool* ok = nullptr)
{
  const std::string doc = "<tv>" + programmeXml + "</tv>";
  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  const bool parsed = XmlTvParser::Parse(doc, out, error);
  if (ok)
    *ok = parsed;
  auto it = out.find("1");
  return it == out.end() ? std::vector<EpgEntry>{} : it->second;
}
} // namespace

TEST_CASE("A programme with no stop time is dropped", "[XmlTvParser]")
{
  bool ok = false;
  auto entries =
      ParseOne(R"(<programme channel="1" start="20260101120000 +0000"><title>No End</title></programme>)", &ok);
  CHECK(ok);
  CHECK(entries.empty());
}

TEST_CASE("A programme that ends at or before its start is dropped", "[XmlTvParser]")
{
  auto entries = ParseOne(
      R"(<programme channel="1" start="20260101130000 +0000" stop="20260101120000 +0000"><title>Backwards</title></programme>)");
  CHECK(entries.empty());
  entries = ParseOne(
      R"(<programme channel="1" start="20260101120000 +0000" stop="20260101120000 +0000"><title>Zero length</title></programme>)");
  CHECK(entries.empty());
  entries = ParseOne(
      R"(<programme channel="1" start="20260101120000 +0000" stop="20260101120001 +0000"><title>One second</title></programme>)");
  CHECK(entries.size() == 1);
}

TEST_CASE("Empty category and actor elements add nothing", "[XmlTvParser]")
{
  auto entries = ParseOne(R"(<programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Show</title><category/><category></category><category>News</category>
      <credits><actor/><actor>Dave Actor</actor><actor></actor></credits>
    </programme>)");
  REQUIRE(entries.size() == 1);
  REQUIRE(entries[0].categories.size() == 1);
  CHECK(entries[0].categories[0] == "News");
  // No stray separator from the empty elements around the one real actor.
  CHECK(entries[0].cast == "Dave Actor");
}

TEST_CASE("A truncated trailing offset on a time is ignored rather than misread", "[XmlTvParser]")
{
  // "+05" is not a complete +HHMM: the time is taken as UTC, not shifted by half an offset.
  const time_t plain = ParseXmlTvTime("20261004120000");
  CHECK(ParseXmlTvTime("20261004120000 +05") == plain);
  CHECK(ParseXmlTvTime("20261004120000 +0500") == plain - 5 * 3600);
}

TEST_CASE("An onscreen episode number too large for an int is unknown, not wrapped", "[XmlTvParser]")
{
  int season = 7, episode = 7;
  ParseOnscreenEpisodeNum("E99999999999", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
  season = episode = 7;
  ParseOnscreenEpisodeNum("S99999999999E1", season, episode);
  CHECK(season == -1);
  CHECK(episode == -1);
}

TEST_CASE("The first xmltv_ns entry on a programme wins over a later one", "[XmlTvParser]")
{
  const char* doc = R"(<tv>
    <programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Two Entries</title>
      <episode-num system="xmltv_ns">2.4.</episode-num>
      <episode-num system="xmltv_ns">7.8.</episode-num>
    </programme>
  </tv>)";
  std::unordered_map<std::string, std::vector<EpgEntry>> out;
  std::string error;
  REQUIRE(XmlTvParser::Parse(doc, out, error));
  CHECK(out.at("1")[0].seasonNumber == 3);
  CHECK(out.at("1")[0].episodeNumber == 5);
}

TEST_CASE("ParseXmlTvTime rejects an impossible calendar date instead of rolling it over", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("20190231000000 +0000") == 0);
  CHECK(ParseXmlTvTime("20260631000000 +0000") == 0);
  CHECK(ParseXmlTvTime("20261131000000 +0000") == 0);
  CHECK(ParseXmlTvTime("20260229000000 +0000") == 0);
  CHECK(ParseXmlTvTime("20280229000000 +0000") != 0); // a real leap day
  CHECK(ParseXmlTvTime("21000229000000 +0000") == 0);
}

TEST_CASE("ParseXmlTvTime reads an offset attached directly to the digits, as XMLTV allows", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("20260101120000+0530") == ParseXmlTvTime("20260101120000 +0530"));
  CHECK(ParseXmlTvTime("20260101120000+0530") == 1767268800 - 5 * 3600 - 30 * 60);
  CHECK(ParseXmlTvTime("20260101120000-0400") == 1767268800 + 4 * 3600);
  CHECK(ParseXmlTvTime("20260101120000   +0530") == ParseXmlTvTime("20260101120000 +0530")); // several spaces
  CHECK(ParseXmlTvTime("20260101120000") == 1767268800);                                     // no offset at all
  CHECK(ParseXmlTvTime("20260101120000 ") == 1767268800);                                    // trailing space only
}

TEST_CASE("Every cast-bucket credit role reaches the cast field, in document order per role", "[XmlTvParser]")
{
  auto entries = ParseOne(R"(<programme channel="1" start="20260101120000 +0000" stop="20260101130000 +0000">
      <title>Show</title>
      <credits><guest>Gus Guest</guest><producer>Pat Producer</producer><commentator>Cam Commentator</commentator>
      <composer>Cleo Composer</composer><editor>Ed Editor</editor><actor>Ann Actor</actor></credits>
    </programme>)");
  REQUIRE(entries.size() == 1);
  const std::string& cast = entries[0].cast;
  for (const char* name : {"Gus Guest", "Pat Producer", "Cam Commentator", "Cleo Composer", "Ed Editor", "Ann Actor"})
    CHECK(cast.find(name) != std::string::npos);
}

TEST_CASE("ParseXmlTvTime rejects a negative hour and ignores a malformed offset", "[XmlTvParser]")
{
  CHECK(ParseXmlTvTime("20261004-10000 +0000") == 0);
  // Five characters that do not start with a sign are not an offset: the time is read as UTC.
  const time_t plain = ParseXmlTvTime("20261004120000");
  CHECK(ParseXmlTvTime("20261004120000 12345") == plain);
  CHECK(ParseXmlTvTime("20261004120000 +0100") == plain - 3600);
}
