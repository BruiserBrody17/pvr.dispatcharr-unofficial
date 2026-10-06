#include "ChannelLineupChange.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;
using Clock = std::chrono::steady_clock;

namespace
{
Channel MakeChannel(int id, double number, const std::string& name = "N")
{
  Channel ch;
  ch.id = id;
  ch.channelNumber = number;
  ch.name = name;
  ch.uuid = "u" + std::to_string(id);
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

TEST_CASE("HasChannelLineupChanged is false for identical snapshots", "[ChannelLineupChange]")
{
  std::vector<Channel> c = {MakeChannel(1, 1), MakeChannel(2, 2)};
  std::vector<ChannelGroup> g = {MakeGroup(1, "A")};
  CHECK_FALSE(HasChannelLineupChanged(c, g, c, g));
}

TEST_CASE("HasChannelLineupChanged is false for the same lineup in a different order", "[ChannelLineupChange]")
{
  std::vector<Channel> a = {MakeChannel(1, 1), MakeChannel(2, 2)};
  std::vector<Channel> b = {MakeChannel(2, 2), MakeChannel(1, 1)};
  std::vector<ChannelGroup> ga = {MakeGroup(1, "A"), MakeGroup(2, "B")};
  std::vector<ChannelGroup> gb = {MakeGroup(2, "B"), MakeGroup(1, "A")};
  CHECK_FALSE(HasChannelLineupChanged(a, ga, b, gb));
}

TEST_CASE("HasChannelLineupChanged is true on a first load", "[ChannelLineupChange]")
{
  std::vector<Channel> c = {MakeChannel(1, 1)};
  CHECK(HasChannelLineupChanged({}, {}, c, {}));
}

TEST_CASE("HasChannelLineupChanged is false when both are empty", "[ChannelLineupChange]")
{
  CHECK_FALSE(HasChannelLineupChanged({}, {}, {}, {}));
}

TEST_CASE("HasChannelLineupChanged detects an added, removed, or swapped channel", "[ChannelLineupChange]")
{
  std::vector<Channel> base = {MakeChannel(1, 1), MakeChannel(2, 2)};
  CHECK(HasChannelLineupChanged(base, {}, {MakeChannel(1, 1)}, {}));
  CHECK(HasChannelLineupChanged(base, {}, {MakeChannel(1, 1), MakeChannel(2, 2), MakeChannel(3, 3)}, {}));
  // Same size, different id set -- a size check alone would miss this.
  CHECK(HasChannelLineupChanged(base, {}, {MakeChannel(1, 1), MakeChannel(3, 2)}, {}));
}

TEST_CASE("HasChannelLineupChanged detects a change to any single channel field", "[ChannelLineupChange]")
{
  std::vector<Channel> base = {MakeChannel(1, 1)};
  auto mutated = [&](auto&& mutate)
  {
    std::vector<Channel> c = base;
    mutate(c[0]);
    return HasChannelLineupChanged(base, {}, c, {});
  };
  CHECK(mutated([](Channel& c) { c.name = "Renamed"; }));
  CHECK(mutated([](Channel& c) { c.channelNumber = 9; }));
  // A sub-channel change the old int representation truncated away.
  CHECK(mutated([](Channel& c) { c.channelNumber = 1.5; }));
  CHECK(mutated([](Channel& c) { c.uuid = "other"; }));
  CHECK(mutated([](Channel& c) { c.logoId = 4; }));
  CHECK(mutated([](Channel& c) { c.groupId = 3; }));
  CHECK(mutated([](Channel& c) { c.groupName = "G"; }));
  CHECK(mutated([](Channel& c) { c.tvgId = "tvg"; }));
  CHECK(mutated([](Channel& c) { c.epgDataId = 7; }));
  CHECK(mutated([](Channel& c) { c.catchupEnabled = true; }));
  CHECK(mutated([](Channel& c) { c.catchupDays = 3; }));
}

TEST_CASE("HasChannelLineupChanged detects group additions, removals, and renames", "[ChannelLineupChange]")
{
  std::vector<Channel> c = {MakeChannel(1, 1)};
  std::vector<ChannelGroup> g = {MakeGroup(1, "A")};
  CHECK(HasChannelLineupChanged(c, g, c, {}));
  CHECK(HasChannelLineupChanged(c, g, c, {MakeGroup(1, "A"), MakeGroup(2, "B")}));
  CHECK(HasChannelLineupChanged(c, g, c, {MakeGroup(1, "Renamed")}));
  CHECK(HasChannelLineupChanged(c, g, c, {MakeGroup(2, "A")}));
}

TEST_CASE("HasChannelLineupChanged treats duplicate ids in the old snapshot as changed", "[ChannelLineupChange]")
{
  std::vector<Channel> dup = {MakeChannel(1, 1), MakeChannel(1, 2)};
  CHECK(HasChannelLineupChanged(dup, {}, dup, {}));
}

TEST_CASE("ShouldTriggerKodiChannelSync always fires on a real change", "[ChannelLineupChange]")
{
  auto now = Clock::now();
  CHECK(ShouldTriggerKodiChannelSync(true, now, now, std::chrono::hours(24)));
}

TEST_CASE("ShouldTriggerKodiChannelSync skips an unchanged lineup inside the silence window", "[ChannelLineupChange]")
{
  auto now = Clock::now();
  CHECK_FALSE(ShouldTriggerKodiChannelSync(false, now - std::chrono::hours(12), now, std::chrono::hours(24)));
}

TEST_CASE("ShouldTriggerKodiChannelSync fires an unchanged lineup once past the silence window",
          "[ChannelLineupChange]")
{
  auto now = Clock::now();
  CHECK(ShouldTriggerKodiChannelSync(false, now - std::chrono::hours(25), now, std::chrono::hours(24)));
}

TEST_CASE("ShouldTriggerKodiChannelSync fires when it has never fired before", "[ChannelLineupChange]")
{
  CHECK(ShouldTriggerKodiChannelSync(false, Clock::time_point{}, Clock::now(), std::chrono::hours(24)));
}
