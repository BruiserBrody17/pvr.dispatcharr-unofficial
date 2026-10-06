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

TEST_CASE("FindChannelGroupIdByKodiName finds a group by its exact name", "[ChannelGroupFilter]")
{
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News"), MakeGroup(2, "Sports")};
  CHECK(FindChannelGroupIdByKodiName(groups, "Sports") == 2);
  CHECK(FindChannelGroupIdByKodiName(groups, "News") == 1);
}

TEST_CASE("FindChannelGroupIdByKodiName returns -1 for an unknown name", "[ChannelGroupFilter]")
{
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News")};
  CHECK(FindChannelGroupIdByKodiName(groups, "Movies") == -1);
  CHECK(FindChannelGroupIdByKodiName({}, "News") == -1);
  // A genuine prefix of a short name is a different name, not truncation.
  CHECK(FindChannelGroupIdByKodiName(groups, "New") == -1);
}

TEST_CASE("FindChannelGroupIdByKodiName matches Kodi's own byte-truncated copy of an over-long name",
          "[ChannelGroupFilter]")
{
  // Regression: PVR_CHANNEL_GROUP's strGroupName is char[1024], filled
  // via a byte-level strncpy(..., 1023) -- a group name longer than that
  // used to come back into GetChannelGroupMembers() truncated, match
  // nothing, and get reported as an empty group.
  std::string longName(1500, 'G');
  std::vector<ChannelGroup> groups = {MakeGroup(1, "News"), MakeGroup(7, longName)};
  CHECK(FindChannelGroupIdByKodiName(groups, longName.substr(0, kKodiGroupNameMaxBytes)) == 7);

  // Same, when the cut lands mid-UTF-8-sequence (2-byte characters).
  std::string multiByte;
  for (int i = 0; i < 600; ++i)
    multiByte += "\xD0\x96";
  groups.push_back(MakeGroup(9, multiByte));
  CHECK(FindChannelGroupIdByKodiName(groups, multiByte.substr(0, kKodiGroupNameMaxBytes)) == 9);
}

TEST_CASE("FindChannelGroupIdByKodiName doesn't treat a shorter prefix of an over-long name as a match",
          "[ChannelGroupFilter]")
{
  std::string longName(1500, 'G');
  std::vector<ChannelGroup> groups = {MakeGroup(7, longName)};
  CHECK(FindChannelGroupIdByKodiName(groups, longName.substr(0, 500)) == -1);
  CHECK(FindChannelGroupIdByKodiName(groups, longName.substr(0, kKodiGroupNameMaxBytes - 1)) == -1);
}

TEST_CASE("FindChannelGroupIdByKodiName matches a name exactly kKodiGroupNameMaxBytes long in full",
          "[ChannelGroupFilter]")
{
  std::string fits(kKodiGroupNameMaxBytes, 'F');
  std::vector<ChannelGroup> groups = {MakeGroup(3, fits)};
  CHECK(FindChannelGroupIdByKodiName(groups, fits) == 3);
}

TEST_CASE("FindChannelGroupIdByKodiName matches a name cut to Kodi's 1023-byte limit, and only then",
          "[ChannelGroupFilter]")
{
  ChannelGroup g;
  g.id = 9;
  g.name = std::string(1023, 'x');
  // Exactly 1023 bytes: matches itself.
  CHECK(FindChannelGroupIdByKodiName({g}, std::string(1023, 'x')) == 9);
  // A 1024-byte server name reaches Kodi cut to 1023: Kodi's copy still finds it.
  g.name = std::string(1024, 'x');
  CHECK(FindChannelGroupIdByKodiName({g}, std::string(1023, 'x')) == 9);
  // But a different 1023-byte name does not, and a short prefix does not either.
  CHECK(FindChannelGroupIdByKodiName({g}, std::string(1022, 'x') + "y") == -1);
  CHECK(FindChannelGroupIdByKodiName({g}, std::string(1022, 'x')) == -1);
}
