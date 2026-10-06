#include "ChannelParser.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

Channel ParseChannelJson(const nlohmann::json& item)
{
  Channel ch;
  ch.id = FieldOr(item, "id", 0);
  ch.uuid = FieldOr<std::string>(item, "uuid", "");
  // Prefers effective_name/effective_channel_number/effective_logo_id --
  // Dispatcharr's own per-field ChannelOverride mechanism (a user can
  // override an auto-synced channel's name/number/logo/group
  // individually without touching the provider-synced raw fields, which
  // a later M3U resync would just overwrite again) -- over the raw
  // name/channel_number/logo_id. Fix for a real, confirmed asymmetry
  // found via a project-wide review, confirmed against Dispatcharr's own
  // real current upstream source (an 18th-pass audit cloned it into a
  // scratchpad, never committed to this repo): tvgId/epgDataId below
  // already preferred their own effective_* fields, but these didn't.
  // The channel-number gap specifically breaks EPG matching, not just
  // display: apps/output/epg.py's own XMLTV export keys `<channel id>`
  // by effective_channel_number (in its default, non-tvg_id/gracenote
  // tvg_id_source mode -- the mode this addon's own m_epgByChannelNumber
  // lookup, GetEPGForChannel(), already assumes) -- so a channel
  // renumbered via an override (e.g. provider number 105, user override
  // 5) would export XMLTV under id="5", but this addon looked up "105"
  // (the raw, un-overridden number) and got no EPG match, or matched a
  // different real channel 105 if one existed.
  ch.name = FieldOr<std::string>(item, "effective_name", FieldOr<std::string>(item, "name", ""));
  // Read as a double, not an int: nlohmann::json's get<int>() silently
  // truncates a floating-point value instead of throwing, so a subchannel
  // numbered 5.1 used to become channel 5 -- colliding with a real channel
  // 5 and losing its own guide (live-confirmed 2026-09-30, docs/OPEN_ITEMS.md).
  ch.channelNumber =
      FieldOr(item, "effective_channel_number", FieldOr(item, "channel_number", FieldOr(item, "channel_num", 0.0)));
  // A null (or absent) number is not the same as a literal 0: Dispatcharr's
  // XMLTV export keys a channel with no number by its id instead, and only a
  // number by the number's own text -- see FormatChannelGuideKey().
  ch.hasChannelNumber = false;
  for (const char* field : {"effective_channel_number", "channel_number", "channel_num"})
  {
    auto it = item.find(field);
    if (it != item.end() && it->is_number())
    {
      ch.hasChannelNumber = true;
      break;
    }
  }
  // Channels only carry a logo_id (an FK to a separate Logo object);
  // there's no logo_url field directly on the channel (that belongs to
  // the underlying Stream model). Resolve the actual image via
  // GetChannelLogoUrl(logoId), not a direct URL field. logo_id is
  // explicitly null (not absent) for channels with no logo.
  ch.logoId = FieldOr(item, "effective_logo_id", FieldOr(item, "logo_id", -1));
  // Channel group may be a nested object or a bare id depending on the
  // serializer; handle both. The override mechanism only affects the
  // plain group id (effective_channel_group_id), not a nested object's
  // own shape, so that preference only applies on the bare-id path --
  // the nested-object branch already reflects whatever group Dispatcharr
  // itself embedded there directly.
  if (item.contains("channel_group") && item["channel_group"].is_object())
  {
    ch.groupId = FieldOr(item["channel_group"], "id", -1);
    ch.groupName = FieldOr<std::string>(item["channel_group"], "name", "");
  }
  else
  {
    ch.groupId = FieldOr(item, "effective_channel_group_id",
                         FieldOr(item, "channel_group", FieldOr(item, "channel_group_id", -1)));
  }
  // EPG linkage: try a nested epg_data object first, then the channel's
  // own effective (override-aware) field, then its plain one -- the live
  // channels list carries no nested epg_data object at all, only the
  // flat/effective fields, but keep the nested-object branch in case a
  // future Dispatcharr revision adds one back.
  if (item.contains("epg_data") && item["epg_data"].is_object())
    ch.tvgId = FieldOr<std::string>(item["epg_data"], "tvg_id", "");
  else
    ch.tvgId = FieldOr<std::string>(item, "effective_tvg_id", FieldOr<std::string>(item, "tvg_id", ""));
  // effective_epg_data_id can point at a *different* EPG source's row
  // than tvgId above resolves to -- see DispatcharrClient::ResolveSeriesRuleTvgId().
  ch.epgDataId = FieldOr(item, "effective_epg_data_id", FieldOr(item, "epg_data_id", 0));

  ch.catchupEnabled = FieldOr(item, "is_catchup", false);
  ch.catchupDays = FieldOr(item, "catchup_days", 0);
  return ch;
}

ChannelGroup ParseChannelGroupJson(const nlohmann::json& item)
{
  ChannelGroup g;
  g.id = FieldOr(item, "id", 0);
  g.name = FieldOr<std::string>(item, "name", "");
  return g;
}

} // namespace dispatcharr
