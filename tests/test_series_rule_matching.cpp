#include "SeriesRuleMatching.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
TimerRule MakeRule(int channelId, const std::string& title)
{
  TimerRule rule;
  rule.channelId = channelId;
  rule.title = title;
  return rule;
}

Recording MakeRecording(int channelId, const std::string& title, time_t startTime, bool isUpcoming = true,
                        bool isInProgress = false, int recurringRuleId = 0)
{
  Recording rec;
  rec.channelId = channelId;
  rec.title = title;
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
