#pragma once

#include "DispatcharrClient.h"
#include "Staleness.h"

#include <chrono>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace dispatcharr
{

// Whether the just-fetched channel/group lineup differs from the one
// already cached in any field this addon hands to Kodi (or that a stream
// URL/EPG lookup is built from) -- order-insensitive, matched by id.
// Flagged from a 26th-pass audit (docs/OPEN_ITEMS.md): EnsureChannelsLoaded()
// used to fire TriggerChannelUpdate()/TriggerChannelGroupsUpdate() on
// every successful commit whether or not anything changed, so every
// otherwise-uneventful periodic refresh (and up to two extra at startup)
// cost Kodi a full channel/group resync for nothing. Deliberately
// conservative: any field difference at all counts, including ones Kodi
// may not even display -- a spurious "changed" only costs one redundant
// resync (the status quo), whereas a missed one would leave Kodi stale.
// Naturally true on a first load (empty old vs. non-empty new).
inline bool HasChannelLineupChanged(const std::vector<Channel>& oldChannels, const std::vector<ChannelGroup>& oldGroups,
                                    const std::vector<Channel>& newChannels, const std::vector<ChannelGroup>& newGroups)
{
  if (oldChannels.size() != newChannels.size() || oldGroups.size() != newGroups.size())
    return true;

  std::map<int, const Channel*> oldById;
  for (const Channel& c : oldChannels)
    oldById[c.id] = &c;
  if (oldById.size() != oldChannels.size())
    return true; // duplicate ids in the old snapshot: can't compare reliably, assume changed
  for (const Channel& n : newChannels)
  {
    auto it = oldById.find(n.id);
    if (it == oldById.end())
      return true;
    const Channel& o = *it->second;
    if (o.uuid != n.uuid || o.name != n.name || o.logoId != n.logoId || o.channelNumber != n.channelNumber ||
        o.groupId != n.groupId || o.groupName != n.groupName || o.tvgId != n.tvgId || o.epgDataId != n.epgDataId ||
        o.catchupEnabled != n.catchupEnabled || o.catchupDays != n.catchupDays)
      return true;
  }

  std::set<std::pair<int, std::string>> oldGroupSet;
  for (const ChannelGroup& g : oldGroups)
    oldGroupSet.emplace(g.id, g.name);
  for (const ChannelGroup& g : newGroups)
    if (oldGroupSet.count({g.id, g.name}) == 0)
      return true;
  return false;
}

// The full "should EnsureChannelsLoaded() poke Kodi" decision: a real
// lineup change always does; an unchanged one only does if it's been
// longer than `maxSilence` since the last poke (a zero/never-fired
// `lastTriggerAt` counts as due, IsStaleSince()'s own sentinel). The
// periodic fallback keeps the old every-refresh trigger's incidental
// self-healing for a poke Kodi's own manager missed (both triggers are
// fire-and-forget, so a lost one would otherwise stay lost until the next
// real change) at a fraction of the cost.
inline bool ShouldTriggerKodiChannelSync(bool lineupChanged, std::chrono::steady_clock::time_point lastTriggerAt,
                                         std::chrono::steady_clock::time_point now,
                                         std::chrono::steady_clock::duration maxSilence)
{
  return lineupChanged || IsStaleSince(lastTriggerAt, now, maxSilence);
}

} // namespace dispatcharr
