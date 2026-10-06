#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace dispatcharr
{

// Series rules have no numeric id at all in Dispatcharr's API (confirmed
// against a real rule: {mode, title, tvg_id, channel_id, title_mode,
// description, description_mode} -- nothing else), so PVRDispatcharr::
// GetTimers() can't use one for a Kodi PVR_TIMER's ClientIndex the way
// every other timer kind does. Hashes the (title, tvgId, epgSourceId)
// triple instead -- the same identity Dispatcharr's own
// SeriesRulesAPIView.post() upsert-matches by (confirmed against its
// real current upstream source) -- masked into the lower 30 bits so the
// high "series rule" flag bit (0x40000000, applied by the caller
// alongside this) is never disturbed.
//
// epgSourceId joined in added 2026-09-29, fixing a real, confirmed gap
// found via a project-wide review, confirmed against Dispatcharr's own
// real current upstream source (see docs/OPEN_ITEMS.md): two rules
// sharing (title, tvgId) but pinned to *different* EPG sources are
// genuinely distinct rules server-side (that's exactly what
// epg_source_id is for), but used to hash to the identical ClientIndex
// here -- so Kodi's own Timers list could only ever show/edit/delete
// one of them, with no way to tell which, and a delete of either one
// removed whichever this addon's own DeleteSeriesRule() call happened
// to match first.
//
// Deliberately does NOT return a value stable across process restarts or
// platforms/compilers: std::hash<std::string> is only guaranteed
// consistent within one running process, which is exactly what a
// transient, in-memory Kodi ClientIndex needs -- it's never persisted or
// compared across a restart. Pulled out here specifically so the masking
// behavior is unit-testable standalone; see
// ../tests/test_timer_identity.cpp.
inline unsigned int ComputeSeriesRuleClientIndex(const std::string& title, const std::string& tvgId, int epgSourceId)
{
  // Normalizes any non-positive value to a single canonical 0 before
  // hashing -- matching Dispatcharr's own parse_optional_epg_source_id()
  // treating anything <= 0 as "not pinned" -- so two callers that both
  // mean "unpinned" but happen to represent it differently (0, a
  // negative sentinel) still hash identically.
  int normalizedEpgSourceId = epgSourceId > 0 ? epgSourceId : 0;
  std::size_t h = std::hash<std::string>()(title + '\x1f' + tvgId + '\x1f' + std::to_string(normalizedEpgSourceId));
  return (static_cast<unsigned int>(h) & 0x3FFFFFFFu) | 0x40000000u;
}

// Which kind of timer a ClientIndex was encoded for -- see
// DecodeTimerClientIndex()'s own comment.
enum class TimerIdentityKind
{
  OneTimeRecording,
  RecurringRule,
  SeriesRule,
};

// id is the underlying Recording's id (OneTimeRecording) or
// RecurringRecordingRule's id (RecurringRule); unused (left 0) for
// SeriesRule, which has no numeric id in Dispatcharr's API at all (see
// ComputeSeriesRuleClientIndex()'s own comment).
struct DecodedTimerIdentity
{
  TimerIdentityKind kind;
  int id;
};

// Decodes a Kodi PVR_TIMER's own ClientIndex back into which kind of timer
// it represents and (for the two kinds that have one) its underlying id --
// shared by PVRDispatcharr::UpdateTimer()/DeleteTimer(), which each
// independently repeated this same classification. Mirrors the exact bit
// layout GetTimers() encodes: the high bit (0x40000000, matching
// ComputeSeriesRuleClientIndex()'s own mask above) flags a series rule's
// timer; recurringRuleIndexFlag (0x20000000, PVRDispatcharr::
// kRecurringRuleIndexFlag -- passed in rather than duplicated as a second
// hardcoded literal here, since unlike the series-rule bit it isn't this
// header's own convention to define) flags a recurring rule's own parent
// timer; anything else is a plain one-time recording, whose ClientIndex is
// just its Recording's own id, unmasked.
inline DecodedTimerIdentity DecodeTimerClientIndex(unsigned int clientIndex, unsigned int recurringRuleIndexFlag)
{
  constexpr unsigned int kSeriesRuleIndexFlag = 0x40000000u;
  if (clientIndex & kSeriesRuleIndexFlag)
    return {TimerIdentityKind::SeriesRule, 0};
  if (clientIndex & recurringRuleIndexFlag)
    return {TimerIdentityKind::RecurringRule, static_cast<int>(clientIndex & ~recurringRuleIndexFlag)};
  return {TimerIdentityKind::OneTimeRecording, static_cast<int>(clientIndex & ~kSeriesRuleIndexFlag)};
}

// PVRDispatcharr::UpdateTimer()'s own "extend an already-recording timer"
// math: Dispatcharr's dedicated extend endpoint takes a relative
// extra_minutes, not the absolute end time Kodi's timer-edit dialog hands
// back, so the delta against the recording's current end time has to be
// converted -- ceiled up to a whole minute (a 61s delta should extend by
// 2 minutes, not truncate to 1) rather than floored, so a small requested
// extension never silently rounds down to zero effect. Returns false
// (leaving extraMinutesOut untouched) for a non-positive delta, matching
// the server's own validation (extend/ rejects extra_minutes <= 0) --
// Kodi's timer-edit dialog has no separate "shorten" action, so a user
// picking an earlier or unchanged end time here just means they didn't
// actually intend to extend anything.
inline bool ComputeRecordingExtendMinutes(time_t currentEndTime, time_t requestedEndTime, int& extraMinutesOut)
{
  time_t deltaSeconds = requestedEndTime - currentEndTime;
  if (deltaSeconds <= 0)
    return false;
  extraMinutesOut = static_cast<int>((deltaSeconds + 59) / 60);
  return true;
}

// Fix for a real, confirmed bug in both of UpdateTimer()'s one-time-
// recording branches: `kodiTitle` (timer.GetTitle(), Kodi's own cached
// copy of the timer, only refreshed by this addon's own GetTimers()/
// recording_refresh_minutes -- up to 5 minutes stale by default, or
// longer with no realtime updates) can itself be the synthesized
// "Recording <id>" placeholder ParseRecordingJson() falls back to before
// Dispatcharr's async title enrichment has caught up (see
// DispatcharrClient.cpp's own "Recording <id>" default). Comparing that
// stale placeholder against a freshly re-fetched `freshTitle` sees a
// "change" that isn't a real user edit at all -- extending an
// already-recording timer via Kodi's "record for longer" OSD action
// (which round-trips the timer's cached title back unchanged) would
// otherwise call RenameRecording() and permanently overwrite the real,
// since-enriched title with the stale placeholder, also marking
// user_edited server-side and blocking any further auto-enrichment --
// found via a project-wide review, not itself independently reproduced.
//
// Also ignores `kodiTitle` when it's exactly `freshTitle` cut down to
// kKodiTimerTitleMaxBytes (added 2026-09-27, a 66th-pass audit, fixing a
// real, confirmed gap of the same "Kodi's own copy isn't a real user
// edit" class, found via a project-wide review, confirmed against Kodi's
// own real current SDK source and Dispatcharr's own real current
// upstream source, not itself independently reproduced): PVR_TIMER's
// `strTitle` is a fixed `char[PVR_ADDON_NAME_STRING_LENGTH]` (1024,
// kodi/c-api/addon-instance/pvr/pvr_defines.h), filled via a byte-level
// `strncpy(..., sizeof - 1)` both by this addon's own GetTimers()
// (`kodi::addon::PVRTimer::SetTitle()`) and by Kodi's own
// `CPVRTimerInfoTag::FillAddonData()` on the way back into UpdateTimer().
// A Dispatcharr recording's title has no length limit of its own once
// set via its `update-metadata` action (apps/channels/api_views.py
// stores it into `custom_properties.program.title` unbounded), so a
// title over 1023 bytes always comes back from Kodi as its own
// truncated prefix -- and any unrelated edit (extending an in-progress
// recording, rescheduling a pending one) used to read that as a rename,
// permanently truncating the real title server-side (or, when the cut
// landed mid-UTF-8-sequence, failing the request outright -- see
// TrySerializeJsonBody(), JsonFieldUtil.h). A user genuinely renaming to
// exactly that 1023-byte prefix is indistinguishable and not worth
// supporting.
constexpr std::size_t kKodiTimerTitleMaxBytes = 1023;

inline bool ShouldRenameOnTimerEdit(const std::string& kodiTitle, const std::string& freshTitle, int recordingId)
{
  if (kodiTitle.empty())
    return false;
  if (kodiTitle == "Recording " + std::to_string(recordingId))
    return false;
  if (freshTitle.size() > kKodiTimerTitleMaxBytes && kodiTitle == freshTitle.substr(0, kKodiTimerTitleMaxBytes))
    return false;
  return kodiTitle != freshTitle;
}

// The recurring-rule counterpart of ShouldRenameOnTimerEdit() above, for
// a real, confirmed bug found via a project-wide review, not itself
// independently reproduced (this branch never got the equivalent guard
// when the one-time-recording fix above was added): GetTimers() falls
// back to a synthesized "Recurring recording <id>" placeholder title
// when a rule's own `name` is empty (e.g. a rule created directly via
// Dispatcharr's own web UI with no name at all) -- but unlike the
// one-time-recording case, UpdateRecurringRule()'s single PATCH call has
// no separate "don't touch name" option, it always sends whatever
// `name` value it's given. Any later edit (including a bare
// enable/disable toggle, which Kodi implements as UpdateTimer() with
// every other field -- including the displayed placeholder title --
// echoed back unchanged) would otherwise send that placeholder straight
// back as the rule's own new, permanent `name`, silently overwriting an
// intentionally-empty name server-side.
//
// Returns the value UpdateTimer() should actually send as `name`:
// `kodiTitle` unless it's exactly this rule's own placeholder, in which
// case `cachedName` (the rule's real, possibly still-empty, last-known
// name) is sent instead, preserving it across an edit that isn't
// actually a rename. A real user-entered title that happens to match a
// *different* rule's placeholder is still sent through unchanged, same
// tradeoff ShouldRenameOnTimerEdit() above already accepts.
inline std::string ResolveRecurringRuleNameForUpdate(const std::string& kodiTitle, const std::string& cachedName,
                                                     int ruleId)
{
  if (kodiTitle == "Recurring recording " + std::to_string(ruleId))
    return cachedName;
  return kodiTitle;
}

// Fix for a real, confirmed bug found via a project-wide review, not
// itself independently reproduced: PVRDispatcharr::AddTimer()/
// UpdateTimer()'s own series-rule branches sent timer.GetTitle() to
// Dispatcharr as the actual EPG-title match pattern -- but Kodi's own
// timer-settings dialog treats "Search guide for"
// (timer.GetEPGSearchString(), shown whenever the timer type declares
// PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH) as the real match pattern,
// and "Name" (timer.GetTitle()) as a separate, purely cosmetic label --
// confirmed against Kodi's own source
// (xbmc/pvr/dialogs/GUIDialogPVRTimerSettings.cpp): the two fields are
// edited and saved back completely independently. Sending GetTitle()
// instead meant editing "Search guide for" alone was silently dropped
// (the addon never read it at all), and editing "Name" alone -- which
// Kodi's own dialog presents as cosmetic -- silently changed
// Dispatcharr's own match rule instead. The ordinary "Record" button
// from an EPG guide entry happened to work correctly only because
// Kodi's own CPVRTimerInfoTag::CreateFromEpg() sets both fields to the
// same EPG title for a brand-new timer.
//
// Falls back to `title` when `epgSearchString` is empty, matching
// Kodi's own dialog convention for a genuinely new timer (which, unlike
// an existing one, does NOT auto-fill an empty search field from the
// title before handing the timer back) -- a manually-created series
// timer with "Search guide for" left blank still matches on whatever
// the user put in "Name", the same as before this fix for that specific
// case.
inline std::string ResolveSeriesRuleMatchTitle(const std::string& epgSearchString, const std::string& title)
{
  return epgSearchString.empty() ? title : epgSearchString;
}

// Whether `rules` holds a rule with this title and tvg_id that is pinned to an EPG source (> 0), for
// the delete of an UNPINNED rule with that title and tvg_id. Dispatcharr's delete only restricts by
// source when the request names one, so deleting the unpinned (title, tvg_id) rule matched and removed
// the pinned sibling too, purging its future recordings (found by the 2026-10-04 eighth hardening
// sweep: a Kodi-created rule beside a Guide-created pinned one, then a channel change or a plain delete).
// RuleT needs `title`, `tvgId` and `epgSourceId` members, like FindSeriesRuleIndexByClientIndex().
template <typename RuleT>
bool HasPinnedSiblingSeriesRule(const std::vector<RuleT>& rules, const std::string& title, const std::string& tvgId)
{
  for (const auto& rule : rules)
  {
    if (rule.title == title && rule.tvgId == tvgId && rule.epgSourceId > 0)
      return true;
  }
  return false;
}

// The EPG source a series rule edit sends. The source is not independent of the channel: Dispatcharr
// pins a rule to one source so the rule's tvg_id resolves inside that source's own guide data. When
// the edit keeps the same tvg_id (an HD and an SD variant of one channel) the pin is echoed and the
// upsert edits the pinned rule in place. When the channel change re-derives a DIFFERENT tvg_id the
// old source need not carry that tvg_id at all: echoing it created a rule (title, new tvg_id, old
// source) that most likely matches nothing while the replace logic deleted the working one (found by
// the 2026-10-04 seventh hardening sweep, a regression of the sixth sweep's echo). Such a rule is
// sent unpinned (0), as before that echo; the old pinned rule is still deleted by its own source.
inline int ResolveSeriesRuleSourceOnEdit(const std::string& oldTvgId, const std::string& newTvgId, int oldEpgSourceId)
{
  if (oldEpgSourceId <= 0)
    return 0;
  return oldTvgId == newTvgId ? oldEpgSourceId : 0;
}

// Whether an edit of a series rule changed its identity, so that saving it
// creates a second rule beside the original instead of editing it (the
// docs/OPEN_ITEMS.md entry "Editing a series rule's match pattern creates a
// duplicate"). Dispatcharr's upsert keys a rule by (tvg_id, title, epg_source_id)
// with exact, case-sensitive string comparison, so a new search pattern or a
// channel change that re-derives tvg_id leaves the old rule -- and its future
// recordings -- in place.
//
// Title and tvg_id, never an EPG source on its own: the source is echoed unchanged from
// the cache on every edit, whichever channel the edit picks (nothing in Kodi can change
// it), and a delete of the old rule that omits the source matches *every* source's rule
// with that title and tvg_id -- including a legacy unsourced rule the same save just
// upgraded in place. With title or tvg_id different, the delete matches only the old rule.
inline bool ShouldReplaceSeriesRuleOnEdit(const std::string& oldTitle, const std::string& oldTvgId,
                                          const std::string& newTitle, const std::string& newTvgId)
{
  return oldTitle != newTitle || oldTvgId != newTvgId;
}

// The same decision with the sources, for a rule that IS pinned to one. A different source
// is a different identity too -- (title, tvg_id, 5) and (title, tvg_id, 7) are two rules -- so
// saving one under the other leaves the old behind, and the delete (scoped to the old
// source, which is > 0 here) removes exactly that rule. An unsourced old rule is left to the
// title/tvg_id test: deleting it without a source would match the new rule as well. Any
// non-positive source is "unpinned", as Dispatcharr's own parse treats it.
inline bool ShouldReplaceSeriesRuleOnEdit(const std::string& oldTitle, const std::string& oldTvgId, int oldEpgSourceId,
                                          const std::string& newTitle, const std::string& newTvgId, int newEpgSourceId)
{
  if (ShouldReplaceSeriesRuleOnEdit(oldTitle, oldTvgId, newTitle, newTvgId))
    return true;
  const int oldSource = oldEpgSourceId > 0 ? oldEpgSourceId : 0;
  const int newSource = newEpgSourceId > 0 ? newEpgSourceId : 0;
  return oldSource > 0 && oldSource != newSource;
}

// Fix for a real, confirmed bug: PVRDispatcharr::UpdateTimer()/DeleteTimer()'s
// own series-rule branches used to re-derive tvgId from the channel's
// own *current* state (ResolveSeriesRuleTvgId(ch->epgDataId, ch->tvgId))
// instead of using the series rule's own originally-stored identity --
// the one ComputeSeriesRuleClientIndex() actually hashed into this exact
// ClientIndex back in GetTimers(). A channel's EPG mapping drifting
// after the rule was created (or a rule created before
// ResolveSeriesRuleTvgId() itself existed, see docs/RECORDINGS.md) meant
// the re-derived tvgId no longer matched what Dispatcharr's own
// DeleteSeriesRule()/CreateSeriesRule() upsert actually stored -- a
// DELETE silently matched nothing (the rule was never removed) and an
// UPDATE's own upsert silently created a *separate* rule under the new,
// drifted identity instead of editing the existing one.
//
// Finds the index in `rules` whose own stored (title, tvgId, epgSourceId)
// hashes to `clientIndex` via ComputeSeriesRuleClientIndex() above -- the
// reverse lookup needed to recover that originally-stored identity.
// Returns -1 if none match (e.g. the rule was already deleted
// server-side since GetTimers() last cached it), in which case callers
// should fall back to today's channel-derived value rather than fail
// outright. RuleT needs title/tvgId/epgSourceId members (duck-typed via
// the template, matching this project's own convention for
// SegmentLookup.h/LiveEdgeMargin.h/CatchUpUtil.h) so this stays
// independent of DispatcharrClient.h's own (heavier) TimerRule type; see
// ../tests/test_timer_identity.cpp.
template <typename RuleT>
int FindSeriesRuleIndexByClientIndex(const std::vector<RuleT>& rules, unsigned int clientIndex)
{
  for (std::size_t i = 0; i < rules.size(); ++i)
  {
    if (ComputeSeriesRuleClientIndex(rules[i].title, rules[i].tvgId, rules[i].epgSourceId) == clientIndex)
      return static_cast<int>(i);
  }
  return -1;
}

// Whether a series rule's own cached channelId (this addon's "no channel"
// convention: <= 0, see TimerRuleParser.cpp's own FieldOr default) still
// matches a timer's current Kodi-side clientChannelUid, for
// PVRDispatcharr::UpdateTimer()'s own "has the channel actually changed
// from what this rule was created against" cache-match check.
//
// Fix for a real, confirmed bug found via a project-wide review
// (a 30th-pass audit, confirmed against Kodi's own real current SDK
// source, not itself independently reproduced): a channel-less series
// rule (Dispatcharr's own "Record series" Guide button creates one with
// no pinned channel -- see GetTimers()'s own comment) is sent to Kodi
// with PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL declared and
// SetClientChannelUid(PVR_CHANNEL_INVALID_UID), i.e. -1 -- but a plain
// `cachedChannelId == timerChannelUid` comparison (0 == -1) always reads
// as "channel changed" for exactly this case, missing the cache and
// falling back to FindChannelByUid(-1), which never matches any real
// channel -- silently resolving tvgId to "" instead of the rule's own
// real, cached tvgId. Dispatcharr's own upsert then no longer matches
// the original rule (title, tvgId) and appends a brand-new rule with
// tvg_id="" (matches every channel), rather than editing the one the
// user actually meant to.
//
// Treats "no channel" (cachedChannelId <= 0) on one side and
// PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL's own sentinel (timerChannelUid <= 0,
// avoiding a Kodi SDK header dependency here -- Dispatcharr channel ids
// are always positive, and PVR_CHANNEL_INVALID_UID is -1) on the other as
// the same "no channel", not different values that happen to disagree.
inline bool IsSameSeriesRuleChannel(int cachedChannelId, int timerChannelUid)
{
  bool cachedHasNoChannel = cachedChannelId <= 0;
  bool timerHasNoChannel = timerChannelUid <= 0;
  if (cachedHasNoChannel || timerHasNoChannel)
    return cachedHasNoChannel && timerHasNoChannel;
  return cachedChannelId == timerChannelUid;
}

} // namespace dispatcharr
