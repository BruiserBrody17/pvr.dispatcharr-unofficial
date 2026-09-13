#include "ChannelParser.h"

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

TEST_CASE("ParseChannelJson defaults logoId to -1 (no logo)", "[ChannelParser]")
{
  CHECK(ParseChannelJson(json::object()).logoId == -1);
  CHECK(ParseChannelJson(json{{"logo_id", nullptr}}).logoId == -1);
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
