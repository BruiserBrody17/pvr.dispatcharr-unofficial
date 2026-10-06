#pragma once

#include <ctime>
#include <string>

namespace dispatcharr
{

// "YYYY-MM-DDTHH:MM:SSZ" for a time_t, matching Dispatcharr's own
// UTC-normalized date-time fields.
std::string IsoFromTime(time_t t);

// Parses the "YYYY-MM-DDTHH:MM:SS" prefix of a Dispatcharr date-time
// field (e.g. "2026-01-01T00:00:00Z" or "...+00:00") and applies any
// trailing UTC offset found after it -- "Z", "+HH:MM"/"+HHMM", or
// "-HH:MM"/"-HHMM" (an optional fractional-seconds part before that is
// skipped, not applied). Every genuine Django-model-serialized field in
// this API is already UTC-normalized (confirmed against Dispatcharr's
// own source: TIME_ZONE="UTC", no timezone.activate() anywhere -- see
// docs/OPEN_ITEMS.md), so this is a no-op for those -- but
// custom_properties.program.start_time/end_time (RecordingParser.cpp)
// is a real, confirmed exception: a hand-built JSON string Dispatcharr's
// own recurring-rule scheduler writes via a timezone-*aware* Python
// datetime's own isoformat() in that rule's own configured system
// timezone, not Django's global one -- silently ignoring a genuine
// offset there (this function's own earlier behavior, an 18th-pass
// audit's "safe" conclusion having only checked the DRF-serialized
// case) returned a time off by the whole zone offset, found via a
// project-wide review (a 35th-pass audit, not itself independently
// reproduced). Returns 0 on anything unparseable.
time_t TimeFromIso(const std::string& isoStr);

// "HH:MM:SS" for a plain seconds-since-midnight value, wrapping into
// [0, 86400) first -- callers may have shifted a UTC time-of-day by
// recurring_rule_utc_offset_minutes, which can push it negative or past
// 24h before this is called.
std::string TimeOfDayString(int secondsSinceMidnight);

// Inverse of TimeOfDayString(): parses "HH:MM:SS" (or "HH:MM") into
// seconds since midnight. Returns 0 on anything unparseable, including a
// field outside its real range (hours 0-23, minutes/seconds 0-59) -- see
// this function's own comment (DateTimeFormat.cpp) for why.
int SecondsSinceMidnightFromString(const std::string& hms);

// "YYYY-MM-DD" for the UTC calendar date of a time_t (this addon only
// ever stores a rule's start_date/end_date as UTC midnight of the
// intended local calendar date -- see RecurringRule's own comment).
std::string DateStringFromTime(time_t t);

// Inverse of DateStringFromTime(): parses "YYYY-MM-DD" into a UTC
// midnight time_t. Returns 0 on anything unparseable.
time_t TimeFromDateString(const std::string& dateStr);

} // namespace dispatcharr
