#include "ChannelParser.h"

#include "ChannelNumber.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

TEST_CASE("ParseChannelJson maps the bare fields", "[ChannelParser]")
{
  json item = {{"id", 5}, {"uuid", "abc-123"}, {"name", "Channel A"}, {"channel_number", 101}};

  Channel ch = ParseChannelJson(item);

  CHECK(ch.id == 5);
  CHECK(ch.uuid == "abc-123");
  CHECK(ch.name == "Channel A");
  CHECK(ch.channelNumber == 101);
}

TEST_CASE("ParseChannelJson falls back to channel_num when channel_number is absent", "[ChannelParser]")
{
  json item = {{"channel_num", 7}};

  CHECK(ParseChannelJson(item).channelNumber == 7);
}

TEST_CASE("ParseChannelJson prefers effective_name/effective_channel_number over the raw fields -- the real bug "
          "this fixes",
          "[ChannelParser]")
{
  // Dispatcharr's own per-field ChannelOverride mechanism: a user can
  // override an auto-synced channel's name/number individually. The
  // channel-number case matters beyond display -- Dispatcharr's own
  // XMLTV export keys <channel id> by effective_channel_number in its
  // default tvg_id_source mode, so ignoring an override here breaks EPG
  // matching for that channel entirely, not just its displayed number.
  json item = {{"name", "Raw Name"},
               {"effective_name", "Overridden Name"},
               {"channel_number", 105},
               {"effective_channel_number", 5}};

  Channel ch = ParseChannelJson(item);

  CHECK(ch.name == "Overridden Name");
  CHECK(ch.channelNumber == 5);
}

TEST_CASE("ParseChannelJson falls back to the raw name/channel_number when no override is set", "[ChannelParser]")
{
  json item = {{"name", "Raw Name"}, {"channel_number", 105}};

  Channel ch = ParseChannelJson(item);

  CHECK(ch.name == "Raw Name");
  CHECK(ch.channelNumber == 105);
}

TEST_CASE("ParseChannelJson keeps a fractional channel number instead of truncating it", "[ChannelParser]")
{
  // The live-confirmed bug (docs/OPEN_ITEMS.md, 2026-09-30): channel_number
  // is a float server-side, and get<int>() silently truncates one, so 5.1
  // became channel 5. Parsed from JSON text, the way a real response is.
  Channel ch = ParseChannelJson(json::parse(R"({"id": 1, "channel_number": 88881.1})"));
  CHECK(ch.channelNumber == 88881.1);
  CHECK(FormatChannelNumberKey(ch.channelNumber) == "88881.1");
  CHECK(WholeChannelNumber(ch.channelNumber) == 88881);
  CHECK(SubChannelNumber(ch.channelNumber) == 1);
}

TEST_CASE("ParseChannelJson reads a whole-valued float channel number as that whole number", "[ChannelParser]")
{
  // The real API returns 101.0 for channel 101.
  Channel ch = ParseChannelJson(json::parse(R"({"channel_number": 101.0})"));
  CHECK(ch.channelNumber == 101.0);
  CHECK(FormatChannelNumberKey(ch.channelNumber) == "101");
  CHECK(SubChannelNumber(ch.channelNumber) == 0);
}

TEST_CASE("ParseChannelJson prefers a fractional effective_channel_number over the raw number", "[ChannelParser]")
{
  Channel ch = ParseChannelJson(json::parse(R"({"channel_number": 105, "effective_channel_number": 5.25})"));
  CHECK(ch.channelNumber == 5.25);
  CHECK(FormatChannelNumberKey(ch.channelNumber) == "5.25");
}

TEST_CASE("ParseChannelJson maps a null or missing channel number to 0 (no number)", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json::parse(R"({"channel_number": null})")).channelNumber == 0.0);
  CHECK(ParseChannelJson(json::object()).channelNumber == 0.0);
}

TEST_CASE("ParseChannelJson tells a null channel number from a literal 0", "[ChannelParser]")
{
  // Dispatcharr's XMLTV export keys the two differently: a number by its text
  // ("0"), a channel with no number by its id.
  CHECK_FALSE(ParseChannelJson(json::parse(R"({"id": 7, "channel_number": null})")).hasChannelNumber);
  CHECK_FALSE(ParseChannelJson(json::parse(R"({"id": 7})")).hasChannelNumber);
  CHECK_FALSE(ParseChannelJson(json::parse(R"({"id": 7, "effective_channel_number": null, "channel_number": null})"))
                  .hasChannelNumber);
  Channel zero = ParseChannelJson(json::parse(R"({"id": 7, "channel_number": 0})"));
  CHECK(zero.hasChannelNumber);
  CHECK(zero.channelNumber == 0.0);
  CHECK(ParseChannelJson(json::parse(R"({"id": 7, "channel_number": 12.0})")).hasChannelNumber);
  // An effective number wins, and a null effective one falls back to the raw.
  CHECK(ParseChannelJson(json::parse(R"({"id": 7, "effective_channel_number": null, "channel_number": 3})"))
            .hasChannelNumber);
}

TEST_CASE("ParseChannelJson doesn't count a non-numeric channel number as a number", "[ChannelParser]")
{
  CHECK_FALSE(ParseChannelJson(json::parse(R"({"id": 7, "channel_number": "abc"})")).hasChannelNumber);
}

TEST_CASE("ParseChannelJson falls back to 0 for a channel number that isn't a number at all", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json::parse(R"({"channel_number": "five"})")).channelNumber == 0.0);
}

TEST_CASE("ParseChannelJson defaults logoId to -1 (no logo)", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json::object()).logoId == -1);
  CHECK(ParseChannelJson(json{{"logo_id", nullptr}}).logoId == -1);
  CHECK(ParseChannelJson(json{{"logo_id", 42}}).logoId == 42);
}

TEST_CASE("ParseChannelJson prefers effective_logo_id over the raw logo_id", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json{{"logo_id", 42}, {"effective_logo_id", 7}}).logoId == 7);
  CHECK(ParseChannelJson(json{{"logo_id", 42}}).logoId == 42);
}

TEST_CASE("ParseChannelJson reads channel_group from a nested object", "[ChannelParser]")
{
  json item = {{"channel_group", {{"id", 9}, {"name", "News"}}}};

  Channel ch = ParseChannelJson(item);

  CHECK(ch.groupId == 9);
  CHECK(ch.groupName == "News");
}

TEST_CASE("ParseChannelJson reads channel_group as a bare id when not nested", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json{{"channel_group", 3}}).groupId == 3);
  CHECK(ParseChannelJson(json{{"channel_group_id", 4}}).groupId == 4);
  CHECK(ParseChannelJson(json::object()).groupId == -1);
}

TEST_CASE("ParseChannelJson prefers effective_channel_group_id over the raw bare id", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json{{"channel_group", 3}, {"effective_channel_group_id", 9}}).groupId == 9);
  CHECK(ParseChannelJson(json{{"channel_group_id", 4}, {"effective_channel_group_id", 9}}).groupId == 9);
}

TEST_CASE("ParseChannelJson reads tvgId from a nested epg_data object first", "[ChannelParser]")
{
  json item = {{"epg_data", {{"tvg_id", "nested.id"}}}, {"effective_tvg_id", "effective.id"}, {"tvg_id", "plain.id"}};

  CHECK(ParseChannelJson(item).tvgId == "nested.id");
}

TEST_CASE("ParseChannelJson falls back to effective_tvg_id then tvg_id when epg_data is absent", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json{{"effective_tvg_id", "effective.id"}, {"tvg_id", "plain.id"}}).tvgId == "effective.id");
  CHECK(ParseChannelJson(json{{"tvg_id", "plain.id"}}).tvgId == "plain.id");
}

TEST_CASE("ParseChannelJson reads epgDataId with the effective-then-plain fallback", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json{{"effective_epg_data_id", 11}, {"epg_data_id", 22}}).epgDataId == 11);
  CHECK(ParseChannelJson(json{{"epg_data_id", 22}}).epgDataId == 22);
  CHECK(ParseChannelJson(json::object()).epgDataId == 0);
}

TEST_CASE("ParseChannelJson maps catch-up fields", "[ChannelParser]")
{
  json item = {{"is_catchup", true}, {"catchup_days", 7}};

  Channel ch = ParseChannelJson(item);

  CHECK(ch.catchupEnabled);
  CHECK(ch.catchupDays == 7);
}

TEST_CASE("ParseChannelGroupJson maps id and name", "[ChannelParser]")
{
  json item = {{"id", 3}, {"name", "Sports"}};

  ChannelGroup g = ParseChannelGroupJson(item);

  CHECK(g.id == 3);
  CHECK(g.name == "Sports");
}

TEST_CASE("ParseChannelJson counts a channel carrying only the older channel_num field as numbered", "[ChannelParser]")
{
  json item = {{"id", 5}, {"channel_num", 7}};
  Channel ch = ParseChannelJson(item);
  CHECK(ch.hasChannelNumber);
  CHECK(ch.channelNumber == 7);
  CHECK_FALSE(ParseChannelJson(json{{"id", 5}}).hasChannelNumber);
  CHECK_FALSE(ParseChannelJson(json{{"id", 5}, {"channel_number", nullptr}}).hasChannelNumber);
}
