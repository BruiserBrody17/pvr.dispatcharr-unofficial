#pragma once

#include "DispatcharrClient.h"

#include <nlohmann/json_fwd.hpp>

namespace dispatcharr
{

// Pure field-mapping core of DispatcharrClient::GetChannels()'s per-item
// loop -- maps a single /api/channels/channels/ item onto a Channel,
// including the documented fallback chains: channel_group as either a
// nested object or a bare id/channel_group_id, and tvgId from a nested
// epg_data object (kept for a possible future Dispatcharr revision, even
// though a live channels list never carries one today) falling back to
// effective_tvg_id then plain tvg_id.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_channel_parser.cpp.
Channel ParseChannelJson(const nlohmann::json& item);

// Pure field-mapping core of DispatcharrClient::GetChannelGroups()'s
// per-item loop.
ChannelGroup ParseChannelGroupJson(const nlohmann::json& item);

} // namespace dispatcharr
