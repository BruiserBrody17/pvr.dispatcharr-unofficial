#include "ManagedRecurringRule.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace dispatcharr
{

namespace
{
const std::string kTag = kManagedRuleMarker;
const std::string kSpacedTag = " " + std::string(kManagedRuleMarker);
} // namespace

bool IsManagedRuleName(const std::string& serverName)
{
  if (serverName == kTag)
    return true;
  return serverName.size() > kSpacedTag.size() &&
         serverName.compare(serverName.size() - kSpacedTag.size(), kSpacedTag.size(), kSpacedTag) == 0;
}

std::string AddManagedRuleMarkerBounded(const std::string& displayName)
{
  if (IsManagedRuleName(displayName))
    return displayName;
  const std::size_t room = kMaxRuleNameCharacters - kSpacedTag.size();
  std::size_t characters = 0;
  std::size_t end = 0;
  while (end < displayName.size() && characters < room)
  {
    // Advance one code point: a lead byte plus its continuation bytes (10xxxxxx).
    ++end;
    while (end < displayName.size() && (static_cast<unsigned char>(displayName[end]) & 0xC0) == 0x80)
      ++end;
    ++characters;
  }
  return AddManagedRuleMarker(displayName.substr(0, end));
}

std::string StripManagedRuleMarker(const std::string& serverName)
{
  if (serverName == kTag)
    return "";
  if (IsManagedRuleName(serverName))
    return serverName.substr(0, serverName.size() - kSpacedTag.size());
  return serverName;
}

std::string AddManagedRuleMarker(const std::string& displayName)
{
  if (IsManagedRuleName(displayName))
    return displayName;
  if (displayName.empty())
    return kTag;
  return displayName + kSpacedTag;
}

bool HasActiveOrImminentOccurrence(const RecurringRule& rule, const std::vector<Recording>& recordings,
                                   bool haveRecordings, time_t now, int safetyMarginSeconds)
{
  if (!haveRecordings)
    return true;
  for (const auto& rec : recordings)
  {
    if (rec.recurringRuleId != rule.id)
      continue;
    if (rec.isInProgress || (rec.isUpcoming && rec.endTime > now && rec.startTime - now < safetyMarginSeconds))
      return true;
  }
  return false;
}

std::string SerializeAdoptionState(const RecurringRuleAdoptionState& state)
{
  return nlohmann::json({{"version", 1}, {"done", state.done}, {"pending", state.pending}}).dump();
}

bool ParseAdoptionState(const std::string& text, RecurringRuleAdoptionState& out)
{
  out = RecurringRuleAdoptionState();
  nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object() || FieldOr(doc, "version", 0) != 1 || !doc.contains("done") || !doc["done"].is_boolean())
    return false;
  out.known = true;
  out.done = doc["done"].get<bool>();
  if (doc.contains("pending") && doc["pending"].is_array())
  {
    for (const auto& id : doc["pending"])
    {
      if (id.is_number_integer() && id.get<int>() > 0)
        out.pending.push_back(id.get<int>());
    }
  }
  if (out.done)
    out.pending.clear();
  return true;
}

RecurringRuleAdoptionState EvaluateInitialAdoption(const std::vector<RecurringRule>& rules, time_t now)
{
  RecurringRuleAdoptionState state;
  state.known = true;
  const bool anyTagged =
      std::any_of(rules.begin(), rules.end(), [](const RecurringRule& r) { return IsManagedRuleName(r.name); });
  if (rules.empty() || anyTagged)
  {
    state.done = true;
    return state;
  }
  for (const auto& rule : rules)
  {
    if (rule.endDate > 0 && rule.endDate <= now)
      continue; // already ran its course
    state.pending.push_back(rule.id);
  }
  state.done = state.pending.empty();
  return state;
}

RecurringRuleAdoptionPlan PlanRecurringRuleAdoption(const std::vector<int>& pending,
                                                    const std::vector<RecurringRule>& rules,
                                                    const std::vector<Recording>& recordings, bool haveRecordings,
                                                    time_t now, int safetyMarginSeconds)
{
  RecurringRuleAdoptionPlan plan;
  for (int id : pending)
  {
    auto it = std::find_if(rules.begin(), rules.end(), [id](const RecurringRule& r) { return r.id == id; });
    if (it == rules.end() || IsManagedRuleName(it->name))
      continue; // gone, or tagged since
    if (HasActiveOrImminentOccurrence(*it, recordings, haveRecordings, now, safetyMarginSeconds))
      plan.deferred.push_back(id);
    else
      plan.toTag.push_back(id);
  }
  return plan;
}

AdoptionFailureKind ClassifyAdoptionPatchFailure(long httpStatus)
{
  // 401 is the credentials, not the rule: Request() reports the first attempt's 401 even when the
  // re-login it triggered then failed (login backoff, a 429 from the login limiter, a 5xx), so one
  // authentication hiccup during the one-time pass would otherwise skip every pending rule for good
  // (found by the 2026-10-04 third hardening sweep).
  if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 401 && httpStatus != 408 && httpStatus != 429)
    return AdoptionFailureKind::kPermanent;
  return AdoptionFailureKind::kRetryLater;
}

} // namespace dispatcharr
