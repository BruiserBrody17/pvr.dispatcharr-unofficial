#include "SeriesRuleMatching.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
TimerRule MakeRule(int channelId, const std::string& title, const std::string& titleMode = "exact",
                   const std::string& description = "", const std::string& descriptionMode = "contains")
{
  TimerRule rule;
  rule.channelId = channelId;
  rule.title = title;
  rule.titleMode = titleMode;
  rule.description = description;
  rule.descriptionMode = descriptionMode;
  return rule;
}

Recording MakeRecording(int channelId, const std::string& title, time_t startTime, bool isUpcoming = true,
                        bool isInProgress = false, int recurringRuleId = 0, const std::string& description = "")
{
  Recording rec;
  rec.channelId = channelId;
  rec.title = title;
  rec.description = description;
  rec.startTime = startTime;
  rec.isUpcoming = isUpcoming;
  rec.isInProgress = isInProgress;
  rec.recurringRuleId = recurringRuleId;
  return rec;
}
} // namespace

TEST_CASE("MatchRecordingsToSeriesRules matches a recording to its rule by channel and title", "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  REQUIRE(result.recordingRuleIndex.size() == 1);
  CHECK(result.recordingRuleIndex[0] == 0);
  REQUIRE(result.ruleEarliestRecordingIndex.size() == 1);
  CHECK(result.ruleEarliestRecordingIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules requires both channel and title to match", "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};

  std::vector<Recording> wrongChannel = {MakeRecording(2, "My Show", 1000)};
  CHECK(MatchRecordingsToSeriesRules(wrongChannel, rules).recordingRuleIndex[0] == -1);

  std::vector<Recording> wrongTitle = {MakeRecording(1, "A Different Show", 1000)};
  CHECK(MatchRecordingsToSeriesRules(wrongTitle, rules).recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules matches titles case-insensitively -- the real bug this fixes",
          "[SeriesRuleMatching]")
{
  // Matches Dispatcharr's own server-side "exact" title_mode evaluation
  // (confirmed against its real source: `title__iexact`) -- a rule whose
  // title differs only in case from the recording's own EPG title still
  // records correctly server-side, so this addon's own client-side
  // re-linking (display-time/parent-index only) must match the same way
  // or it silently reintroduces the Unix-epoch display bug this module
  // was written to fix.
  std::vector<TimerRule> rules = {MakeRule(1, "the news")};
  std::vector<Recording> recordings = {MakeRecording(1, "The News", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.ruleEarliestRecordingIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules tracks the earliest matching recording, not just any match",
          "[SeriesRuleMatching]")
{
  // The real reason this tracking exists: a series rule has no fixed
  // time of its own, so without an earliest-match it displayed as the
  // Unix epoch ("12/31/1969") in Kodi -- confirmed live, not cosmetic.
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {
      MakeRecording(1, "My Show", 3000),
      MakeRecording(1, "My Show", 1000), // earliest
      MakeRecording(1, "My Show", 2000),
  };

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.ruleEarliestRecordingIndex[0] == 1);
}

TEST_CASE("MatchRecordingsToSeriesRules skips a recording already linked to a recurring rule", "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {
      MakeRecording(1, "My Show", 1000, /*isUpcoming=*/true, /*isInProgress=*/false, /*recurringRuleId=*/5)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == -1);
  CHECK(result.ruleEarliestRecordingIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules skips a recording that's neither upcoming nor in-progress",
          "[SeriesRuleMatching]")
{
  // Completed recordings are surfaced via GetRecordings(), not as timers
  // -- never a match candidate here regardless of title/channel.
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000, /*isUpcoming=*/false, /*isInProgress=*/false)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules matches an in-progress recording too, not just upcoming ones",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000, /*isUpcoming=*/false, /*isInProgress=*/true)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules matches the first rule when more than one shares a channel",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "Show A"), MakeRule(1, "Show B")};
  std::vector<Recording> recordings = {MakeRecording(1, "Show B", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 1);
}

TEST_CASE("MatchRecordingsToSeriesRules leaves every rule unmatched when there are no recordings",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules({}, rules);

  REQUIRE(result.ruleEarliestRecordingIndex.size() == 1);
  CHECK(result.ruleEarliestRecordingIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules handles no rules at all", "[SeriesRuleMatching]")
{
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, {});

  REQUIRE(result.recordingRuleIndex.size() == 1);
  CHECK(result.recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules strips the rule's own title the way Dispatcharr's evaluation does",
          "[SeriesRuleMatching]")
{
  // Regression: evaluate_series_rules_impl() matches on the rule's title
  // .strip()'d, but the rule itself is stored unstripped -- a rule saved
  // as " My Show \t" recorded "My Show" server-side but never linked back
  // here.
  std::vector<TimerRule> rules = {MakeRule(1, " My Show \t"), MakeRule(2, "\x1cOther\r\n")};
  std::vector<Recording> recordings = {MakeRecording(1, "my show", 1000), MakeRecording(2, "Other", 2000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == 1);
  CHECK(result.ruleEarliestRecordingIndex[0] == 0);
  CHECK(result.ruleEarliestRecordingIndex[1] == 1);
}

TEST_CASE("MatchRecordingsToSeriesRules doesn't strip the recording's own title", "[SeriesRuleMatching]")
{
  // The server compares its stripped rule title against each programme's
  // own raw title, so a programme titled "My Show " never matched a
  // "My Show" rule server-side either -- linking it here would be wrong.
  std::vector<TimerRule> rules = {MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show ", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules keeps interior whitespace in a rule's title significant",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "  My  Show  ")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000), MakeRecording(1, "My  Show", 2000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == -1);
  CHECK(result.recordingRuleIndex[1] == 0);
}

// ---------------------------------------------------------------------
// Non-exact title modes, description filters, and Unicode -- found live
// 2026-09-30 (docs/OPEN_ITEMS.md): each of these was a rule Dispatcharr
// scheduled correctly that this addon never linked back, leaving the row
// in Kodi showing "Any day at any time" instead of its next recording.
// ---------------------------------------------------------------------

TEST_CASE("MatchRecordingsToSeriesRules links a contains-mode rule to a longer title -- the live-confirmed gap",
          "[SeriesRuleMatching]")
{
  // A "contains" rule "ZZZ_TEST Alpha" beside a future recording titled
  // "ZZZ_TEST Alpha Report" on the same channel: Kodi showed the rule as
  // "Any day at any time" despite the matching recording.
  std::vector<TimerRule> rules = {MakeRule(1, "ZZZ_TEST Alpha", "contains")};
  std::vector<Recording> recordings = {MakeRecording(1, "ZZZ_TEST Alpha Report", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.ruleEarliestRecordingIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules does not let an exact-mode rule match a longer title", "[SeriesRuleMatching]")
{
  // The control from the same live check: an exact rule for the full
  // title links, one for just the prefix does not.
  std::vector<TimerRule> rules = {MakeRule(1, "ZZZ_TEST Alpha")};
  std::vector<Recording> recordings = {MakeRecording(1, "ZZZ_TEST Alpha Report", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules evaluates a contains rule's AND/OR grammar the way the server does",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "news AND sport", "contains"),
                                  MakeRule(1, "cooking OR garden", "contains")};
  std::vector<Recording> recordings = {MakeRecording(1, "Sport and News Tonight", 1000),
                                       MakeRecording(1, "Evening News", 2000), MakeRecording(1, "Garden Rescue", 3000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == -1);
  CHECK(result.recordingRuleIndex[2] == 1);
}

TEST_CASE("MatchRecordingsToSeriesRules treats any mode name the server doesn't special-case as contains",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "Alpha", "whatever")};
  std::vector<Recording> recordings = {MakeRecording(1, "Alpha Report", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules reads an empty or differently-cased title_mode the way the server does",
          "[SeriesRuleMatching]")
{
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000), MakeRecording(1, "My Show Extra", 2000)};

  // An empty mode is exact, and so is any casing of "exact".
  for (const char* mode : {"", "EXACT", "Exact"})
  {
    std::vector<TimerRule> rules = {MakeRule(1, "my show", mode)};
    SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);
    CHECK(result.recordingRuleIndex[0] == 0);
    CHECK(result.recordingRuleIndex[1] == -1);
  }
}

TEST_CASE("MatchRecordingsToSeriesRules matches a search-mode rule on whole words only", "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "news", "search")};
  std::vector<Recording> recordings = {MakeRecording(1, "Evening News", 1000), MakeRecording(1, "Newsroom Live", 2000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules leaves a regex rule unlinked rather than guessing", "[SeriesRuleMatching]")
{
  // A PostgreSQL regular expression isn't ECMAScript; linking by a wrong
  // interpretation would be worse than the honest "Any day at any time".
  std::vector<TimerRule> rules = {MakeRule(1, "^My Show$", "regex")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == -1);
  CHECK(result.ruleEarliestRecordingIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules links a description-only rule by its description", "[SeriesRuleMatching]")
{
  // The server accepts a rule with an empty title as long as it has a
  // description, and matches every programme whose description qualifies.
  std::vector<TimerRule> rules = {MakeRule(1, "", "exact", "season finale")};
  std::vector<Recording> recordings = {
      MakeRecording(1, "Show A", 1000, true, false, 0, "The Season Finale airs tonight"),
      MakeRecording(1, "Show B", 2000, true, false, 0, "A regular episode"),
      MakeRecording(1, "Show C", 3000, true, false, 0, "")};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == -1);
  CHECK(result.recordingRuleIndex[2] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules requires both a rule's title and its description filter",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show", "exact", "finale")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000, true, false, 0, "The finale"),
                                       MakeRecording(1, "My Show", 2000, true, false, 0, "An ordinary episode"),
                                       MakeRecording(1, "Other Show", 3000, true, false, 0, "The finale")};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == -1);
  CHECK(result.recordingRuleIndex[2] == -1);
  CHECK(result.ruleEarliestRecordingIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules reads a description_mode of regex as unlinkable, even with a matching title",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "My Show", "exact", "fin.*", "regex")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000, true, false, 0, "finale")};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules never links a rule with neither a title nor a description",
          "[SeriesRuleMatching]")
{
  // The server rejects it ("invalid_rule") and never evaluates it.
  std::vector<TimerRule> rules = {MakeRule(1, "", "exact", ""), MakeRule(1, " \t", "contains", "  ")};
  std::vector<Recording> recordings = {MakeRecording(1, "Anything", 1000, true, false, 0, "whatever")};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == -1);
  CHECK(result.ruleEarliestRecordingIndex[0] == -1);
  CHECK(result.ruleEarliestRecordingIndex[1] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules strips a trailing no-break space from a rule's title -- live-confirmed",
          "[SeriesRuleMatching]")
{
  // Dispatcharr stores the rule's title unstripped, so the API returns the
  // no-break space, and its evaluation's Python .strip() removes it before
  // matching. The old ASCII-only strip left it in place, so the rule
  // recorded but stayed unlinked.
  std::vector<TimerRule> rules = {MakeRule(1, "ZZZ_TEST Alpha\xC2\xA0")};
  std::vector<Recording> recordings = {MakeRecording(1, "ZZZ_TEST Alpha", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules doesn't strip a no-break space from the recording's own title",
          "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "ZZZ_TEST Alpha")};
  std::vector<Recording> recordings = {MakeRecording(1, "ZZZ_TEST Alpha\xC2\xA0", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules folds non-ASCII letter case like the database does -- live-confirmed",
          "[SeriesRuleMatching]")
{
  // A rule titled with a capital U-umlaut against a recording titled with
  // the lowercase one: Kodi showed the rule as "Any day at any time" because
  // the old fold was ASCII-only, while the real instance's UPPER() folds both.
  std::vector<TimerRule> rules = {MakeRule(1, "ZZZ_TEST \xC3\x9CNDER TEST")};
  std::vector<Recording> recordings = {MakeRecording(1, "ZZZ_TEST \xC3\xBCnder test", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == 0);
}

TEST_CASE("MatchRecordingsToSeriesRules does not fold an accent away", "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeRule(1, "cafe")};
  std::vector<Recording> recordings = {MakeRecording(1, "caf\xC3\xA9", 1000)};

  CHECK(MatchRecordingsToSeriesRules(recordings, rules).recordingRuleIndex[0] == -1);
}

TEST_CASE("MatchRecordingsToSeriesRules keeps first-rule-wins across modes", "[SeriesRuleMatching]")
{
  // A contains rule listed before an exact one claims a recording both match.
  std::vector<TimerRule> rules = {MakeRule(1, "Show", "contains"), MakeRule(1, "My Show")};
  std::vector<Recording> recordings = {MakeRecording(1, "My Show", 1000)};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);

  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.ruleEarliestRecordingIndex[0] == 0);
  CHECK(result.ruleEarliestRecordingIndex[1] == -1);
}

namespace
{
TimerRule MakeChannelLessRule(const std::string& title, const std::string& tvgId)
{
  TimerRule rule = MakeRule(0, title);
  rule.tvgId = tvgId;
  return rule;
}

Recording MakeRecordingWithTvgId(int channelId, const std::string& title, time_t startTime, const std::string& tvgId)
{
  Recording rec = MakeRecording(channelId, title, startTime);
  rec.programTvgId = tvgId;
  return rec;
}
} // namespace

TEST_CASE("A series rule with no pinned channel links to recordings by the guide id of their programme",
          "[SeriesRuleMatching]")
{
  // Dispatcharr's own guide "Record series" button creates such a rule. Found by
  // the 2026-10-04 hardening sweep: it could never match (channelId 0 against a
  // recording's real channel), so it showed as "Any day at any time" and its
  // occurrences stayed unparented timers.
  std::vector<TimerRule> rules = {MakeChannelLessRule("My Show", "guide.id.1")};
  std::vector<Recording> recordings = {MakeRecordingWithTvgId(5, "My Show", 2000, "guide.id.1"),
                                       MakeRecordingWithTvgId(9, "My Show", 1000, "guide.id.1"),
                                       MakeRecordingWithTvgId(5, "My Show", 3000, "another.id")};

  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);
  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == 0);  // any channel
  CHECK(result.recordingRuleIndex[2] == -1); // a different guide id is a different rule
  CHECK(result.ruleEarliestRecordingIndex[0] == 1);
}

TEST_CASE("A channel-less series rule with no tvg_id matches the title on any channel", "[SeriesRuleMatching]")
{
  std::vector<TimerRule> rules = {MakeChannelLessRule("My Show", "")};
  std::vector<Recording> recordings = {MakeRecordingWithTvgId(5, "My Show", 1000, ""),
                                       MakeRecordingWithTvgId(6, "My Show", 2000, "whatever"),
                                       MakeRecordingWithTvgId(6, "Other", 3000, "whatever")};
  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);
  CHECK(result.recordingRuleIndex[0] == 0);
  CHECK(result.recordingRuleIndex[1] == 0);
  CHECK(result.recordingRuleIndex[2] == -1);
}

TEST_CASE("A rule pinned to the recording's channel wins over a channel-less rule that also matches",
          "[SeriesRuleMatching]")
{
  // The channel-less rule is listed FIRST, so a plain linear scan would pick it.
  std::vector<TimerRule> rules = {MakeChannelLessRule("My Show", ""), MakeRule(5, "My Show")};
  std::vector<Recording> recordings = {MakeRecordingWithTvgId(5, "My Show", 1000, "g"),
                                       MakeRecordingWithTvgId(8, "My Show", 2000, "g")};
  SeriesRuleMatchResult result = MatchRecordingsToSeriesRules(recordings, rules);
  CHECK(result.recordingRuleIndex[0] == 1); // pinned
  CHECK(result.recordingRuleIndex[1] == 0); // no pinned rule for channel 8: the channel-less one
}

TEST_CASE("A pinned rule never matches another channel's recording by guide id", "[SeriesRuleMatching]")
{
  TimerRule pinned = MakeRule(5, "My Show");
  pinned.tvgId = "guide.id.1";
  std::vector<Recording> recordings = {MakeRecordingWithTvgId(8, "My Show", 1000, "guide.id.1")};
  CHECK(MatchRecordingsToSeriesRules(recordings, {pinned}).recordingRuleIndex[0] == -1);
}
