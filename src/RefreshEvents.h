#pragma once

#include <chrono>
#include <string>

namespace dispatcharr
{

// What a refresh event on Dispatcharr's realtime "updates" socket means for this addon's own cached channel list and
// guide (docs/OPEN_ITEMS.md, the realtime refresh events entry; docs/EPG.md for the captured sequences).
//
// Dispatcharr tells every connected client, as it goes, how an EPG source or an M3U account is refreshing:
// {"type": "update", "data": {"type": "epg_refresh", "source": <id>, "action": ..., "progress": ..., "status": ...}}
// and the same with "m3u_refresh" and an "account". Captured live (2026-10-06, a real XMLTV source and a real M3U
// account, refreshed on demand): an EPG refresh runs downloading -> parsing_channels (which ends with its own
// status "success" at progress 100, a step, not the end) -> parsing_programs, whose last event is progress 100 with
// status "success"; an M3U refresh runs processing_groups -> parsing (progress climbing to 100 with status "parsing")
// -> one more "parsing" event with status "success" that carries the channel auto-sync's counts (channels_created /
// channels_updated / channels_deleted), then a "vod_refresh" that is not about channels. Anything else, including every
// progress event, a failure and an unrelated message, is not a signal.
enum class RefreshEventKind
{
  kNone,
  kGuideParsed,    // an EPG source finished parsing its programmes: Dispatcharr's guide now differs
  kChannelsSynced, // an M3U account finished and its channel auto-sync created, updated or deleted channels
};

// kGuideParsed for the final event of an EPG refresh, kChannelsSynced for the final event of an M3U refresh that
// changed channels (or does not say whether it did, which is treated as a change: refetching is the safe error),
// kNone for everything else, including malformed input.
RefreshEventKind ClassifyRefreshEvent(const std::string& message);

// How long after a finished EPG refresh the guide is fetched again: past Dispatcharr's own 300 s cache of the
// exported guide, which a refresh does not invalidate, so an earlier fetch is served the same stale guide and costs a
// full download (the same window, and the same reason, as a channel renumbering's follow-up fetch in Staleness.h).
constexpr std::chrono::seconds kGuideRefetchAfterRefreshEvent{330};

// When the follow-up guide fetch for an event seen at `now` should run, given the one already scheduled
// (`currentDueAt`, the zero time_point when none is). A fetch already scheduled for a time that is itself past this
// event's own window keeps it: it will pick up this refresh too, and each fetch clears the schedule, so one guide
// fetch serves every source that finishes inside the window. One scheduled sooner moves out to this event's window,
// since fetching earlier would be served Dispatcharr's cached copy of the old guide and clear the schedule.
std::chrono::steady_clock::time_point ScheduleGuideRefetchForEvent(std::chrono::steady_clock::time_point currentDueAt,
                                                                   std::chrono::steady_clock::time_point now);

} // namespace dispatcharr
