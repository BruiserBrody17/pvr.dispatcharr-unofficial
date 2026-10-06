#pragma once

#include "DispatcharrClient.h"

#include <nlohmann/json_fwd.hpp>

namespace dispatcharr
{

// Pure field-mapping core of DispatcharrClient::GetChannels()'s per-item
// loop -- maps a single /api/channels/channels/ item onto a Channel,
// including the documented fallback chains: channel_group as either a
// nested object or a bare id/channel_group_id (preferring
// effective_channel_group_id on the bare-id path), tvgId from a nested
// epg_data object (kept for a possible future Dispatcharr revision, even
// though a live channels list never carries one today) falling back to
// effective_tvg_id then plain tvg_id, and -- added 2026-09-26, fixing a
// real asymmetry found via a project-wide review, confirmed against
// Dispatcharr's own real current upstream source -- name/channelNumber/
// logoId each preferring their own effective_name/effective_channel_number/
// effective_logo_id (Dispatcharr's per-field ChannelOverride mechanism)
// over the raw field. The channel-number case specifically matters for
// EPG matching, not just display: Dispatcharr's own XMLTV export keys
// `<channel id>` by effective_channel_number in its default
// tvg_id_source mode (the mode this addon's own channel-number-keyed EPG
// lookup already assumes), so an overridden channel number that isn't
// also read here breaks EPG matching for that channel entirely.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_channel_parser.cpp.
Channel ParseChannelJson(const nlohmann::json& item);

// Pure field-mapping core of DispatcharrClient::GetChannelGroups()'s
// per-item loop.
ChannelGroup ParseChannelGroupJson(const nlohmann::json& item);

} // namespace dispatcharr
