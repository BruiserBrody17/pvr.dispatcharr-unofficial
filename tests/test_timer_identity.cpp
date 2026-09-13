#include "TimerIdentity.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ComputeSeriesRuleClientIndex is deterministic for the same input", "[TimerIdentity]")
{
  CHECK(ComputeSeriesRuleClientIndex("My Show", "abc.tvg") == ComputeSeriesRuleClientIndex("My Show", "abc.tvg"));
}

TEST_CASE("ComputeSeriesRuleClientIndex always sets the series-rule flag bit (0x40000000)", "[TimerIdentity]")
{
  CHECK((ComputeSeriesRuleClientIndex("My Show", "abc.tvg") & 0x40000000u) != 0);
  CHECK((ComputeSeriesRuleClientIndex("", "") & 0x40000000u) != 0);
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
    unsigned int index = ComputeSeriesRuleClientIndex(title, tvgId);
    CHECK((index & 0x80000000u) == 0);
  }
}

TEST_CASE("ComputeSeriesRuleClientIndex distinguishes different titles with the same tvgId", "[TimerIdentity]")
{
  CHECK(ComputeSeriesRuleClientIndex("Show A", "same.tvg") != ComputeSeriesRuleClientIndex("Show B", "same.tvg"));
}

TEST_CASE("ComputeSeriesRuleClientIndex distinguishes the same title with different tvgIds", "[TimerIdentity]")
{
  CHECK(ComputeSeriesRuleClientIndex("Same Show", "tvg.a") != ComputeSeriesRuleClientIndex("Same Show", "tvg.b"));
}

TEST_CASE("ComputeSeriesRuleClientIndex handles empty title/tvgId without crashing", "[TimerIdentity]")
{
  CHECK((ComputeSeriesRuleClientIndex("", "") & 0x40000000u) != 0);
}
