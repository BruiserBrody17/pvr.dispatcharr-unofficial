#include "DateTimeFormat.h"

#include "TimeUtil.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace dispatcharr
{

std::string IsoFromTime(time_t t)
{
  char buf[32];
  tm tmVal{};
  GmTimeUtc(t, &tmVal);
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmVal);
  return std::string(buf);
}

time_t TimeFromIso(const std::string& isoStr)
{
  if (isoStr.size() < 19)
    return 0;

  tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(isoStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(isoStr.substr(5, 2)) - 1;
    tmVal.tm_mday = std::stoi(isoStr.substr(8, 2));
    tmVal.tm_hour = std::stoi(isoStr.substr(11, 2));
    tmVal.tm_min = std::stoi(isoStr.substr(14, 2));
    tmVal.tm_sec = std::stoi(isoStr.substr(17, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }
  // A field outside its real range is not a timestamp. PortableTimeGm() normalizes it
  // the way timegm() does ("2026-13-45T25:61:61Z" came back as a time about a year
  // later), and that wrong-but-plausible value would flow straight into the
  // scheduling arithmetic. A leap second (60) is tolerated.
  if (tmVal.tm_mon < 0 || tmVal.tm_mon > 11 || tmVal.tm_mday < 1 || tmVal.tm_mday > 31 || tmVal.tm_hour < 0 ||
      tmVal.tm_hour > 23 || tmVal.tm_min < 0 || tmVal.tm_min > 59 || tmVal.tm_sec < 0 || tmVal.tm_sec > 60 ||
      !IsValidCivilDate(tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday))
    return 0;
  time_t utc = PortableTimeGmSaturating(&tmVal);

  // A genuine, non-zero trailing UTC offset -- fix for a real, confirmed
  // bug found via a project-wide review (a 35th-pass audit, confirmed
  // against Dispatcharr's own real current upstream source, not itself
  // independently reproduced): this function's own header comment's
  // "every timestamp elsewhere in this API being UTC-normalized already"
  // is correct for a serialized Django model field (confirmed by an
  // 18th-pass audit: TIME_ZONE="UTC", no timezone.activate() call
  // anywhere), but custom_properties.program.start_time/end_time
  // (RecordingParser.cpp) is a real exception -- not a serialized model
  // field at all, but a plain JSON string Dispatcharr's own recurring-
  // rule scheduler hand-builds via a timezone-*aware* Python datetime's
  // own isoformat() call, in that rule's own configured system timezone
  // (sync_recurring_rule_impl(), apps/channels/tasks.py -- an admin-
  // configurable value this addon's own recurring_rule_timezone setting
  // already has to bridge elsewhere, not Django's global TIME_ZONE).
  // Silently ignoring a genuine "-04:00"-style offset there returned a
  // time off by the whole zone offset -- reachable in practice via
  // Recording::programStartTime (added the previous, 34th, pass), which
  // fed a wrong EPG match straight into SetEPGUid()/SetEPGEventId().
  //
  // Skips an optional fractional-seconds part first (already tolerated
  // before this fix, just via truncation rather than being read), then
  // "Z" (no adjustment) or a "+HH:MM"/"+HHMM"/"-HH:MM"/"-HHMM" offset.
  // Anything else unrecognized here is left as UTC, matching this
  // function's own existing best-effort tolerance for anything it can't
  // confidently parse.
  std::size_t pos = 19;
  if (pos < isoStr.size() && isoStr[pos] == '.')
  {
    ++pos;
    while (pos < isoStr.size() && std::isdigit(static_cast<unsigned char>(isoStr[pos])))
      ++pos;
  }
  if (pos < isoStr.size() && (isoStr[pos] == '+' || isoStr[pos] == '-'))
  {
    bool negative = isoStr[pos] == '-';
    // Reads exactly two digits at `p`, or -1 if either character there
    // isn't a digit (including running off the end of the string).
    auto readTwoDigits = [&](std::size_t p) -> int
    {
      if (p + 1 >= isoStr.size() || !std::isdigit(static_cast<unsigned char>(isoStr[p])) ||
          !std::isdigit(static_cast<unsigned char>(isoStr[p + 1])))
        return -1;
      return (isoStr[p] - '0') * 10 + (isoStr[p + 1] - '0');
    };
    std::size_t hhPos = pos + 1;
    int offsetHours = readTwoDigits(hhPos);
    if (offsetHours >= 0)
    {
      std::size_t mmPos = hhPos + 2;
      if (mmPos < isoStr.size() && isoStr[mmPos] == ':')
        ++mmPos; // "+HH:MM" -- skip the colon "+HHMM" doesn't have
      int offsetMinutes = readTwoDigits(mmPos);
      // A real offset is at most a day (+HH:MM is within +-23:59); "+99:99" is a
      // malformed value and used to shift the result by over four days.
      if (offsetMinutes >= 0 && offsetHours <= 23 && offsetMinutes <= 59)
      {
        time_t offsetSeconds = static_cast<time_t>(offsetHours) * 3600 + offsetMinutes * 60;
        utc -= negative ? -offsetSeconds : offsetSeconds;
      }
    }
  }
  return utc;
}

std::string TimeOfDayString(int secondsSinceMidnight)
{
  int s = secondsSinceMidnight % 86400;
  if (s < 0)
    s += 86400;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  return std::string(buf);
}

int SecondsSinceMidnightFromString(const std::string& hms)
{
  // Parsed by hand, one bounded field at a time, rather than via
  // sscanf("%d:%d:%d") -- fix for a real, confirmed undefined-behavior
  // gap (added 2026-09-27, a 64th-pass audit, found via a project-wide
  // UB review, confirmed by direct reproduction under
  // -fsanitize=undefined, not reproduced live): the old version's own
  // `h * 3600 + m * 60 + s` was signed-integer overflow for a large
  // enough hour or minute field (UBSan, for "600000:00:00": "600000 *
  // 3600 cannot be represented in type 'int'"), and sscanf's
  // own %d conversion is itself undefined behavior per the C standard
  // for a digit string outside int's range, before that multiplication
  // is even reached. Dispatcharr's own RecurringRecordingRule.start_time/
  // end_time is a Django TimeField, which only ever serializes as a
  // zero-padded "HH:MM:SS[.ffffff]" within a real day -- so this is
  // defensive against a malformed server response (the same "don't let
  // a server-supplied value reach UB" standard FieldOr<T>()'s own
  // out-of-range guard already applies), not a fix for anything a
  // well-behaved Dispatcharr instance sends. Each field stops
  // accumulating (and the whole parse is rejected as unparseable) the
  // moment it exceeds its own real range (hours 0-23, minutes/seconds
  // 0-59), so no intermediate value can ever get anywhere near int's
  // limits. Otherwise keeps the old sscanf-based version's own
  // tolerances: leading whitespace before a field, a missing seconds
  // field ("HH:MM"), and anything trailing a parsed field (e.g.
  // Dispatcharr's own ".ffffff" fractional seconds) are all still
  // accepted. A sign ("-1:00", "+1:00") is no longer accepted: in the
  // hour or minute field it makes the whole string unparseable, and in
  // the seconds field it's treated the same as any other non-digit
  // there -- a missing seconds field ("01:02:-5" reads as 01:02:00).
  // The old version instead folded a sign into the result (a negative
  // time-of-day for "-1:00"), which no real TimeField value can ever be.
  constexpr int kMaxField[3] = {23, 59, 59};
  int fields[3] = {0, 0, 0};
  int parsedCount = 0;
  std::size_t pos = 0;
  while (parsedCount < 3)
  {
    while (pos < hms.size() && std::isspace(static_cast<unsigned char>(hms[pos])))
      ++pos;
    std::size_t digitsStart = pos;
    int value = 0;
    while (pos < hms.size() && std::isdigit(static_cast<unsigned char>(hms[pos])))
    {
      value = value * 10 + (hms[pos] - '0');
      if (value > kMaxField[parsedCount])
        return 0;
      ++pos;
    }
    if (pos == digitsStart)
      break; // no digits here -- stop, same as sscanf failing this conversion
    fields[parsedCount++] = value;
    if (pos >= hms.size() || hms[pos] != ':')
      break;
    ++pos;
  }
  if (parsedCount < 2)
    return 0;
  return fields[0] * 3600 + fields[1] * 60 + fields[2];
}

std::string DateStringFromTime(time_t t)
{
  char buf[16];
  tm tmVal{};
  GmTimeUtc(t, &tmVal);
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmVal);
  return std::string(buf);
}

time_t TimeFromDateString(const std::string& dateStr)
{
  if (dateStr.size() < 10)
    return 0;
  tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(dateStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(dateStr.substr(5, 2)) - 1;
    tmVal.tm_mday = std::stoi(dateStr.substr(8, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }
  if (!IsValidCivilDate(tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday))
    return 0;
  return PortableTimeGmSaturating(&tmVal);
}

} // namespace dispatcharr
