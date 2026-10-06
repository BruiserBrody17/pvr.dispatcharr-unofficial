#include "ServerOffsetCrossCheck.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr time_t kDay = 86400;
// 2026-05-12 00:30 UTC, and 2026-12-15 03:30 UTC (a winter instant for a northern zone).
constexpr time_t kMidYear = 1778545800;
constexpr time_t kDecember = 1797305400;

constexpr int hours(int h)
{
  return h * 3600;
}
constexpr int hm(int h, int m)
{
  return h * 3600 + m * 60;
}

// A table that always answers `minutes`, and one that changes at `changeAt`.
std::function<bool(time_t, int&)> Always(int minutes)
{
  return [minutes](time_t, int& out)
  {
    out = minutes;
    return true;
  };
}
std::function<bool(time_t, int&)> Changing(time_t changeAt, int before, int after)
{
  return [=](time_t at, int& out)
  {
    out = at >= changeAt ? after : before;
    return true;
  };
}
} // namespace

TEST_CASE("ImpliedOffsetMinutes reads the offset off a local time and its UTC instant", "[ServerOffsetCrossCheck]")
{
  // 19:30 local for an instant at 00:30 UTC: UTC-5, whatever the table expected.
  CHECK(ImpliedOffsetMinutes(hm(19, 30), kMidYear, -300) == -300);
  CHECK(ImpliedOffsetMinutes(hm(19, 30), kMidYear, -420) == -300);
  CHECK(ImpliedOffsetMinutes(hm(19, 30), kMidYear, 0) == -300);
  // 20:30 local for 03:30 UTC (December): UTC-7.
  CHECK(ImpliedOffsetMinutes(hm(20, 30), kDecember, -420) == -420);
  // No offset, east of UTC, and half- and quarter-hour zones.
  CHECK(ImpliedOffsetMinutes(hm(0, 30), kMidYear, 0) == 0);
  CHECK(ImpliedOffsetMinutes(hm(1, 30), kMidYear, 0) == 60);
  CHECK(ImpliedOffsetMinutes(hm(6, 0), kMidYear, 330) == 330);
  CHECK(ImpliedOffsetMinutes(hm(6, 15), kMidYear, 345) == 345);
}

TEST_CASE("ImpliedOffsetMinutes uses the table to tell a zone from the one a day away", "[ServerOffsetCrossCheck]")
{
  // 13:30 local for 00:30 UTC is +13 h or -11 h: the same time of day. The expected offset picks the neighbour.
  CHECK(ImpliedOffsetMinutes(hm(13, 30), kMidYear, 780) == 780);
  CHECK(ImpliedOffsetMinutes(hm(13, 30), kMidYear, 720) == 780);   // NZ standard time expected, daylight time applied
  CHECK(ImpliedOffsetMinutes(hm(13, 30), kMidYear, -660) == -660); // American Samoa
  CHECK(ImpliedOffsetMinutes(hm(13, 30), kMidYear, -600) == -660);
  // The extremes of real zones, +14 and -12, each reached from a table value an hour off.
  CHECK(ImpliedOffsetMinutes(hm(14, 30), kMidYear, 780) == 840);
}

TEST_CASE("ImpliedOffsetMinutes puts a tie exactly half a day away on the later offset", "[ServerOffsetCrossCheck]")
{
  // 12 h from the table either way is not a real situation, but the answer must be one fixed choice, not a coin toss:
  // the fold is into (-12 h, +12 h], so +12 h stays and -12 h becomes +12 h.
  CHECK(ImpliedOffsetMinutes(hm(12, 30), kMidYear, 0) == 720);   // 12 h later than the table's 0
  CHECK(ImpliedOffsetMinutes(hm(0, 30), kMidYear, 720) == 1440); // 12 h earlier than the table's +12 h: a day later
  CHECK(ImpliedOffsetMinutes(hm(12, 29), kMidYear, 0) == 719);   // one minute inside the boundary
  CHECK(ImpliedOffsetMinutes(hm(12, 31), kMidYear, 0) == -719);  // one minute past it wraps to the other side
}

TEST_CASE("ImpliedOffsetMinutes recovers any real offset from a table an hour or two away", "[ServerOffsetCrossCheck]")
{
  for (int offset = -720; offset <= 840; offset += 15)
  {
    for (time_t base : {kMidYear, kDecember, static_cast<time_t>(0), static_cast<time_t>(-86400 * 40 - 1234 * 60)})
    {
      // The rule's local time of day is the instant's UTC time of day plus the offset, wrapped.
      const long tod = (((base % 86400) + 86400) % 86400 + static_cast<long>(offset) * 60 + 86400 * 3) % 86400;
      for (int off : {-120, -60, 0, 60, 120})
        CHECK(ImpliedOffsetMinutes(static_cast<int>(tod), base, offset + off) == offset);
    }
  }
}

TEST_CASE("CrossCheckServerOffset agrees when the table is right", "[ServerOffsetCrossCheck]")
{
  const auto verdict = CrossCheckServerOffset(Always(-300), {{hm(19, 30), kMidYear}});
  CHECK(verdict.kind == ServerOffsetVerdict::Kind::kAgrees);
}

TEST_CASE("CrossCheckServerOffset reports a table the server contradicts", "[ServerOffsetCrossCheck]")
{
  // The table says UTC-6; the server scheduled 19:30 at 00:30 UTC, which is UTC-5.
  const auto verdict = CrossCheckServerOffset(Always(-360), {{hm(19, 30), kMidYear}});
  CHECK(verdict.kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(verdict.impliedOffsetMinutes == -300);
  CHECK(verdict.tableOffsetMinutes == -360);
}

TEST_CASE("CrossCheckServerOffset needs every usable sample to name the same other offset", "[ServerOffsetCrossCheck]")
{
  // Two samples that both say -300, against a table that says -360.
  auto verdict = CrossCheckServerOffset(Always(-360), {{hm(19, 30), kMidYear}, {hm(19, 30), kMidYear + kDay}});
  CHECK(verdict.kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(verdict.impliedOffsetMinutes == -300);

  // Two disagreeing samples that contradict each other (-360 and -480 against -420): no evidence.
  verdict = CrossCheckServerOffset(Always(-420), {{hm(18, 30), kMidYear}, {hm(16, 30), kMidYear}});
  CHECK(verdict.kind == ServerOffsetVerdict::Kind::kNoEvidence);

  // One agreeing sample outweighs a disagreeing one: the table is right at least where it was looked at.
  verdict = CrossCheckServerOffset(Always(-300), {{hm(19, 30), kMidYear}, {hm(18, 30), kMidYear}});
  CHECK(verdict.kind == ServerOffsetVerdict::Kind::kAgrees);
}

TEST_CASE("CrossCheckServerOffset has no opinion without samples or with an unknown zone", "[ServerOffsetCrossCheck]")
{
  CHECK(CrossCheckServerOffset(Always(-300), {}).kind == ServerOffsetVerdict::Kind::kNoEvidence);
  const auto unknown = [](time_t, int&) { return false; };
  CHECK(CrossCheckServerOffset(unknown, {{hm(19, 30), kMidYear}}).kind == ServerOffsetVerdict::Kind::kNoEvidence);
}

TEST_CASE("CrossCheckServerOffset ignores a sample within a day of a clock change in the table",
          "[ServerOffsetCrossCheck]")
{
  // Spring-forward at kMidYear + 12 h in this made-up table. The server resolving a nonexistent or repeated local time
  // reads as the other offset without anything being wrong, so the samples on and next to that day say nothing.
  const time_t change = kMidYear + 12 * 3600;
  const auto table = Changing(change, -360, -300);
  for (time_t at : {change - kDay + 1, change - 3600, change, change + 3600, change + kDay - 1})
  {
    const auto verdict = CrossCheckServerOffset(table, {{hm(20, 30), at}}); // an offset the table does not have
    CHECK(verdict.kind == ServerOffsetVerdict::Kind::kNoEvidence);
  }
  // Two days either side, the same sample counts.
  // (12:30 UTC at -240 is 08:30; at -420 it is 05:30.)
  auto after = CrossCheckServerOffset(table, {{hm(8, 30), change + 2 * kDay}});
  CHECK(after.kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(after.impliedOffsetMinutes == -240);
  CHECK(after.tableOffsetMinutes == -300);
  auto before = CrossCheckServerOffset(table, {{hm(5, 30), change - 2 * kDay}});
  CHECK(before.kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(before.impliedOffsetMinutes == -420);
  CHECK(before.tableOffsetMinutes == -360);
}

TEST_CASE("ImpliedOffsetMinutes rounds a seconds difference to the nearest minute, either side of UTC",
          "[ServerOffsetCrossCheck]")
{
  // The occurrence carries 28 seconds the rule does not. Truncating toward zero gave 59 for +60 and -299 for -300.
  constexpr time_t base = kMidYear - 30 * 60; // 00:00 UTC
  CHECK(ImpliedOffsetMinutes(hm(1, 0), base + 28, 60) == 60);
  CHECK(ImpliedOffsetMinutes(hm(1, 0), base - 28, 60) == 60);
  CHECK(ImpliedOffsetMinutes(hm(19, 0), base + 28, -300) == -300);
  CHECK(ImpliedOffsetMinutes(hm(19, 0), base - 28, -300) == -300);
  // 31 seconds is past the half minute, so it is a minute off, in the direction it points.
  CHECK(ImpliedOffsetMinutes(hm(1, 0), base + 31, 60) == 59);
  CHECK(ImpliedOffsetMinutes(hm(19, 0), base + 31, -300) == -301);
}

TEST_CASE("CrossCheckServerOffset takes no sample that implies more than an hour from the table",
          "[ServerOffsetCrossCheck]")
{
  // A rule edited from 19:30 to 21:00 beside occurrences still at their old time reads as 90 minutes off the table.
  const auto edited = CrossCheckServerOffset(Always(-300), {{hm(21, 0), kMidYear}});
  CHECK(edited.kind == ServerOffsetVerdict::Kind::kNoEvidence);
  // An hour is the largest clock difference there is to find, in either direction.
  CHECK(CrossCheckServerOffset(Always(-300), {{hm(20, 30), kMidYear}}).kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(CrossCheckServerOffset(Always(-300), {{hm(18, 30), kMidYear}}).kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(CrossCheckServerOffset(Always(-300), {{hm(20, 31), kMidYear}}).kind == ServerOffsetVerdict::Kind::kNoEvidence);
  CHECK(CrossCheckServerOffset(Always(-300), {{hm(18, 29), kMidYear}}).kind == ServerOffsetVerdict::Kind::kNoEvidence);
  // An implausible sample does not count as agreement, nor does it spoil the plausible ones.
  const auto mixed = CrossCheckServerOffset(Always(-300), {{hm(21, 0), kMidYear}, {hm(20, 30), kMidYear + kDay}});
  CHECK(mixed.kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(mixed.impliedOffsetMinutes == -240);
}

TEST_CASE("CrossCheckServerOffset needs the disagreeing samples to share the table offset too",
          "[ServerOffsetCrossCheck]")
{
  // The same implied offset (-390, a half-hour zone) against two different table answers is two different
  // differences (+30 and -30 minutes), not one.
  const time_t change = kMidYear + 20 * kDay;
  const auto table = Changing(change, -420, -360);
  const OffsetSample before{hm(18, 0), kMidYear};
  const OffsetSample after{hm(18, 0), kMidYear + 25 * kDay};
  CHECK(CrossCheckServerOffset(table, {before, after}).kind == ServerOffsetVerdict::Kind::kNoEvidence);
  // Two samples on the same side of the change do share it.
  const OffsetSample before2{hm(18, 0), kMidYear + kDay};
  const auto same = CrossCheckServerOffset(table, {before, before2});
  CHECK(same.kind == ServerOffsetVerdict::Kind::kDisagrees);
  CHECK(same.impliedOffsetMinutes == -390);
  CHECK(same.tableOffsetMinutes == -420);
}

TEST_CASE("SelectOffsetSamples takes only upcoming occurrences of known rules, the soonest first",
          "[ServerOffsetCrossCheck]")
{
  const time_t now = kMidYear;
  const std::map<int, int> rules = {{1, hm(19, 30)}, {2, hm(8, 0)}};
  const std::vector<OccurrenceRef> occurrences = {
      {1, now - kDay},     // already started: the server may have been on other clock rules then
      {1, now + 3 * kDay}, // kept
      {1, now},            // starting right now counts
      {2, now + kDay},     // kept
      {3, now + kDay},     // a rule this check was not given
      {0, now + kDay},     // not an occurrence of any rule
      {-1, now + kDay},    //
      {1, 0},              // no start time
      {2, now + 2 * kDay}, // kept
  };
  const auto samples = SelectOffsetSamples(occurrences, rules, now, 10);
  REQUIRE(samples.size() == 4);
  CHECK(samples[0].occurrenceStartUtc == now);
  CHECK(samples[0].ruleStartTimeOfDaySeconds == hm(19, 30));
  CHECK(samples[1].occurrenceStartUtc == now + kDay);
  CHECK(samples[1].ruleStartTimeOfDaySeconds == hm(8, 0));
  CHECK(samples[2].occurrenceStartUtc == now + 2 * kDay);
  CHECK(samples[3].occurrenceStartUtc == now + 3 * kDay);
}

TEST_CASE("SelectOffsetSamples keeps the soonest few", "[ServerOffsetCrossCheck]")
{
  std::vector<OccurrenceRef> occurrences;
  for (int i = 20; i >= 1; --i)
    occurrences.push_back({1, kMidYear + i * kDay});
  const auto samples = SelectOffsetSamples(occurrences, {{1, hm(19, 30)}}, kMidYear, 6);
  REQUIRE(samples.size() == 6);
  for (int i = 0; i < 6; ++i)
    CHECK(samples[i].occurrenceStartUtc == kMidYear + (i + 1) * kDay);
  CHECK(SelectOffsetSamples(occurrences, {{1, hm(19, 30)}}, kMidYear, 0).empty());
  CHECK(SelectOffsetSamples({}, {{1, hm(19, 30)}}, kMidYear, 6).empty());
  CHECK(SelectOffsetSamples(occurrences, {}, kMidYear, 6).empty());
}

TEST_CASE("ApplyServerOffsetOverride resolves each mode", "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  CHECK(ApplyServerOffsetOverride({}, -420, -480) == -420);
  CHECK(ApplyServerOffsetOverride({Mode::kLegacyZoneData, 0, 0}, -420, -480) == -480);
  const ServerOffsetOverride delta{Mode::kConstantDelta, -60, -420};
  CHECK(ApplyServerOffsetOverride(delta, -420, -999) == -480);
  // The difference was measured against -420 only: where the table says something else, it is trusted.
  CHECK(ApplyServerOffsetOverride(delta, -360, -999) == -360);
  CHECK(ApplyServerOffsetOverride({Mode::kConstantDelta, 60, 0}, 0, -999) == 60);
}

namespace
{
// A made-up zone whose table stays on -420 from `permanentFrom`, where the legacy table goes back to -480 for the
// winter.
constexpr time_t kPermanentFrom = kMidYear + 25 * kDay; // 2026-10-31T00:30Z
std::function<bool(time_t, int&)> PermanentTable()
{
  return [](time_t at, int& out)
  {
    out = -420;
    (void)at;
    return true;
  };
}
// Legacy: -420 until `kPermanentFrom + 40 days`, -480 for the 120 days after, -420 again.
std::function<bool(time_t, int&)> LegacyTable()
{
  return [](time_t at, int& out)
  {
    out = (at >= kPermanentFrom + 5 * kDay && at < kPermanentFrom + 125 * kDay) ? -480 : -420;
    return true;
  };
}
} // namespace

TEST_CASE("JudgeServerOffset tells a zone whose rules changed from an unexplained difference",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  // The winter occurrences are at -480 (a 18:30 rule at 02:30 UTC); the current table says -420, the legacy one -480.
  const time_t winter = kPermanentFrom + 30 * kDay + 2 * 3600; // 02:30 UTC
  auto judged = JudgeServerOffset(PermanentTable(), LegacyTable(), {{hm(18, 30), winter}});
  CHECK(judged.kind == Kind::kDisagrees);
  CHECK(judged.proposed.mode == Mode::kLegacyZoneData);

  // The same samples against a legacy table that does not explain them: a plain difference of an hour.
  judged = JudgeServerOffset(PermanentTable(), PermanentTable(), {{hm(18, 30), winter}});
  CHECK(judged.kind == Kind::kDisagrees);
  CHECK(judged.proposed.mode == Mode::kConstantDelta);
  CHECK(judged.proposed.deltaMinutes == -60);
  CHECK(judged.proposed.tableOffsetMinutes == -420);

  // Agreement and no evidence carry no proposal.
  judged = JudgeServerOffset(PermanentTable(), LegacyTable(), {{hm(19, 30), winter}});
  CHECK(judged.kind == Kind::kAgrees);
  CHECK(judged.proposed.mode == Mode::kNone);
  CHECK(JudgeServerOffset(PermanentTable(), LegacyTable(), {}).kind == Kind::kNoEvidence);
}

TEST_CASE("UpdateServerOffsetTracker adopts an override only once two evaluations in a row propose it",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement legacy;
  legacy.kind = Kind::kDisagrees;
  legacy.proposed.mode = Mode::kLegacyZoneData;
  ServerOffsetJudgement agrees;
  agrees.kind = Kind::kAgrees;
  ServerOffsetJudgement none;

  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", legacy) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kNone);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", legacy) == ServerOffsetChange::kActivated);
  CHECK(tracker.active.mode == Mode::kLegacyZoneData);
  // Once in force, the same proposal is no change.
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", legacy) == ServerOffsetChange::kNone);
  // No evidence leaves it in force.
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", none) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kLegacyZoneData);
  // An agreeing table clears it when the older rules no longer fit the samples.
  agrees.legacyKind = Kind::kDisagrees;
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", agrees) == ServerOffsetChange::kCleared);
  CHECK(tracker.active.mode == Mode::kNone);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", agrees) == ServerOffsetChange::kNone);
}

TEST_CASE("UpdateServerOffsetTracker keeps a legacy-zone override through the months the table agrees anyway",
          "[ServerOffsetCrossCheck]")
{
  // A zone that stopped changing its clocks, a server on its older rules: from March to November both give the same
  // offset, so the table agrees with the samples all summer. The override must survive that, or every winter-dated rule
  // is an hour off until the next winter (and two warnings a year).
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement legacy;
  legacy.kind = Kind::kDisagrees;
  legacy.proposed.mode = Mode::kLegacyZoneData;
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  REQUIRE(tracker.active.mode == Mode::kLegacyZoneData);

  ServerOffsetJudgement summer;
  summer.kind = Kind::kAgrees;
  summer.legacyKind = Kind::kAgrees;
  for (int i = 0; i < 5; ++i)
    CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", summer) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kLegacyZoneData);
  summer.legacyKind = Kind::kNoEvidence; // samples on a clock-change day of the legacy table: nothing against it either
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", summer) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kLegacyZoneData);
  // The server updated its tz data: the table agrees and the older rules do not.
  summer.legacyKind = Kind::kDisagrees;
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", summer) == ServerOffsetChange::kCleared);
  CHECK(tracker.active.mode == Mode::kNone);
}

TEST_CASE("UpdateServerOffsetTracker clears a constant difference only when samples at its own table offset agree",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement delta;
  delta.kind = Kind::kDisagrees;
  delta.proposed = {Mode::kConstantDelta, -60, -300};
  UpdateServerOffsetTracker(tracker, "Zone/A", delta);
  UpdateServerOffsetTracker(tracker, "Zone/A", delta);
  REQUIRE(tracker.active == delta.proposed);

  ServerOffsetJudgement elsewhere;
  elsewhere.kind = Kind::kAgrees;
  elsewhere.agreeingTableOffsets = {-360};
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", elsewhere) == ServerOffsetChange::kNone);
  CHECK(tracker.active == delta.proposed);
  ServerOffsetJudgement here = elsewhere;
  here.agreeingTableOffsets = {-360, -300};
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", here) == ServerOffsetChange::kCleared);
  CHECK(tracker.active.mode == Mode::kNone);
}

TEST_CASE("UpdateServerOffsetTracker: an evaluation that proposes the override in force restarts the confirmation of "
          "another",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement a, b;
  a.kind = b.kind = Kind::kDisagrees;
  a.proposed = {Mode::kConstantDelta, 60, -300};
  b.proposed = {Mode::kConstantDelta, -60, -300};
  UpdateServerOffsetTracker(tracker, "Zone/A", a);
  UpdateServerOffsetTracker(tracker, "Zone/A", a);
  REQUIRE(tracker.active == a.proposed);
  // B, A (the override in force: B's streak is broken), B: B has been seen once in a row, not twice.
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", b) == ServerOffsetChange::kNone);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", a) == ServerOffsetChange::kNone);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", b) == ServerOffsetChange::kNone);
  CHECK(tracker.active == a.proposed);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", b) == ServerOffsetChange::kChanged);
  CHECK(tracker.active == b.proposed);
}

TEST_CASE("UpdateServerOffsetTracker counts a new zone's first judgement, and says the old override went",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement legacy;
  legacy.kind = Kind::kDisagrees;
  legacy.proposed.mode = Mode::kLegacyZoneData;
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  REQUIRE(tracker.active.mode == Mode::kLegacyZoneData);
  // The setting changed and the new zone's samples already propose an override: that is its first sighting.
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/B", legacy) == ServerOffsetChange::kCleared);
  CHECK(tracker.active.mode == Mode::kNone);
  CHECK(tracker.pendingCount == 1);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/B", legacy) == ServerOffsetChange::kActivated);
}

TEST_CASE("JudgeServerOffset has no opinion on a disagreement the legacy table cannot judge",
          "[ServerOffsetCrossCheck]")
{
  using Kind = ServerOffsetVerdict::Kind;
  // The samples all sit within a day of the legacy table's own clock change (so it skips them), while the current table
  // is at a steady offset there and disagrees: no evidence, not a constant difference.
  const time_t change = kPermanentFrom + 40 * kDay;
  auto legacyChanging = [change](time_t at, int& out)
  {
    out = at >= change ? -480 : -420;
    return true;
  };
  const auto judged = JudgeServerOffset(PermanentTable(), legacyChanging, {{hm(18, 30), change + 2 * 3600}});
  CHECK(judged.kind == Kind::kNoEvidence);
  CHECK(judged.proposed.mode == ServerOffsetOverride::Mode::kNone);
}

TEST_CASE("JudgeServerOffset reports what the legacy table and the agreeing samples said", "[ServerOffsetCrossCheck]")
{
  using Kind = ServerOffsetVerdict::Kind;
  // Summer: current and legacy tables give the same offset, and an agreeing sample names it.
  const auto summer = JudgeServerOffset(Always(-420), Always(-420), {{hm(17, 30), kMidYear}});
  CHECK(summer.kind == Kind::kAgrees);
  CHECK(summer.legacyKind == Kind::kAgrees);
  CHECK(summer.agreeingTableOffsets == std::vector<int>{-420});
  // A server that has moved on to the current rules: the table agrees, the legacy one does not.
  const auto updated = JudgeServerOffset(Always(-420), Always(-480), {{hm(17, 30), kMidYear}});
  CHECK(updated.kind == Kind::kAgrees);
  CHECK(updated.legacyKind == Kind::kDisagrees);
}

TEST_CASE("ImpliedOffsetMinutes rounds exactly half a minute up", "[ServerOffsetCrossCheck]")
{
  constexpr time_t base = kMidYear - 30 * 60;                 // 00:00 UTC
  CHECK(ImpliedOffsetMinutes(hm(1, 0), base + 30, 60) == 60); // 59.5 minutes: up to 60
  CHECK(ImpliedOffsetMinutes(hm(1, 0), base - 30, 60) == 61);
  CHECK(ImpliedOffsetMinutes(hm(19, 0), base + 30, -300) == -300);
}

TEST_CASE("UpdateServerOffsetTracker does not adopt a proposal seen once between other judgements",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement one;
  one.kind = Kind::kDisagrees;
  one.proposed = {Mode::kConstantDelta, 60, -300};
  ServerOffsetJudgement other;
  other.kind = Kind::kDisagrees;
  other.proposed = {Mode::kConstantDelta, -60, -300};
  ServerOffsetJudgement none;
  ServerOffsetJudgement agrees;
  agrees.kind = Kind::kAgrees;

  // Alternating proposals never confirm.
  for (int i = 0; i < 4; ++i)
    CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", i % 2 ? other : one) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kNone);
  // A proposal, an evaluation with no evidence, then the proposal again: the streak is broken.
  tracker = {};
  UpdateServerOffsetTracker(tracker, "Zone/A", one);
  UpdateServerOffsetTracker(tracker, "Zone/A", none);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", one) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kNone);
  // And one that the table then agrees with.
  tracker = {};
  UpdateServerOffsetTracker(tracker, "Zone/A", one);
  UpdateServerOffsetTracker(tracker, "Zone/A", agrees);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", one) == ServerOffsetChange::kNone);
}

TEST_CASE("UpdateServerOffsetTracker replaces an override with a different confirmed one", "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement a;
  a.kind = Kind::kDisagrees;
  a.proposed = {Mode::kConstantDelta, 60, -300};
  ServerOffsetJudgement b = a;
  b.proposed.deltaMinutes = -60;
  UpdateServerOffsetTracker(tracker, "Zone/A", a);
  UpdateServerOffsetTracker(tracker, "Zone/A", a);
  REQUIRE(tracker.active == a.proposed);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", b) == ServerOffsetChange::kNone);
  CHECK(tracker.active == a.proposed); // one sighting of the other does not replace it
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/A", b) == ServerOffsetChange::kChanged);
  CHECK(tracker.active == b.proposed);
}

TEST_CASE("UpdateServerOffsetTracker forgets everything when the zone setting changes or goes manual",
          "[ServerOffsetCrossCheck]")
{
  using Mode = ServerOffsetOverride::Mode;
  using Kind = ServerOffsetVerdict::Kind;
  ServerOffsetTracker tracker;
  ServerOffsetJudgement legacy;
  legacy.kind = Kind::kDisagrees;
  legacy.proposed.mode = Mode::kLegacyZoneData;
  ServerOffsetJudgement none;
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  REQUIRE(tracker.active.mode == Mode::kLegacyZoneData);

  // A different zone, and the next verdict has no evidence: the old zone's override must not carry over.
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/B", none) == ServerOffsetChange::kCleared);
  CHECK(tracker.active.mode == Mode::kNone);
  CHECK(tracker.zone == "Zone/B");

  UpdateServerOffsetTracker(tracker, "Zone/B", legacy);
  UpdateServerOffsetTracker(tracker, "Zone/B", legacy);
  REQUIRE(tracker.active.mode == Mode::kLegacyZoneData);
  CHECK(UpdateServerOffsetTracker(tracker, "manual", none) == ServerOffsetChange::kCleared);
  CHECK(tracker.active.mode == Mode::kNone);
  CHECK(UpdateServerOffsetTracker(tracker, "manual", legacy) == ServerOffsetChange::kNone);
  CHECK(tracker.active.mode == Mode::kNone);

  // A half-confirmed proposal for the old zone is not carried to the new one either.
  tracker = {};
  UpdateServerOffsetTracker(tracker, "Zone/A", legacy);
  UpdateServerOffsetTracker(tracker, "Zone/B", legacy);
  CHECK(tracker.active.mode == Mode::kNone);
  CHECK(UpdateServerOffsetTracker(tracker, "Zone/B", legacy) == ServerOffsetChange::kActivated);
}

TEST_CASE("FormatUtcOffsetMinutes keeps the sign of the whole offset", "[ServerOffsetCrossCheck]")
{
  CHECK(FormatUtcOffsetMinutes(0) == "UTC+0:00");
  CHECK(FormatUtcOffsetMinutes(60) == "UTC+1:00");
  CHECK(FormatUtcOffsetMinutes(-300) == "UTC-5:00");
  CHECK(FormatUtcOffsetMinutes(-30) == "UTC-0:30");
  CHECK(FormatUtcOffsetMinutes(30) == "UTC+0:30");
  CHECK(FormatUtcOffsetMinutes(345) == "UTC+5:45");
  CHECK(FormatUtcOffsetMinutes(-570) == "UTC-9:30");
  CHECK(FormatUtcOffsetMinutes(840) == "UTC+14:00");
}
