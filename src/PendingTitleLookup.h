#pragma once

#include <algorithm>
#include <chrono>
#include <vector>

namespace dispatcharr
{

// Both shared by DispatcharrClient::ParseRecordingJson()'s PendingTitle-
// cache lookup (see PendingTitle's own comment in DispatcharrClient.h for
// the full story: a short-lived, best-effort bridge that fills in a
// recording's title from the "Record" request that started it, before
// Dispatcharr's own async enrichment has caught up). Templated (duck-typed
// on the member(s) each needs, same convention as SegmentLookup.h/
// LiveEdgeMargin.h) rather than depending on the real, private
// PendingTitle type directly, so both are unit-testable standalone with a
// minimal synthetic struct; see ../tests/test_pending_title_lookup.cpp.
// The mutex guarding the real cache, and the vector mutation itself, stay
// in ParseRecordingJson() -- neither is available to a free function.

// Drops every entry older than `ttl` as of `now`. EntryT only needs an
// `insertedAt` member (a std::chrono::steady_clock::time_point).
template <typename EntryT>
void PruneExpiredPendingTitles(std::vector<EntryT>& entries, std::chrono::steady_clock::time_point now,
                               std::chrono::steady_clock::duration ttl)
{
  entries.erase(
      std::remove_if(entries.begin(), entries.end(), [&](const EntryT& e) { return now - e.insertedAt > ttl; }),
      entries.end());
}

// Finds the entry for `recordingId`, or nullptr if none match. Deliberately
// does not erase on match -- see PendingTitle's own comment on why: this
// runs on every poll until Dispatcharr's own enrichment lands, and erasing
// after the first match would make the title flicker back to a
// placeholder on the very next poll if enrichment hadn't caught up yet.
// Callers should prune expired entries first (via
// PruneExpiredPendingTitles() above) if a stale match shouldn't count.
// EntryT needs a recordingId member.
//
// Keyed by the real Dispatcharr recording id, not channelId -- fix for a
// real, confirmed bug found via a project-wide review (a 32nd-pass audit,
// flagged but not fixed by an earlier, 25th-pass audit -- see this
// function's own git history/PendingTitle's own comment), not itself
// independently reproduced: matching by channel alone let ANY other
// untitled recording on that same channel (an old completed "Custom
// Recording" that never got a title from Dispatcharr at all, or a
// different, unrelated future scheduled timer) borrow a brand-new
// recording's own in-flight title for the rest of this cache entry's TTL
// -- not just a display glitch, since a borrowed title landing in Kodi's
// own cached copy of an unrelated timer, followed by any edit to that
// timer before Kodi's next refresh, could pass ShouldRenameOnTimerEdit()'s
// own placeholder check (it only recognizes THIS recording's own
// "Recording <id>" default, not an entirely different borrowed title) and
// get permanently written back via RenameRecording() -- silently
// corrupting an unrelated recording's real title server-side and marking
// it user_edited, which blocks Dispatcharr's own future auto-enrichment
// for it entirely.
template <typename EntryT>
const EntryT* FindPendingTitleForRecording(const std::vector<EntryT>& entries, int recordingId)
{
  for (const auto& entry : entries)
  {
    if (entry.recordingId == recordingId)
      return &entry;
  }
  return nullptr;
}

} // namespace dispatcharr
