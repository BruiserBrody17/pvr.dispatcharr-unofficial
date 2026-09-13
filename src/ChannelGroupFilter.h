#pragma once

#include "DispatcharrClient.h"

#include <algorithm>
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

} // namespace dispatcharr
