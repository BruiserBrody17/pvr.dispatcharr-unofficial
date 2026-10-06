#include "ManagedRecurringRule.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr time_t kNow = 1767225600;

RecurringRule Rule(int id, const std::string& name, time_t endDate = kNow + 20 * 86400)
{
  RecurringRule r;
  r.id = id;
  r.name = name;
  r.endDate = endDate;
  return r;
}
} // namespace

TEST_CASE("the tag is recognised at the end of a name, or as the whole name", "[ManagedRecurringRule]")
{
  CHECK(IsManagedRuleName("Evening News at 10pm [Kodi]"));
  CHECK(IsManagedRuleName("[Kodi]"));
  CHECK_FALSE(IsManagedRuleName("Evening News at 10pm"));
  CHECK_FALSE(IsManagedRuleName(""));
  // Only a tag at the end counts, and only as its own word.
  CHECK_FALSE(IsManagedRuleName("[Kodi] Evening News"));
  CHECK_FALSE(IsManagedRuleName("Evening News[Kodi]"));
  CHECK_FALSE(IsManagedRuleName("Show [kodi]"));
  CHECK_FALSE(IsManagedRuleName("Show [Kodi] "));
}

TEST_CASE("the tag is added and stripped without disturbing the name", "[ManagedRecurringRule]")
{
  CHECK(AddManagedRuleMarker("Evening News") == "Evening News [Kodi]");
  CHECK(StripManagedRuleMarker("Evening News [Kodi]") == "Evening News");
  CHECK(StripManagedRuleMarker("Evening News") == "Evening News");
  CHECK(StripManagedRuleMarker("[Kodi] Evening News") == "[Kodi] Evening News");
}

TEST_CASE("adding the tag is idempotent and an empty name becomes the bare tag", "[ManagedRecurringRule]")
{
  CHECK(AddManagedRuleMarker("Evening News [Kodi]") == "Evening News [Kodi]");
  CHECK(AddManagedRuleMarker("") == "[Kodi]");
  CHECK(AddManagedRuleMarker("[Kodi]") == "[Kodi]");
  CHECK(StripManagedRuleMarker("[Kodi]").empty());
}

TEST_CASE("add then strip is the identity, strip then add keeps ownership", "[ManagedRecurringRule]")
{
  for (const std::string name : {"Evening News", "A", "Show with [brackets]", "[Kodi] first", "Café 12", ""})
  {
    CHECK(StripManagedRuleMarker(AddManagedRuleMarker(name)) == name);
    CHECK(IsManagedRuleName(AddManagedRuleMarker(name)));
  }
}

TEST_CASE("HasActiveOrImminentOccurrence sees a recording, a start inside the margin, and an unreadable list",
          "[ManagedRecurringRule]")
{
  RecurringRule rule = Rule(5, "x [Kodi]");
  Recording running;
  running.recurringRuleId = 5;
  running.isInProgress = true;
  CHECK(HasActiveOrImminentOccurrence(rule, {running}, true, kNow, 3600));

  Recording soon;
  soon.recurringRuleId = 5;
  soon.isUpcoming = true;
  soon.startTime = kNow + 600;
  soon.endTime = kNow + 1800;
  CHECK(HasActiveOrImminentOccurrence(rule, {soon}, true, kNow, 3600));
  soon.startTime = kNow + 7200;
  soon.endTime = kNow + 8000;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {soon}, true, kNow, 3600));

  CHECK(HasActiveOrImminentOccurrence(rule, {}, /*haveRecordings=*/false, kNow, 3600));
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {}, true, kNow, 3600));
}

TEST_CASE("HasActiveOrImminentOccurrence ignores another rule's occurrences and a missed one", "[ManagedRecurringRule]")
{
  RecurringRule rule = Rule(5, "x [Kodi]");
  Recording other;
  other.recurringRuleId = 6;
  other.isInProgress = true;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {other}, true, kNow, 3600));

  Recording missed; // stuck at "scheduled" with its whole window already past
  missed.recurringRuleId = 5;
  missed.isUpcoming = true;
  missed.startTime = kNow - 7200;
  missed.endTime = kNow - 3600;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {missed}, true, kNow, 3600));
}

TEST_CASE("the first look at an account with no rules is done at once, so a later web-UI rule is never swept up",
          "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionState s = EvaluateInitialAdoption({}, kNow);
  CHECK(s.known);
  CHECK(s.done);
  CHECK(s.pending.empty());
}

TEST_CASE("the first look finds every untagged rule that has not run its course pending", "[ManagedRecurringRule]")
{
  std::vector<RecurringRule> rules = {Rule(1, "A"), Rule(2, "B", /*end*/ 0), Rule(3, "C", kNow - 86400),
                                      Rule(4, "D", kNow + 3 * 86400)};
  RecurringRuleAdoptionState s = EvaluateInitialAdoption(rules, kNow);
  CHECK_FALSE(s.done);
  // 3 ran its course (expired) and is left alone; 2 has no end date and is taken.
  CHECK(s.pending == std::vector<int>{1, 2, 4});
}

TEST_CASE("a tagged rule already there means someone got there first: done, nothing pending", "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionState s = EvaluateInitialAdoption({Rule(1, "A"), Rule(2, "B [Kodi]")}, kNow);
  CHECK(s.done);
  CHECK(s.pending.empty());
}

TEST_CASE("only expired rules means nothing to do", "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionState s = EvaluateInitialAdoption({Rule(1, "A", kNow - 86400)}, kNow);
  CHECK(s.done);
}

TEST_CASE("adoption state survives a serialize/parse round trip", "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionState in;
  in.known = true;
  in.pending = {3, 9};
  RecurringRuleAdoptionState out;
  REQUIRE(ParseAdoptionState(SerializeAdoptionState(in), out));
  CHECK(out.known);
  CHECK_FALSE(out.done);
  CHECK(out.pending == std::vector<int>{3, 9});

  in.done = true;
  REQUIRE(ParseAdoptionState(SerializeAdoptionState(in), out));
  CHECK(out.done);
  CHECK(out.pending.empty());
}

TEST_CASE("an unreadable adoption state is unknown, so it is simply evaluated again", "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionState out;
  out.known = true;
  for (const char* text :
       {"", "garbage", "{\"version\":2,\"done\":true}", "{\"version\":1}", "[]", "{\"version\":1,\"done\":\"yes\"}"})
  {
    CHECK_FALSE(ParseAdoptionState(text, out));
    CHECK_FALSE(out.known);
  }
}

TEST_CASE("pending ids that are not positive integers are dropped on read", "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionState out;
  REQUIRE(ParseAdoptionState("{\"version\":1,\"done\":false,\"pending\":[4,-1,0,\"x\",7]}", out));
  CHECK(out.pending == std::vector<int>{4, 7});
}

// ---------------------------------------------------------------------
// PlanRecurringRuleAdoption
// ---------------------------------------------------------------------

TEST_CASE("an adoption cycle tags the pending rules it can and defers one with a recording running or imminent",
          "[ManagedRecurringRule]")
{
  std::vector<RecurringRule> rules = {Rule(1, "A"), Rule(2, "B"), Rule(3, "C")};
  Recording running;
  running.recurringRuleId = 2;
  running.isInProgress = true;
  Recording soon;
  soon.recurringRuleId = 3;
  soon.isUpcoming = true;
  soon.startTime = kNow + 600;
  soon.endTime = kNow + 1800;

  RecurringRuleAdoptionPlan plan = PlanRecurringRuleAdoption({1, 2, 3}, rules, {running, soon}, true, kNow, 3600);
  CHECK(plan.toTag == std::vector<int>{1});
  CHECK(plan.deferred == std::vector<int>{2, 3});
}

TEST_CASE("an adoption cycle forgets a pending rule that is gone or was tagged since", "[ManagedRecurringRule]")
{
  std::vector<RecurringRule> rules = {Rule(1, "A [Kodi]"), Rule(3, "C")};
  RecurringRuleAdoptionPlan plan = PlanRecurringRuleAdoption({1, 2, 3}, rules, {}, true, kNow, 3600);
  CHECK(plan.toTag == std::vector<int>{3});
  CHECK(plan.deferred.empty());
}

TEST_CASE("an adoption cycle defers everything when the recordings could not be read", "[ManagedRecurringRule]")
{
  // Without the recording list the occurrence-safety check cannot be made, so
  // nothing is PATCHed blind; the rules stay pending for a later cycle.
  std::vector<RecurringRule> rules = {Rule(1, "A"), Rule(2, "B")};
  RecurringRuleAdoptionPlan plan = PlanRecurringRuleAdoption({1, 2}, rules, {}, /*haveRecordings=*/false, kNow, 3600);
  CHECK(plan.toTag.empty());
  CHECK(plan.deferred == std::vector<int>{1, 2});
}

TEST_CASE("an adoption cycle with nothing pending plans nothing", "[ManagedRecurringRule]")
{
  RecurringRuleAdoptionPlan plan = PlanRecurringRuleAdoption({}, {Rule(1, "A")}, {}, true, kNow, 3600);
  CHECK(plan.toTag.empty());
  CHECK(plan.deferred.empty());
}

// ---------------------------------------------------------------------
// ClassifyAdoptionPatchFailure
// ---------------------------------------------------------------------

TEST_CASE("A 4xx that will repeat is a permanent failure for that rule", "[ManagedRecurringRule]")
{
  // A name that fails the server's validation once the tag is appended, no permission to edit, a
  // rule deleted meanwhile: none of these get better by asking again next cycle.
  for (long status : {400L, 403L, 404L, 405L, 409L, 410L, 413L, 422L})
    CHECK(ClassifyAdoptionPatchFailure(status) == AdoptionFailureKind::kPermanent);
}

TEST_CASE("No response, a server error, a timeout or a rate limit is worth retrying", "[ManagedRecurringRule]")
{
  CHECK(ClassifyAdoptionPatchFailure(0) == AdoptionFailureKind::kRetryLater); // no response at all
  // 401 included: Request() reports the first attempt's 401 even when the re-login it triggered
  // then failed, so it says nothing about the rule itself.
  for (long status : {500L, 502L, 503L, 504L, 401L, 408L, 429L})
    CHECK(ClassifyAdoptionPatchFailure(status) == AdoptionFailureKind::kRetryLater);
}

TEST_CASE("A status that should not occur on a failure is retried rather than skipped for good",
          "[ManagedRecurringRule]")
{
  for (long status : {-1L, 200L, 204L, 301L, 399L, 600L})
    CHECK(ClassifyAdoptionPatchFailure(status) == AdoptionFailureKind::kRetryLater);
}

TEST_CASE("The per-session retry cap is a small bounded number", "[ManagedRecurringRule]")
{
  CHECK(kMaxAdoptionRetriesPerSession >= 2);
  CHECK(kMaxAdoptionRetriesPerSession <= 10);
}

TEST_CASE("a name that is only a space and the tag is not managed, but the bare tag is", "[ManagedRecurringRule]")
{
  CHECK_FALSE(IsManagedRuleName(" [Kodi]"));
  CHECK(IsManagedRuleName("x [Kodi]"));
  CHECK(IsManagedRuleName("[Kodi]"));
}

TEST_CASE("HasActiveOrImminentOccurrence treats a start exactly at the margin as not yet imminent",
          "[ManagedRecurringRule]")
{
  RecurringRule rule = Rule(5, "x [Kodi]");
  Recording rec;
  rec.recurringRuleId = 5;
  rec.isUpcoming = true;
  rec.endTime = kNow + 9000;
  rec.startTime = kNow + 3599;
  CHECK(HasActiveOrImminentOccurrence(rule, {rec}, true, kNow, 3600));
  rec.startTime = kNow + 3600;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {rec}, true, kNow, 3600));
}

TEST_CASE("EvaluateInitialAdoption skips a rule whose end date is exactly now, keeps one a second later",
          "[ManagedRecurringRule]")
{
  CHECK(EvaluateInitialAdoption({Rule(1, "a", kNow)}, kNow).pending.empty());
  CHECK(EvaluateInitialAdoption({Rule(1, "a", kNow + 1)}, kNow).pending == std::vector<int>{1});
  CHECK(EvaluateInitialAdoption({Rule(1, "a", 0)}, kNow).pending == std::vector<int>{1}); // open-ended
}

TEST_CASE("AddManagedRuleMarkerBounded leaves room for the tag within the server's 255-character limit",
          "[ManagedRecurringRule]")
{
  const std::string tag = " [Kodi]";
  // A name that fits with the tag is tagged unchanged.
  CHECK(AddManagedRuleMarkerBounded("Evening News") == "Evening News [Kodi]");
  CHECK(AddManagedRuleMarkerBounded("") == "[Kodi]");
  // 248 characters is the most that fits: kept whole; 249 is cut by one.
  const std::string fits(kMaxRuleNameCharacters - tag.size(), 'a');
  CHECK(AddManagedRuleMarkerBounded(fits) == fits + tag);
  CHECK(AddManagedRuleMarkerBounded(fits).size() == kMaxRuleNameCharacters);
  CHECK(AddManagedRuleMarkerBounded(fits + "a") == fits + tag);
  CHECK(AddManagedRuleMarkerBounded(std::string(400, 'z')).size() == kMaxRuleNameCharacters);
  // An already-tagged name is returned as it is, however long.
  CHECK(AddManagedRuleMarkerBounded("Show [Kodi]") == "Show [Kodi]");
}

TEST_CASE("AddManagedRuleMarkerBounded cuts on a code-point boundary and counts characters, not bytes",
          "[ManagedRecurringRule]")
{
  const std::string tag = " [Kodi]";
  const std::size_t room = kMaxRuleNameCharacters - tag.size();
  // 300 two-byte characters (U+00E9): only `room` of them survive, each whole.
  std::string accented;
  for (int i = 0; i < 300; ++i)
    accented += "\xC3\xA9";
  const std::string result = AddManagedRuleMarkerBounded(accented);
  CHECK(result.size() == room * 2 + tag.size());
  CHECK(result.substr(0, room * 2) == accented.substr(0, room * 2));
  // Three- and four-byte sequences are never split either.
  std::string wide;
  for (int i = 0; i < 300; ++i)
    wide += "\xF0\x9F\x93\xBA"; // U+1F4FA
  const std::string wideResult = AddManagedRuleMarkerBounded(wide);
  CHECK(wideResult.size() == room * 4 + tag.size());
  // Exactly `room` multi-byte characters fit untouched.
  std::string exact;
  for (std::size_t i = 0; i < room; ++i)
    exact += "\xC3\xA9";
  CHECK(AddManagedRuleMarkerBounded(exact) == exact + tag);
}

TEST_CASE("AddManagedRuleMarkerBounded leaves an already tagged name alone, however long", "[ManagedRecurringRule]")
{
  // The server accepted it, so it is returned as is rather than cut to fit a tag it already carries.
  const std::string longTagged = std::string(300, 'a') + " [Kodi]";
  CHECK(AddManagedRuleMarkerBounded(longTagged) == longTagged);
  CHECK(AddManagedRuleMarkerBounded("[Kodi]") == "[Kodi]");
}

TEST_CASE("HasActiveOrImminentOccurrence does not count an upcoming occurrence that ends exactly now",
          "[ManagedRecurringRule]")
{
  // A never-run occurrence whose whole window has passed stays "scheduled" forever on the server; it
  // must not read as imminent (and block renewal) once its end is not after now.
  RecurringRule rule = Rule(5, "x [Kodi]");
  Recording over;
  over.recurringRuleId = 5;
  over.isUpcoming = true;
  over.startTime = kNow - 1800;
  over.endTime = kNow;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {over}, true, kNow, 3600));
  over.endTime = kNow + 1;
  CHECK(HasActiveOrImminentOccurrence(rule, {over}, true, kNow, 3600));
}

TEST_CASE("HasActiveOrImminentOccurrence ignores an occurrence that is neither running nor upcoming",
          "[ManagedRecurringRule]")
{
  // One stopped early keeps its originally scheduled end and start time: still in the future by the clock, but the
  // server has finished with it (neither upcoming nor in progress), so it must not hold the rule back from renewal.
  RecurringRule rule = Rule(5, "x [Kodi]");
  Recording stopped;
  stopped.recurringRuleId = 5;
  stopped.isUpcoming = false;
  stopped.isInProgress = false;
  stopped.startTime = kNow + 600;
  stopped.endTime = kNow + 1800;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {stopped}, true, kNow, 3600));
  stopped.isUpcoming = true;
  CHECK(HasActiveOrImminentOccurrence(rule, {stopped}, true, kNow, 3600));
  // An occurrence of another rule never counts, upcoming or not.
  stopped.recurringRuleId = 6;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {stopped}, true, kNow, 3600));
  stopped.isInProgress = true;
  CHECK_FALSE(HasActiveOrImminentOccurrence(rule, {stopped}, true, kNow, 3600));
}
