#include "TimerIdentity.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ComputeSeriesRuleClientIndex is deterministic for the same input", "[TimerIdentity]")
{
  CHECK(ComputeSeriesRuleClientIndex("My Show", "abc.tvg", 0) == ComputeSeriesRuleClientIndex("My Show", "abc.tvg", 0));
}

TEST_CASE("ComputeSeriesRuleClientIndex always sets the series-rule flag bit (0x40000000)", "[TimerIdentity]")
{
  CHECK((ComputeSeriesRuleClientIndex("My Show", "abc.tvg", 0) & 0x40000000u) != 0);
  CHECK((ComputeSeriesRuleClientIndex("", "", 0) & 0x40000000u) != 0);
}

TEST_CASE("ComputeSeriesRuleClientIndex never sets bit 0x80000000 (masked out of the hash)", "[TimerIdentity]")
{
  // The top bit is masked out of the hash itself (& 0x3FFFFFFFu) before the
  // flag bit is OR'd in, so this bit combination (both 0x80000000 and
  // 0x40000000 set) should never occur -- distinguishes this from a
  // different id scheme this project uses elsewhere (kRecurringRuleIndexFlag),
  // which does use the top bit.
  for (const auto& [title, tvgId] : {std::pair<std::string, std::string>{"A", "1"}, {"B", "2"}, {"", ""}, {"C", "3"}})
  {
    unsigned int index = ComputeSeriesRuleClientIndex(title, tvgId, 0);
    CHECK((index & 0x80000000u) == 0);
  }
}

TEST_CASE("ComputeSeriesRuleClientIndex distinguishes different titles with the same tvgId", "[TimerIdentity]")
{
  CHECK(ComputeSeriesRuleClientIndex("Show A", "same.tvg", 0) != ComputeSeriesRuleClientIndex("Show B", "same.tvg", 0));
}

TEST_CASE("ComputeSeriesRuleClientIndex distinguishes the same title with different tvgIds", "[TimerIdentity]")
{
  CHECK(ComputeSeriesRuleClientIndex("Same Show", "tvg.a", 0) != ComputeSeriesRuleClientIndex("Same Show", "tvg.b", 0));
}

TEST_CASE("ComputeSeriesRuleClientIndex handles empty title/tvgId without crashing", "[TimerIdentity]")
{
  CHECK((ComputeSeriesRuleClientIndex("", "", 0) & 0x40000000u) != 0);
}

TEST_CASE("ComputeSeriesRuleClientIndex distinguishes the same title+tvgId pinned to different EPG sources",
          "[TimerIdentity]")
{
  // The real, confirmed bug this fixes (see docs/OPEN_ITEMS.md): two
  // genuinely distinct rules server-side (Dispatcharr's own upsert
  // matches by title+tvg_id+epg_source_id) used to collide onto the
  // same ClientIndex here, since epgSourceId wasn't part of the hash at
  // all.
  CHECK(ComputeSeriesRuleClientIndex("Same Show", "same.tvg", 1) !=
        ComputeSeriesRuleClientIndex("Same Show", "same.tvg", 2));
}

TEST_CASE("ComputeSeriesRuleClientIndex treats every non-positive epgSourceId as the same canonical 'unpinned'",
          "[TimerIdentity]")
{
  // Matches Dispatcharr's own parse_optional_epg_source_id() treating
  // anything <= 0 as "not pinned" -- two callers representing "unpinned"
  // differently (0, a negative sentinel) must still agree.
  CHECK(ComputeSeriesRuleClientIndex("Same Show", "same.tvg", 0) ==
        ComputeSeriesRuleClientIndex("Same Show", "same.tvg", -1));
}

// ---------------------------------------------------------------------
// DecodeTimerClientIndex
// ---------------------------------------------------------------------

constexpr unsigned int kTestRecurringRuleIndexFlag = 0x20000000u;

TEST_CASE("DecodeTimerClientIndex identifies a series rule and leaves id at 0", "[TimerIdentity]")
{
  unsigned int clientIndex = ComputeSeriesRuleClientIndex("My Show", "abc.tvg", 0);
  DecodedTimerIdentity decoded = DecodeTimerClientIndex(clientIndex, kTestRecurringRuleIndexFlag);
  CHECK(decoded.kind == TimerIdentityKind::SeriesRule);
  CHECK(decoded.id == 0);
}

TEST_CASE("DecodeTimerClientIndex identifies a recurring rule and unmasks its id", "[TimerIdentity]")
{
  unsigned int clientIndex = 42u | kTestRecurringRuleIndexFlag;
  DecodedTimerIdentity decoded = DecodeTimerClientIndex(clientIndex, kTestRecurringRuleIndexFlag);
  CHECK(decoded.kind == TimerIdentityKind::RecurringRule);
  CHECK(decoded.id == 42);
}

TEST_CASE("DecodeTimerClientIndex treats an unmasked ClientIndex as a plain one-time recording", "[TimerIdentity]")
{
  DecodedTimerIdentity decoded = DecodeTimerClientIndex(7u, kTestRecurringRuleIndexFlag);
  CHECK(decoded.kind == TimerIdentityKind::OneTimeRecording);
  CHECK(decoded.id == 7);
}

TEST_CASE("DecodeTimerClientIndex prioritizes the series-rule bit over the recurring-rule bit", "[TimerIdentity]")
{
  // Not a real combination GetTimers() ever produces, but the series check
  // must still come first per the documented bit layout.
  unsigned int clientIndex = 0x40000000u | kTestRecurringRuleIndexFlag;
  DecodedTimerIdentity decoded = DecodeTimerClientIndex(clientIndex, kTestRecurringRuleIndexFlag);
  CHECK(decoded.kind == TimerIdentityKind::SeriesRule);
}

// ---------------------------------------------------------------------
// ComputeRecordingExtendMinutes
// ---------------------------------------------------------------------

TEST_CASE("ComputeRecordingExtendMinutes ceils a partial-minute delta up", "[TimerIdentity]")
{
  int extraMinutes = 0;
  // 61s delta -- must round up to 2 minutes, not truncate to 1.
  CHECK(ComputeRecordingExtendMinutes(1000, 1061, extraMinutes));
  CHECK(extraMinutes == 2);
}

TEST_CASE("ComputeRecordingExtendMinutes returns exact whole-minute deltas unchanged", "[TimerIdentity]")
{
  int extraMinutes = 0;
  CHECK(ComputeRecordingExtendMinutes(1000, 1000 + 300, extraMinutes));
  CHECK(extraMinutes == 5);
}

TEST_CASE("ComputeRecordingExtendMinutes rejects a zero delta", "[TimerIdentity]")
{
  int extraMinutes = 999;
  CHECK_FALSE(ComputeRecordingExtendMinutes(1000, 1000, extraMinutes));
  CHECK(extraMinutes == 999); // left untouched
}

TEST_CASE("ComputeRecordingExtendMinutes rejects a negative delta (a shorten, not an extend)", "[TimerIdentity]")
{
  int extraMinutes = 999;
  CHECK_FALSE(ComputeRecordingExtendMinutes(1000, 900, extraMinutes));
  CHECK(extraMinutes == 999);
}

TEST_CASE("ComputeRecordingExtendMinutes accepts a 1-second delta as a 1-minute extension", "[TimerIdentity]")
{
  int extraMinutes = 0;
  CHECK(ComputeRecordingExtendMinutes(1000, 1001, extraMinutes));
  CHECK(extraMinutes == 1);
}

// ---------------------------------------------------------------------
// ShouldRenameOnTimerEdit
// ---------------------------------------------------------------------

TEST_CASE("ShouldRenameOnTimerEdit is true for a genuine title change", "[TimerIdentity]")
{
  CHECK(ShouldRenameOnTimerEdit("New Title", "Old Title", 42));
}

TEST_CASE("ShouldRenameOnTimerEdit is false when the title didn't actually change", "[TimerIdentity]")
{
  CHECK_FALSE(ShouldRenameOnTimerEdit("Same Title", "Same Title", 42));
}

TEST_CASE("ShouldRenameOnTimerEdit is false for an empty Kodi-side title", "[TimerIdentity]")
{
  CHECK_FALSE(ShouldRenameOnTimerEdit("", "Real Title", 42));
}

TEST_CASE("ShouldRenameOnTimerEdit ignores a stale client-side 'Recording <id>' placeholder", "[TimerIdentity]")
{
  // The real, confirmed bug this fixes: Kodi's own cached timer title can
  // still be the synthesized "Recording <id>" default (see
  // DispatcharrClient.cpp's ParseRecordingJson()) from before Dispatcharr's
  // async title enrichment caught up. Extending an already-recording timer
  // via Kodi's "record for longer" OSD action round-trips that stale
  // placeholder back unchanged -- without this guard, that read as a
  // genuine title edit and permanently overwrote the real, since-enriched
  // title with the placeholder.
  CHECK_FALSE(ShouldRenameOnTimerEdit("Recording 42", "Real Enriched Title", 42));
}

TEST_CASE("ShouldRenameOnTimerEdit still catches the placeholder as a real title for a DIFFERENT recording id",
          "[TimerIdentity]")
{
  // "Recording 42" is only a placeholder for recording id 42 specifically
  // -- a real user-entered title that happens to look like a different
  // recording's placeholder shouldn't be silently ignored.
  CHECK(ShouldRenameOnTimerEdit("Recording 42", "Some Other Title", 99));
}

TEST_CASE("ShouldRenameOnTimerEdit ignores Kodi's own byte-truncated copy of an over-long title", "[TimerIdentity]")
{
  // Regression: Kodi's PVR_TIMER strTitle is char[1024], filled via a
  // byte-level strncpy(..., 1023) -- an unrelated edit (e.g. extending an
  // in-progress recording) used to read the truncated copy as a rename
  // and truncate the real title server-side.
  std::string full(1500, 'A');
  CHECK_FALSE(ShouldRenameOnTimerEdit(full.substr(0, kKodiTimerTitleMaxBytes), full, 42));

  // Same, when the cut lands mid-UTF-8-sequence (2-byte characters).
  std::string multiByte;
  for (int i = 0; i < 600; ++i)
    multiByte += "\xD0\x96";
  CHECK_FALSE(ShouldRenameOnTimerEdit(multiByte.substr(0, kKodiTimerTitleMaxBytes), multiByte, 42));
}

TEST_CASE("ShouldRenameOnTimerEdit still renames an over-long title to a genuinely different one", "[TimerIdentity]")
{
  std::string full(1500, 'A');
  CHECK(ShouldRenameOnTimerEdit("Channel A special", full, 42));
  // A prefix that isn't exactly Kodi's own truncation length is a real edit.
  CHECK(ShouldRenameOnTimerEdit(full.substr(0, 500), full, 42));
}

TEST_CASE("ShouldRenameOnTimerEdit's truncation guard doesn't apply to a title Kodi could hold in full",
          "[TimerIdentity]")
{
  // freshTitle exactly kKodiTimerTitleMaxBytes long fits unmodified, so
  // an identical kodiTitle is just "unchanged", and a shorter prefix is a
  // real edit, not truncation.
  std::string fits(kKodiTimerTitleMaxBytes, 'B');
  CHECK_FALSE(ShouldRenameOnTimerEdit(fits, fits, 42));
  CHECK(ShouldRenameOnTimerEdit(fits.substr(0, 1000), fits, 42));
}

// ---------------------------------------------------------------------
// ResolveRecurringRuleNameForUpdate
// ---------------------------------------------------------------------

TEST_CASE("ResolveRecurringRuleNameForUpdate sends the Kodi title through unchanged for a genuine rename",
          "[TimerIdentity]")
{
  CHECK(ResolveRecurringRuleNameForUpdate("New Name", "Old Name", 42) == "New Name");
}

TEST_CASE("ResolveRecurringRuleNameForUpdate preserves the cached (possibly empty) name instead of the "
          "placeholder -- the real bug this fixes",
          "[TimerIdentity]")
{
  // The recurring-rule counterpart of the one-time-recording placeholder
  // bug: a rule with no name displays as "Recurring recording <id>" in
  // Kodi's Timers list, and any edit that doesn't touch the title at all
  // (e.g. a bare enable/disable toggle) echoes that placeholder straight
  // back. Without this, UpdateRecurringRule()'s own PATCH -- which always
  // sends whatever `name` it's given, unlike a one-time recording's
  // separate RenameRecording() call -- would permanently overwrite the
  // rule's genuinely empty name with the placeholder text.
  CHECK(ResolveRecurringRuleNameForUpdate("Recurring recording 42", "", 42) == "");
}

TEST_CASE("ResolveRecurringRuleNameForUpdate preserves a real cached name too, not just an empty one",
          "[TimerIdentity]")
{
  CHECK(ResolveRecurringRuleNameForUpdate("Recurring recording 42", "Real Cached Name", 42) == "Real Cached Name");
}

TEST_CASE("ResolveRecurringRuleNameForUpdate still catches the placeholder as a real title for a DIFFERENT rule id",
          "[TimerIdentity]")
{
  // "Recurring recording 42" is only a placeholder for rule id 42
  // specifically -- a real user-entered title that happens to look like
  // a different rule's placeholder shouldn't be silently ignored.
  CHECK(ResolveRecurringRuleNameForUpdate("Recurring recording 42", "Some Other Name", 99) == "Recurring recording 42");
}

// ---------------------------------------------------------------------
// ResolveSeriesRuleMatchTitle
// ---------------------------------------------------------------------

TEST_CASE("ResolveSeriesRuleMatchTitle prefers the EPG search string when present -- the real bug this fixes",
          "[TimerIdentity]")
{
  // The real bug: this addon used to send the cosmetic "Name" field
  // (title) as the actual match pattern, silently dropping any edit to
  // "Search guide for" specifically.
  CHECK(ResolveSeriesRuleMatchTitle("The Real Search Pattern", "Cosmetic Name") == "The Real Search Pattern");
}

TEST_CASE("ResolveSeriesRuleMatchTitle falls back to title when the search string is empty", "[TimerIdentity]")
{
  // Matches Kodi's own dialog convention for a genuinely new timer,
  // which doesn't auto-fill an empty search field from the title before
  // handing the timer back -- a manually-created series timer with
  // "Search guide for" left blank still matches on "Name".
  CHECK(ResolveSeriesRuleMatchTitle("", "Some Title") == "Some Title");
}

// ---------------------------------------------------------------------
// FindSeriesRuleIndexByClientIndex
// ---------------------------------------------------------------------

namespace
{
struct FakeRule
{
  std::string title;
  std::string tvgId;
  int epgSourceId = 0;
};
} // namespace

TEST_CASE("FindSeriesRuleIndexByClientIndex finds the matching rule", "[TimerIdentity]")
{
  std::vector<FakeRule> rules = {{"Show A", "tvg.a"}, {"Show B", "tvg.b"}};
  unsigned int clientIndex = ComputeSeriesRuleClientIndex("Show B", "tvg.b", 0);
  CHECK(FindSeriesRuleIndexByClientIndex(rules, clientIndex) == 1);
}

TEST_CASE("FindSeriesRuleIndexByClientIndex returns -1 when no rule matches", "[TimerIdentity]")
{
  std::vector<FakeRule> rules = {{"Show A", "tvg.a"}};
  unsigned int clientIndex = ComputeSeriesRuleClientIndex("Show B", "tvg.b", 0);
  CHECK(FindSeriesRuleIndexByClientIndex(rules, clientIndex) == -1);
}

TEST_CASE("FindSeriesRuleIndexByClientIndex returns -1 for an empty list", "[TimerIdentity]")
{
  std::vector<FakeRule> rules;
  CHECK(FindSeriesRuleIndexByClientIndex(rules, ComputeSeriesRuleClientIndex("Show A", "tvg.a", 0)) == -1);
}

TEST_CASE("FindSeriesRuleIndexByClientIndex is the exact reverse of ComputeSeriesRuleClientIndex "
          "-- the real bug this fixes",
          "[TimerIdentity]")
{
  // Simulates a channel's tvgId drifting after the rule was created: the
  // rule was created (and cached by GetTimers()) under "tvg.original",
  // but re-deriving from the channel's *current* state now yields
  // "tvg.drifted" -- looking the rule up by its own ClientIndex must
  // still recover the original identity, not the drifted one.
  std::vector<FakeRule> rules = {{"My Show", "tvg.original"}};
  unsigned int clientIndex = ComputeSeriesRuleClientIndex("My Show", "tvg.original", 0);
  int idx = FindSeriesRuleIndexByClientIndex(rules, clientIndex);
  REQUIRE(idx == 0);
  CHECK(rules[static_cast<std::size_t>(idx)].tvgId == "tvg.original");
}

TEST_CASE("FindSeriesRuleIndexByClientIndex returns the first match on a hash collision", "[TimerIdentity]")
{
  // Not a realistic scenario (two different rules hashing identically),
  // but the function's own contract is "first match wins", matching
  // std::find_if-style semantics.
  std::vector<FakeRule> rules = {{"Show A", "tvg.a"}, {"Show A", "tvg.a"}};
  unsigned int clientIndex = ComputeSeriesRuleClientIndex("Show A", "tvg.a", 0);
  CHECK(FindSeriesRuleIndexByClientIndex(rules, clientIndex) == 0);
}

TEST_CASE("FindSeriesRuleIndexByClientIndex distinguishes two rules sharing title+tvgId but pinned to "
          "different EPG sources -- the real bug this fixes",
          "[TimerIdentity]")
{
  // Two genuinely distinct rules server-side (same title+tvgId, but
  // Dispatcharr's own upsert also matches by epg_source_id) must resolve
  // to their own distinct index, not collide onto whichever one happens
  // to be first in the list.
  std::vector<FakeRule> rules = {{"Same Show", "same.tvg", 1}, {"Same Show", "same.tvg", 2}};
  unsigned int clientIndexForSourceTwo = ComputeSeriesRuleClientIndex("Same Show", "same.tvg", 2);
  int idx = FindSeriesRuleIndexByClientIndex(rules, clientIndexForSourceTwo);
  REQUIRE(idx == 1);
  CHECK(rules[static_cast<std::size_t>(idx)].epgSourceId == 2);
}

// ---------------------------------------------------------------------
// IsSameSeriesRuleChannel
// ---------------------------------------------------------------------

TEST_CASE("IsSameSeriesRuleChannel is true for two matching real channel ids", "[TimerIdentity]")
{
  CHECK(IsSameSeriesRuleChannel(42, 42));
}

TEST_CASE("IsSameSeriesRuleChannel is false for two different real channel ids -- a genuine channel change",
          "[TimerIdentity]")
{
  CHECK_FALSE(IsSameSeriesRuleChannel(42, 7));
}

TEST_CASE("IsSameSeriesRuleChannel treats a cached 0 and a timer's PVR_CHANNEL_INVALID_UID (-1) as the same "
          "channel -- the real bug this fixes",
          "[TimerIdentity]")
{
  // A channel-less rule's cached channelId is 0 (TimerRuleParser's own
  // FieldOr default); PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL's own
  // clientChannelUid sentinel is -1. A plain == read this as "channel
  // changed" (0 != -1), missing the cache and silently resolving tvgId
  // to "" -- appending a brand-new, match-everything rule instead of
  // editing the original.
  CHECK(IsSameSeriesRuleChannel(0, -1));
}

TEST_CASE("IsSameSeriesRuleChannel is false when a channel-less rule gains a pinned channel on edit", "[TimerIdentity]")
{
  // A genuine channel change still needs a fresh tvgId derived from the
  // newly-pinned channel -- "no channel" on one side and a real channel
  // on the other must never read as unchanged.
  CHECK_FALSE(IsSameSeriesRuleChannel(0, 42));
  CHECK_FALSE(IsSameSeriesRuleChannel(42, -1));
}

// ---------------------------------------------------------------------
// ShouldReplaceSeriesRuleOnEdit
// ---------------------------------------------------------------------

TEST_CASE("ShouldReplaceSeriesRuleOnEdit is false when the identity is unchanged -- the upsert edits in place",
          "[TimerIdentity]")
{
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("Evening News", "tvg.news", "Evening News", "tvg.news"));
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("", "", "", ""));
}

TEST_CASE("ShouldReplaceSeriesRuleOnEdit is true for a new search pattern or a channel that changes tvg_id",
          "[TimerIdentity]")
{
  CHECK(ShouldReplaceSeriesRuleOnEdit("Evening News", "tvg.news", "Late News", "tvg.news"));
  CHECK(ShouldReplaceSeriesRuleOnEdit("Evening News", "tvg.news", "Evening News", "tvg.other"));
  CHECK(ShouldReplaceSeriesRuleOnEdit("Evening News", "", "Evening News", "tvg.news"));
}

TEST_CASE("ShouldReplaceSeriesRuleOnEdit compares exactly, as Dispatcharr's own upsert does", "[TimerIdentity]")
{
  // A case-only change is a different rule server-side (title match is exact and
  // case-sensitive), so it is replaced rather than left as a duplicate.
  CHECK(ShouldReplaceSeriesRuleOnEdit("Evening News", "tvg.news", "evening news", "tvg.news"));
}

TEST_CASE("ShouldReplaceSeriesRuleOnEdit with sources: a pinned rule's changed source is a new identity",
          "[TimerIdentity]")
{
  // The same title and tvg_id under a different source is another rule: the old one must go.
  CHECK(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.news", 7));
  CHECK(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.news", 0));
  CHECK(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.news", -1));
  // Unchanged identity, source echoed: edited in place.
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.news", 5));
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 0, "News", "tvg.news", 0));
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", -3, "News", "tvg.news", 0));
  // An unsourced old rule is never replaced for a source alone: an unscoped delete would also
  // remove the new rule.
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 0, "News", "tvg.news", 5));
  // Title or tvg_id still decide, whatever the sources.
  CHECK(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "Late News", "tvg.news", 5));
  CHECK(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.other", 5));
}

TEST_CASE("ComputeSeriesRuleClientIndex always has bit 30 set and bit 31 clear", "[TimerIdentity]")
{
  // Bit 30 flags a series rule and bit 31 must never be set (the hash fills the low 30 bits).
  for (const char* title : {"", "a", "Evening News", "ZZZ", "A very long title that hashes somewhere else entirely"})
  {
    for (int source : {0, 1, 5, -1, 123456})
    {
      const unsigned int index = ComputeSeriesRuleClientIndex(title, "tvg.x", source);
      CHECK((index & 0x40000000u) != 0);
      CHECK((index & 0x80000000u) == 0);
    }
  }
}

TEST_CASE("ResolveSeriesRuleSourceOnEdit keeps the pin only for the tvg_id it was pinned with", "[TimerIdentity]")
{
  // Same tvg_id (an HD/SD variant of the channel): the pin is echoed and the rule is edited in place.
  CHECK(ResolveSeriesRuleSourceOnEdit("tvg.news", "tvg.news", 5) == 5);
  // A different tvg_id: the old source need not carry it, so the new rule is sent unpinned...
  CHECK(ResolveSeriesRuleSourceOnEdit("tvg.news", "tvg.other", 5) == 0);
  CHECK(ResolveSeriesRuleSourceOnEdit("tvg.news", "", 5) == 0);
  CHECK(ResolveSeriesRuleSourceOnEdit("", "tvg.news", 5) == 0);
  // ...and an unpinned rule stays unpinned whatever happens.
  CHECK(ResolveSeriesRuleSourceOnEdit("tvg.news", "tvg.news", 0) == 0);
  CHECK(ResolveSeriesRuleSourceOnEdit("tvg.news", "tvg.other", -3) == 0);
  // And the replace decision for that case still deletes the old pinned rule by its own source.
  CHECK(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.other",
                                      ResolveSeriesRuleSourceOnEdit("tvg.news", "tvg.other", 5)));
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("News", "tvg.news", 5, "News", "tvg.news",
                                            ResolveSeriesRuleSourceOnEdit("tvg.news", "tvg.news", 5)));
}

TEST_CASE("HasPinnedSiblingSeriesRule finds a pinned rule sharing title and tvg_id, and nothing else",
          "[TimerIdentity]")
{
  struct Rule
  {
    std::string title;
    std::string tvgId;
    int epgSourceId;
  };
  const std::vector<Rule> rules = {
      {"News", "tvg.news", 0}, {"News", "tvg.news", 7}, {"Other", "tvg.news", 9}, {"News", "tvg.other", 5}};
  CHECK(HasPinnedSiblingSeriesRule(rules, "News", "tvg.news"));
  // Only an unpinned rule exists for this pair: no pinned sibling.
  CHECK_FALSE(HasPinnedSiblingSeriesRule(std::vector<Rule>{{"News", "tvg.news", 0}}, "News", "tvg.news"));
  CHECK_FALSE(HasPinnedSiblingSeriesRule(std::vector<Rule>{{"News", "tvg.news", -1}}, "News", "tvg.news"));
  // A different title or tvg_id is not a sibling, pinned or not.
  CHECK_FALSE(HasPinnedSiblingSeriesRule(std::vector<Rule>{{"Other", "tvg.news", 9}}, "News", "tvg.news"));
  CHECK_FALSE(HasPinnedSiblingSeriesRule(std::vector<Rule>{{"News", "tvg.other", 5}}, "News", "tvg.news"));
  CHECK_FALSE(HasPinnedSiblingSeriesRule(std::vector<Rule>{}, "News", "tvg.news"));
}

TEST_CASE("IsSameSeriesRuleChannel treats every non-positive id on both sides as the same 'no channel'",
          "[TimerIdentity]")
{
  CHECK(IsSameSeriesRuleChannel(0, 0));
  CHECK(IsSameSeriesRuleChannel(-1, 0));
  CHECK_FALSE(IsSameSeriesRuleChannel(0, 5));
  CHECK_FALSE(IsSameSeriesRuleChannel(5, -1));
}

TEST_CASE("ShouldReplaceSeriesRuleOnEdit does not replace for an unsourced old rule or a non-positive source",
          "[TimerIdentity]")
{
  // An unsourced old rule is left to the title/tvg_id test: deleting it without a source would match the
  // new rule as well. Any non-positive source is "unpinned".
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("T", "v", 0, "T", "v", 5));
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("T", "v", -2, "T", "v", 5));
  CHECK(ShouldReplaceSeriesRuleOnEdit("T", "v", 5, "T", "v", 0));
  CHECK(ShouldReplaceSeriesRuleOnEdit("T", "v", 5, "T", "v", 6));
  CHECK_FALSE(ShouldReplaceSeriesRuleOnEdit("T", "v", 5, "T", "v", 5));
}
