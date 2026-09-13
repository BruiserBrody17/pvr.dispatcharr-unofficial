#pragma once

#include "DispatcharrClient.h"

#include <nlohmann/json_fwd.hpp>

namespace dispatcharr
{

// Pure field-mapping core of DispatcharrClient::GetTimerRules()'s
// per-item loop -- maps a single /api/channels/series-rules/ item onto a
// TimerRule. Exact per-rule field names aren't confirmed (the account
// available while developing this addon lacked permission to create a
// series rule to inspect one) -- "title"/"channel_id" match the
// confirmed SeriesRuleRequest create payload, kept alongside the older
// assumed names as fallbacks in case the list response shape differs.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_timer_rule_parser.cpp.
TimerRule ParseTimerRuleJson(const nlohmann::json& item);

// Pure field-mapping core of DispatcharrClient::GetRecurringRules()'s
// per-item loop -- maps a single /api/channels/recurring-rules/ item onto
// a RecurringRule, including days_of_week's array-of-ints parsing (only
// integer entries are kept; anything else in the array is silently
// skipped rather than treated as an error).
RecurringRule ParseRecurringRuleJson(const nlohmann::json& item);

} // namespace dispatcharr
