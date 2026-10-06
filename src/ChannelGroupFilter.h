#pragma once

#include "DispatcharrClient.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace dispatcharr
{

// Pure filtering core of PVRDispatcharr::EnsureChannelsLoaded() --
// Dispatcharr's /api/channels/groups/ returns every group that has ever
// existed, regardless of whether it's currently enabled for any M3U
// account: "enabled" isn't even a property of the group itself, it's
// per (group, account) pair, so there's no simple flag to check here.
// Disabled groups' channels are already correctly excluded from the
// channel list Dispatcharr just gave us, so this drops any group with
// no member channels left in it rather than trying to reconstruct
// Dispatcharr's own enable/disable logic.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see
// ../tests/test_channel_group_filter.cpp.
inline std::vector<ChannelGroup> FilterChannelGroupsWithChannels(std::vector<ChannelGroup> groups,
                                                                 const std::vector<Channel>& channels)
{
  std::unordered_set<int> groupIdsWithChannels;
  for (const auto& ch : channels)
    groupIdsWithChannels.insert(ch.groupId);
  groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const ChannelGroup& g)
                              { return groupIdsWithChannels.find(g.id) == groupIdsWithChannels.end(); }),
               groups.end());
  return groups;
}

// Which group GetChannelGroupMembers() is being asked about -- the pure
// lookup core of PVRDispatcharr::GetChannelGroupMembers(), which Kodi
// identifies only by name. Returns -1 when no group matches.
//
// Compares against each group's name cut to kKodiGroupNameMaxBytes, not
// the full name (added 2026-09-27, a 67th-pass audit, fixing a real,
// confirmed gap of the same class as ShouldRenameOnTimerEdit()'s own
// kKodiTimerTitleMaxBytes guard, TimerIdentity.h, found via a
// project-wide review, confirmed against Kodi's own real current SDK
// source and Dispatcharr's own real current upstream source, not itself
// independently reproduced): PVR_CHANNEL_GROUP's `strGroupName` is a fixed
// `char[PVR_ADDON_NAME_STRING_LENGTH]` (1024,
// kodi/c-api/addon-instance/pvr/pvr_defines.h), filled via a byte-level
// `strncpy(..., sizeof - 1)` both by this addon's own GetChannelGroups()
// (`kodi::addon::PVRChannelGroup::SetGroupName()`) and by Kodi's own
// `CPVRChannelGroup::FillAddonData()` (PVRChannelGroup.cpp) on the way
// back into GetChannelGroupMembers(). Dispatcharr's own `ChannelGroup.name`
// is an unbounded TextField (apps/channels/models.py), so a group name
// over 1023 bytes always came back as its own truncated prefix, matched
// nothing, and was reported as a group with no members at all -- which
// Kodi treats as authoritative and empties that group. If two groups'
// names share the same first 1023 bytes, the first wins -- Kodi itself
// can't tell them apart either, since it keys a client's groups by name.
constexpr std::size_t kKodiGroupNameMaxBytes = 1023;

inline int FindChannelGroupIdByKodiName(const std::vector<ChannelGroup>& groups, const std::string& kodiGroupName)
{
  for (const auto& g : groups)
  {
    if (g.name.compare(0, kKodiGroupNameMaxBytes, kodiGroupName) == 0)
      return g.id;
  }
  return -1;
}

} // namespace dispatcharr
