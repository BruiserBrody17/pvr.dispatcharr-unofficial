#pragma once

#include <ctime>
#include <string>

namespace dispatcharr
{

// Auto-computes the current UTC offset (minutes) for the small set of
// well-known IANA zones (US/Canada, UK/EU) hardcoded in
// TimeZoneUtil.cpp, using their real, stable DST transition rules -- see
// docs/RECURRING_RULES.md for why a full timezone database isn't bundled
// to do this for every possible zone instead. Returns false
// (offsetMinutesOut untouched) for any zone not in that short list, in
// which case recurring_rule_utc_offset_minutes still needs to be set
// manually. Pure computation, no network/instance state, no Kodi SDK
// dependency -- kept self-contained specifically so it's unit-testable
// standalone (see ../tests/test_timezone_util.cpp).
// `nowUtc` is a parameter purely for testability; real callers should
// always pass the actual current time.
bool ComputeKnownZoneOffsetMinutes(const std::string& ianaZoneName, time_t nowUtc, int& offsetMinutesOut);

} // namespace dispatcharr
