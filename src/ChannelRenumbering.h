#pragma once

#include "ChannelNumber.h"
#include "DispatcharrClient.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dispatcharr
{

// The text Dispatcharr's export puts in `<channel id>` for this channel, and so
// the key the guide is looked up by: its number's text, or its id when it has
// no number. Empty when there is nothing usable.
inline std::string ChannelGuideKey(const Channel& ch)
{
  return FormatChannelGuideKey(ch.hasChannelNumber, ch.channelNumber, ch.id);
}

// Whether any channel present in both `oldChannels` and `newChannels`
// (matched by id) has a different `channelNumber` between the two --
// pulled out of PVRDispatcharr::EnsureChannelsLoaded() specifically so
// it's unit-testable standalone; see ../tests/test_channel_renumbering.cpp.
//
// Fix for a real, confirmed bug found via a project-wide review (a
// 23rd-pass audit), confirmed against Dispatcharr's own real current
// upstream source (cloned into a scratchpad, never committed to this
// repo -- stronger than the API shape alone, not the same standard as a
// live test), not itself independently reproduced: Dispatcharr's own
// "compact numbering" scheme (apps/channels/compact_numbering.py, whose
// own docstring says as much) shifts every channel number after a
// hidden/unhidden one to close the gap, so hiding a single channel in a
// compact-numbered group renumbers every later channel in it.
//
// PVRDispatcharr::GetEPGForChannel() keys its own cache by channel
// number (m_epgByChannelNumber), and this addon's own channel and EPG
// refreshes run on completely independent timers
// (channel_refresh_hours, default 12h, vs. epg_refresh_hours, default
// 4h) with nothing telling the EPG side a renumbering just happened.
// Without this check, a channel whose number just shifted could show
// its former neighbor's guide (or none at all) for up to the full
// epg_refresh_hours window even after the channel list itself already
// picked up the new numbers.
//
// Naturally returns false on a first load (an empty `oldChannels`) with
// no special-casing needed -- there's no common id to disagree on yet.
//
// Also catches a channel *id* that's new to this addon reusing a number
// a *different*, since-removed id used to hold (added 2026-09-26, a
// 24th-pass audit, fixing a real, confirmed gap in the fix above found
// via a project-wide review, confirmed against Dispatcharr's own real
// current upstream source, not itself independently reproduced):
// `Channel.get_next_available_channel_number()` (`apps/channels/models.py`)
// hands out the lowest free number, so deleting channel id 1 (number 5)
// and then creating (or auto-syncing) a brand-new channel id 2 can
// reuse that exact number 5 -- an id-only comparison sees no shared id
// between the two snapshots at all and misses it, even though this is
// the identical "stale guide shown against the wrong channel" symptom
// the id-based check above already exists to catch. Deliberately does
// NOT also trigger for a genuinely new number never seen before (a
// brand-new channel with nothing to reuse) -- that's "no guide yet",
// not "wrong guide", the same tolerated delay any newly-added channel
// already has until its own next natural EPG refresh, not a
// correctness bug worth forcing an extra fetch for.
//
// The number-reuse check keeps every id that ever held a given number in
// `oldChannels` (`oldIdsByNumber`, a set rather than a single id) --
// fixing a real, confirmed regression in the fix above found via a
// project-wide review (a 25th-pass audit), not itself independently
// reproduced: Dispatcharr genuinely permits two channels sharing the
// same `channel_number` (see the already-logged duplicate-channel-number
// EPG-collision item), and the initial version of this check stored only
// the *last* id seen for a given number, overwriting any earlier one --
// so for two old channels sharing number 5 (ids 1 and 2, whichever was
// iterated last "wins" the map slot), a *completely unchanged* channel
// among them could still spuriously read as "reused by a different id"
// against the other one's own id, forcing a full EPG re-fetch on every
// single successful channel refresh from then on, deterministically, not
// just around an actual renumbering event. Checking set membership
// instead (`ch.id` is *among* the ids that held this number before, not
// *exactly equal to* one arbitrarily-remembered one) fixes this for any
// number of channels sharing a number, stable or not.
//
// A channel with no usable key at all (empty: a number that isn't finite or is
// absurdly large) is skipped entirely from the number-reuse check (though
// still covered by the id-based check above), since it is never looked up by
// key in the first place. A channel with NO number is not that case: it has
// a key, its own id (see ChannelGuideKey()), so it takes part like any other.

inline bool HaveChannelNumbersChanged(const std::vector<Channel>& oldChannels, const std::vector<Channel>& newChannels)
{
  // Compared as the guide-lookup key text (FormatChannelNumberKey()), not
  // the raw number: that text is exactly what a renumbering has to change
  // for a cached guide to go stale, and it keeps 5 and 5.1 -- distinct
  // channels to Dispatcharr, but the same integer before the subchannel
  // fix -- from ever reading as the same number. An empty key means no
  // usable number.
  std::unordered_map<int, std::string> oldKeyById;
  std::unordered_map<std::string, std::unordered_set<int>> oldIdsByKey;
  oldKeyById.reserve(oldChannels.size());
  for (const auto& ch : oldChannels)
  {
    std::string key = ChannelGuideKey(ch);
    if (!key.empty())
      oldIdsByKey[key].insert(ch.id);
    oldKeyById[ch.id] = std::move(key);
  }

  for (const auto& ch : newChannels)
  {
    std::string key = ChannelGuideKey(ch);
    auto byId = oldKeyById.find(ch.id);
    if (byId != oldKeyById.end() && byId->second != key)
      return true;
    if (key.empty())
      continue;
    auto byKey = oldIdsByKey.find(key);
    if (byKey != oldIdsByKey.end() && byKey->second.find(ch.id) == byKey->second.end())
      return true;
  }
  return false;
}

// Returns the channelNumber values (as the same FormatChannelNumberKey()
// text PVRDispatcharr::GetEPGForChannel()/ResolveRecordingBroadcastId() use
// against m_epgByChannelNumber) shared by two or more channels in the given
// list. Keyed by the exact text, so a subchannel 5.1 and a channel 5 are
// different numbers here, as they are in Dispatcharr's own XMLTV export. Dispatcharr deliberately permits this --
// ChannelSerializer's own help text states outright that "duplicate channel_number values across channels are
// permitted" -- and its own unauthenticated /output/epg export (this addon's only export path, since it never
// authenticates as a real Dispatcharr user for that fetch) has no way to disambiguate a collision: apps/output/epg.py's
// generate_epg() only remaps a duplicate when the export has a real `user` (Dispatcharr's own XC-client-only path), so
// both colliding channels are exported under the identical `<channel id="N">`.
//
// Confirmed live 2026-09-28, not just theoretical (see
// docs/OPEN_ITEMS.md): a real lab instance's own channel list had many
// distinct collisions, and the real /output/epg XMLTV feed showed exactly
// the predicted shape for one of them -- two separate `<channel
// id>` tags for one number (different display-names, no disambiguation)
// with real `<programme>` entries attached to that one shared id.
// Without this check, both PVRDispatcharr::GetEPGForChannel() and
// ResolveRecordingBroadcastId()'s own m_epgByChannelNumber lookups would
// hand the identical (unioned) programme list to every channel sharing
// that number -- including one that has nothing actually scheduled on it.
//
// Skips a channel with no usable key at all (empty), which is never looked up.
// A channel with no number is keyed by its id, so it can only collide with a
// channel whose number happens to be that same text -- which it genuinely does,
// since Dispatcharr writes both into one `<channel id>` space.
inline std::unordered_set<std::string> FindAmbiguousChannelNumbers(const std::vector<Channel>& channels)
{
  std::unordered_map<std::string, int> countByKey;
  for (const auto& ch : channels)
  {
    std::string key = ChannelGuideKey(ch);
    if (!key.empty())
      ++countByKey[key];
  }

  std::unordered_set<std::string> ambiguous;
  for (const auto& [key, count] : countByKey)
  {
    if (count > 1)
      ambiguous.insert(key);
  }
  return ambiguous;
}

} // namespace dispatcharr
