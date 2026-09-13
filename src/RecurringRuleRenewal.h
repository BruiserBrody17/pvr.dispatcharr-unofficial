#pragma once

#include "DispatcharrClient.h"

#include <ctime>
#include <vector>

namespace dispatcharr
{

// Pure decision core of PVRDispatcharr::RenewRecurringRules()'s per-rule
// loop -- decides whether a recurring rule's end_date should be pushed
// forward this cycle. Returns false for:
//   - a disabled rule (nothing materializes for it anyway);
//   - one still comfortably inside its rolling window (more than half of
//     `windowDays` remaining, checked here rather than waiting until it's
//     about to actually run out, so most cycles do nothing at all -- see
//     PVRDispatcharr.h's own kRecurringRuleWindowDays comment);
//   - one with an occurrence currently recording or starting within
//     `safetyMarginSeconds` (defense in depth -- live testing confirmed
//     Dispatcharr's own regeneration on this kind of update already
//     leaves an in-progress/completed occurrence alone, see
//     ExtendRecurringRuleEndDate()'s own comment and
//     docs/RECURRING_RULES.md -- but this doesn't rely on that alone);
//   - one where `!haveRecordings` (GetRecordings() itself failed) --
//     erring toward skipping rather than renewing blind, since there's
//     no way to check the occurrence-safety condition above without it.
//
// Takes plain values (not a kodi::addon::PVRTimer&, and windowDays/
// safetyMarginSeconds as explicit parameters rather than
// PVRDispatcharr's own private static constants) specifically so this is
// unit-testable standalone with no Kodi SDK dependency; see
// ../tests/test_recurring_rule_renewal.cpp.
bool ShouldRenewRecurringRule(const RecurringRule& rule, const std::vector<Recording>& recordings, bool haveRecordings,
                              time_t now, int windowDays, int safetyMarginSeconds);

} // namespace dispatcharr
