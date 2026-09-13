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
  ch.name = FieldOr<std::string>(item, "name", "");
  ch.channelNumber = FieldOr(item, "channel_number", FieldOr(item, "channel_num", 0));
  // Channels only carry a logo_id (an FK to a separate Logo object);
  // there's no logo_url field directly on the channel (that belongs to
  // the underlying Stream model). Resolve the actual image via
  // GetChannelLogoUrl(logoId), not a direct URL field. logo_id is
  // explicitly null (not absent) for channels with no logo.
  ch.logoId = FieldOr(item, "logo_id", -1);
  // Channel group may be a nested object or a bare id depending on the
  // serializer; handle both.
  if (item.contains("channel_group") && item["channel_group"].is_object())
  {
    ch.groupId = FieldOr(item["channel_group"], "id", -1);
    ch.groupName = FieldOr<std::string>(item["channel_group"], "name", "");
  }
  else
  {
    ch.groupId = FieldOr(item, "channel_group", FieldOr(item, "channel_group_id", -1));
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
