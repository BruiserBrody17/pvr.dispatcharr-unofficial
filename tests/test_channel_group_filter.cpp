#include "ChannelGroupFilter.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
Channel MakeChannel(int groupId)
{
  Channel ch;
  ch.groupId = groupId;
  return ch;
}

ChannelGroup MakeGroup(int id, const std::string& name)
{
  ChannelGroup g;
  g.id = id;
  g.name = name;
  return g;
}
} // namespace

TEST_CASE("FilterChannelGroupsWithChannels keeps a group with at least one member channel", "[ChannelGroupFilter]")
{
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News")};
  std::vector<Channel> channels = {MakeChannel(1)};

  std::vector<ChannelGroup> result = FilterChannelGroupsWithChannels(groups, channels);

  REQUIRE(result.size() == 1);
  CHECK(result[0].id == 1);
}

TEST_CASE("FilterChannelGroupsWithChannels drops a group with no member channels", "[ChannelGroupFilter]")
{
  // The real reason this exists: Dispatcharr's /api/channels/groups/
  // returns every group that has ever existed, including ones no longer
  // enabled for any M3U account -- "enabled" isn't even a property of
  // the group itself, so this is the only reliable signal.
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News"), MakeGroup(2, "Disabled Group")};
  std::vector<Channel> channels = {MakeChannel(1)};

  std::vector<ChannelGroup> result = FilterChannelGroupsWithChannels(groups, channels);

  REQUIRE(result.size() == 1);
  CHECK(result[0].id == 1);
}

TEST_CASE("FilterChannelGroupsWithChannels drops every group when there are no channels at all", "[ChannelGroupFilter]")
{
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News"), MakeGroup(2, "Sports")};

  std::vector<ChannelGroup> result = FilterChannelGroupsWithChannels(groups, {});

  CHECK(result.empty());
}

TEST_CASE("FilterChannelGroupsWithChannels keeps every group when every group has a channel", "[ChannelGroupFilter]")
{
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News"), MakeGroup(2, "Sports")};
  std::vector<Channel> channels = {MakeChannel(1), MakeChannel(2)};

  std::vector<ChannelGroup> result = FilterChannelGroupsWithChannels(groups, channels);

  CHECK(result.size() == 2);
}

TEST_CASE("FilterChannelGroupsWithChannels handles an empty groups list", "[ChannelGroupFilter]")
{
  CHECK(FilterChannelGroupsWithChannels({}, {MakeChannel(1)}).empty());
}

TEST_CASE("FilterChannelGroupsWithChannels counts a group with multiple member channels once", "[ChannelGroupFilter]")
{
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News")};
  std::vector<Channel> channels = {MakeChannel(1), MakeChannel(1), MakeChannel(1)};

  std::vector<ChannelGroup> result = FilterChannelGroupsWithChannels(groups, channels);

  REQUIRE(result.size() == 1);
  CHECK(result[0].id == 1);
}
