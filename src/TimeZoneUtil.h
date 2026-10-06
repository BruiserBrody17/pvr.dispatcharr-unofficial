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
// Also accepts the older names of those zones (Asia/Calcutta for Asia/Kolkata,
// US/Central for America/Chicago, ...) -- see CanonicalKnownZoneName().
// `applyZoneRuleChanges` false leaves out a zone's rule change that took effect recently (British Columbia and
// Alberta staying on daylight time from 2026-11-01): what a server whose tz data predates the change still applies
// (ServerOffsetCrossCheck.h).
bool ComputeKnownZoneOffsetMinutes(const std::string& ianaZoneName, time_t nowUtc, int& offsetMinutesOut,
                                   bool applyZoneRuleChanges = true);

// The name the table (and so settings.xml's recurring_rule_timezone dropdown)
// knows `ianaZoneName` by: the modern name for a known older alias, anything
// else unchanged. Dispatcharr's web UI stores whatever the browser's timezone
// list offers, which in Chromium is the older spelling for several zones.
std::string CanonicalKnownZoneName(const std::string& ianaZoneName);

// The recurring_rule_timezone setting value SyncTimezoneFromDispatcharr() selects
// for Dispatcharr's configured zone: the table's own (canonical) name when the
// zone -- or an alias of it -- is one this addon has DST rules for, otherwise
// "manual", the dropdown's fallback to the plain numeric offset. `knownOut`, when
// given, says which of the two it was. The alias half was a live-confirmed gap
// (docs/RECURRING_RULES.md): a zone Chromium spells the old way left the setting
// on "manual".
std::string DesiredRecurringRuleTimezoneSetting(const std::string& ianaZoneName, time_t nowUtc,
                                                bool* knownOut = nullptr);

} // namespace dispatcharr
