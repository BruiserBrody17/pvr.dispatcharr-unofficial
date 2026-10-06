#include "ServerOffsetCrossCheck.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace dispatcharr
{

namespace
{
long FloorDiv(long a, long b)
{
  long q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}
} // namespace

int ImpliedOffsetMinutes(int ruleStartTimeOfDaySeconds, time_t occurrenceStartUtc, int nearOffsetMinutes)
{
  constexpr long kDay = 86400;
  const long utcTimeOfDay = static_cast<long>(((occurrenceStartUtc % kDay) + kDay) % kDay);
  const long near = static_cast<long>(nearOffsetMinutes) * 60;
  // The difference between the offset the two times imply and the one expected, folded into (-12 h, +12 h].
  long delta = (static_cast<long>(ruleStartTimeOfDaySeconds) - utcTimeOfDay - near) % kDay;
  if (delta > kDay / 2)
    delta -= kDay;
  else if (delta <= -kDay / 2)
    delta += kDay;
  return static_cast<int>(FloorDiv(near + delta + 30, 60));
}

ServerOffsetVerdict CrossCheckServerOffset(const std::function<bool(time_t, int&)>& tableOffsetMinutesAt,
                                           const std::vector<OffsetSample>& samples)
{
  ServerOffsetVerdict verdict;
  bool agrees = false;
  std::vector<int> agreeingOffsets;
  bool haveDisagreement = false;
  bool disagreementConsistent = true;
  int disagreement = 0;
  int disagreementTableOffset = 0;
  for (const OffsetSample& sample : samples)
  {
    int tableOffset = 0, before = 0, after = 0;
    if (!tableOffsetMinutesAt(sample.occurrenceStartUtc, tableOffset) ||
        !tableOffsetMinutesAt(sample.occurrenceStartUtc - 86400, before) ||
        !tableOffsetMinutesAt(sample.occurrenceStartUtc + 86400, after))
      continue;
    if (before != tableOffset || after != tableOffset)
      continue; // too close to a clock change in the table to read anything into
    const int implied = ImpliedOffsetMinutes(sample.ruleStartTimeOfDaySeconds, sample.occurrenceStartUtc, tableOffset);
    if (implied == tableOffset)
    {
      agrees = true;
      if (std::find(agreeingOffsets.begin(), agreeingOffsets.end(), tableOffset) == agreeingOffsets.end())
        agreeingOffsets.push_back(tableOffset);
      continue;
    }
    if (std::abs(implied - tableOffset) > kMaxPlausibleOffsetDeltaMinutes)
      continue; // not a clock rule the table could be missing: most likely an edited rule beside its old occurrences
    if (!haveDisagreement)
    {
      haveDisagreement = true;
      disagreement = implied;
      disagreementTableOffset = tableOffset;
    }
    else if (disagreement != implied || disagreementTableOffset != tableOffset)
      disagreementConsistent = false;
  }
  if (agrees)
  {
    verdict.kind = ServerOffsetVerdict::Kind::kAgrees;
    verdict.agreeingTableOffsets = std::move(agreeingOffsets);
  }
  else if (haveDisagreement && disagreementConsistent)
  {
    verdict.kind = ServerOffsetVerdict::Kind::kDisagrees;
    verdict.impliedOffsetMinutes = disagreement;
    verdict.tableOffsetMinutes = disagreementTableOffset;
  }
  return verdict;
}

std::vector<OffsetSample> SelectOffsetSamples(const std::vector<OccurrenceRef>& occurrences,
                                              const std::map<int, int>& ruleStartTimeOfDaySeconds, time_t now,
                                              size_t maxSamples)
{
  std::vector<OffsetSample> samples;
  for (const OccurrenceRef& occurrence : occurrences)
  {
    if (occurrence.recurringRuleId <= 0 || occurrence.startTime <= 0 || occurrence.startTime < now)
      continue;
    const auto rule = ruleStartTimeOfDaySeconds.find(occurrence.recurringRuleId);
    if (rule != ruleStartTimeOfDaySeconds.end())
      samples.push_back({rule->second, occurrence.startTime});
  }
  const size_t keep = std::min(maxSamples, samples.size());
  std::partial_sort(samples.begin(), samples.begin() + keep, samples.end(),
                    [](const OffsetSample& a, const OffsetSample& b)
                    { return a.occurrenceStartUtc < b.occurrenceStartUtc; });
  samples.resize(keep);
  return samples;
}

int ApplyServerOffsetOverride(const ServerOffsetOverride& ov, int tableOffsetMinutes, int legacyOffsetMinutes)
{
  switch (ov.mode)
  {
  case ServerOffsetOverride::Mode::kLegacyZoneData:
    return legacyOffsetMinutes;
  case ServerOffsetOverride::Mode::kConstantDelta:
    return tableOffsetMinutes == ov.tableOffsetMinutes ? tableOffsetMinutes + ov.deltaMinutes : tableOffsetMinutes;
  case ServerOffsetOverride::Mode::kNone:
    break;
  }
  return tableOffsetMinutes;
}

ServerOffsetJudgement JudgeServerOffset(const std::function<bool(time_t, int&)>& tableOffsetMinutesAt,
                                        const std::function<bool(time_t, int&)>& legacyOffsetMinutesAt,
                                        const std::vector<OffsetSample>& samples)
{
  ServerOffsetJudgement judgement;
  const ServerOffsetVerdict verdict = CrossCheckServerOffset(tableOffsetMinutesAt, samples);
  const ServerOffsetVerdict legacy = CrossCheckServerOffset(legacyOffsetMinutesAt, samples);
  judgement.kind = verdict.kind;
  judgement.legacyKind = legacy.kind;
  judgement.agreeingTableOffsets = verdict.agreeingTableOffsets;
  if (verdict.kind != ServerOffsetVerdict::Kind::kDisagrees)
    return judgement;
  if (legacy.kind == ServerOffsetVerdict::Kind::kAgrees)
  {
    judgement.proposed.mode = ServerOffsetOverride::Mode::kLegacyZoneData;
    return judgement;
  }
  if (legacy.kind == ServerOffsetVerdict::Kind::kNoEvidence)
  {
    judgement.kind = ServerOffsetVerdict::Kind::kNoEvidence;
    return judgement;
  }
  judgement.proposed.mode = ServerOffsetOverride::Mode::kConstantDelta;
  judgement.proposed.deltaMinutes = verdict.impliedOffsetMinutes - verdict.tableOffsetMinutes;
  judgement.proposed.tableOffsetMinutes = verdict.tableOffsetMinutes;
  return judgement;
}

ServerOffsetChange UpdateServerOffsetTracker(ServerOffsetTracker& tracker, const std::string& zone,
                                             const ServerOffsetJudgement& judgement)
{
  using Kind = ServerOffsetVerdict::Kind;
  auto clear = [&tracker]()
  {
    tracker.active = {};
    tracker.pending = {};
    tracker.pendingCount = 0;
  };
  bool clearedByZoneChange = false;
  if (zone == "manual" || zone != tracker.zone)
  {
    clearedByZoneChange = tracker.active.mode != ServerOffsetOverride::Mode::kNone;
    clear();
    tracker.zone = zone;
    // A manual offset is the user's own number: nothing to learn. A new zone's first judgement still counts below.
    if (zone == "manual")
      return clearedByZoneChange ? ServerOffsetChange::kCleared : ServerOffsetChange::kNone;
  }
  const auto result = [&]() -> ServerOffsetChange
  {
    if (judgement.kind == Kind::kAgrees)
    {
      tracker.pending = {};
      tracker.pendingCount = 0;
      using Mode = ServerOffsetOverride::Mode;
      bool contradicted = false;
      switch (tracker.active.mode)
      {
      case Mode::kNone:
        break;
      case Mode::kLegacyZoneData:
        // A server on the zone's older rules and the current table give the same answer wherever the two agree (all
        // summer, for a zone that stopped changing its clocks): only a legacy table that disagrees with the samples
        // says the server has moved on.
        contradicted = judgement.legacyKind == Kind::kDisagrees;
        break;
      case Mode::kConstantDelta:
        // Measured against one table offset: agreement elsewhere says nothing about it.
        contradicted = std::find(judgement.agreeingTableOffsets.begin(), judgement.agreeingTableOffsets.end(),
                                 tracker.active.tableOffsetMinutes) != judgement.agreeingTableOffsets.end();
        break;
      }
      if (!contradicted)
        return ServerOffsetChange::kNone;
      clear();
      return ServerOffsetChange::kCleared;
    }
    if (judgement.kind != Kind::kDisagrees)
    {
      tracker.pending = {};
      tracker.pendingCount = 0;
      return ServerOffsetChange::kNone;
    }
    const bool wasActive = tracker.active.mode != ServerOffsetOverride::Mode::kNone;
    if (judgement.proposed == tracker.active)
    {
      tracker.pending = {};
      tracker.pendingCount = 0;
      return ServerOffsetChange::kNone;
    }
    if (judgement.proposed == tracker.pending)
      ++tracker.pendingCount;
    else
    {
      tracker.pending = judgement.proposed;
      tracker.pendingCount = 1;
    }
    if (tracker.pendingCount < kServerOffsetConfirmations)
      return ServerOffsetChange::kNone;
    tracker.active = tracker.pending;
    tracker.pending = {};
    tracker.pendingCount = 0;
    return wasActive ? ServerOffsetChange::kChanged : ServerOffsetChange::kActivated;
  }();
  return clearedByZoneChange && result == ServerOffsetChange::kNone ? ServerOffsetChange::kCleared : result;
}

std::string FormatUtcOffsetMinutes(int offsetMinutes)
{
  const int magnitude = std::abs(offsetMinutes);
  std::string minutes = std::to_string(magnitude % 60);
  if (minutes.size() < 2)
    minutes.insert(0, "0");
  return std::string("UTC") + (offsetMinutes < 0 ? "-" : "+") + std::to_string(magnitude / 60) + ":" + minutes;
}

} // namespace dispatcharr
