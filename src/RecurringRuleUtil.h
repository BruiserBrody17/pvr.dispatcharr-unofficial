#pragma once

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace dispatcharr
{

// How many calendar days later an occurrence falls in Dispatcharr's zone than
// in Kodi's, for an occurrence whose UTC time of day is `utcSecondsOfDay`
// (any int; wrapped into one day). Both offsets are minutes east of UTC, and
// both are the offsets in effect at that occurrence's own instant -- with
// daylight saving in play they are not constants of a zone.
// Usually 0 -- it is nonzero only when that time of day lands on different
// sides of midnight in the two zones, which needs the two zones to differ and
// the rule to sit close enough to midnight for that difference to matter.
// -1 means Dispatcharr's calendar is a day behind Kodi's (Kodi's Monday is
// Dispatcharr's Sunday), +1 a day ahead. Always 0 for two equal offsets, and
// so for a Kodi and a Dispatcharr in the same zone at every time of year.
//
// It depends only on the time of day and the two offsets, not on the
// occurrence's date: the same instant-of-day lands a fixed number of
// calendar days apart in two fixed-offset zones on any date, which is what
// lets a recurring rule's single weekday list be translated with one number.
// (Across a daylight-saving transition in either zone the true shift can
// change for an occurrence on the far side of it, which a single weekday
// list cannot express; the addon resolves each zone's offset at the rule's
// own start instant, which is the instant the translated start time and
// first day describe.)
//
// Why a recurring rule needs this at all: Kodi's own timer dialog
// (GUIDialogPVRTimerSettings.cpp) converts a repeating timer's start time,
// end time and first day between local time and UTC
// (CPVRTimerInfoTag::ConvertUTCToLocalTime()/ConvertLocalTimeToUTC(), which
// ask libc's localtime() about the very instant being converted, so they
// follow daylight saving per date), but sets the weekday bitmask
// (`m_iWeekdays`) straight from its checkboxes with no conversion, and
// FillAddonTimer() (PVRTimerInfoTag.cpp) copies it through raw -- so "Mo-Fr
// at 8:00 PM" means Monday to Friday by the local clock Kodi shows next to
// it. Dispatcharr reads `days_of_week` in its own configured system
// timezone instead. With the two zones equal this is the identity and
// nothing here ever mattered; with them different, every evening or late-
// night rule landed its weekdays a day early or late (confirmed against
// Kodi's own real source; see docs/OPEN_ITEMS.md).
//
// Zero Kodi/curl/member-state dependency -- see
// ../tests/test_recurring_rule_util.cpp.
int ComputeRecurringRuleDayShift(int utcSecondsOfDay, int kodiOffsetMinutes, int dispatcharrOffsetMinutes);

// Moves every selected weekday in a Monday-first bitmask (bit 0 = Monday ..
// bit 6 = Sunday, the layout Kodi's PVR_WEEKDAY_* and Dispatcharr's 0-6
// `days_of_week` share -- see RecurringRuleWeekdays.h) `days` days later,
// wrapping around the week; a negative `days` moves them earlier. Bits above
// bit 6 are dropped. The inverse of rotating by `-days`.
unsigned int RotateWeekdaysBitmask(unsigned int bitmask, int days);

// Pure integer-arithmetic core of PVRDispatcharr::ComputeRecurringRuleFields
// -- converts Kodi's UTC start-end-time-of-day/first-day and its own
// weekday bitmask into Dispatcharr's own representation (0-6 day list, its
// configured-system-timezone-local time-of-day via offsetMinutes, always
// wrapped into [0, 86400) -- the same representation the server reports
// back, see the .cpp -- and a start date in that same calendar). Returns
// false (with error set) only when no weekday is selected at all --
// everything else here is pure, infallible conversion.
//
// Takes plain values (not a kodi::addon::PVRTimer&) and an explicit
// `nowUtc` (used only when `firstDay` is <= 0, meaning Kodi didn't supply
// one) specifically so this is unit-testable standalone with no Kodi SDK
// dependency; see ../tests/test_recurring_rule_util.cpp. startTime/endTime/
// firstDay are already UTC (this addon's convention throughout), and a UTC
// time_t's own modulo-86400 gives an exact, DST-free calendar-day/
// time-of-day split with no gmtime/timegm round-trip needed. The only
// places a real timezone enters are the two explicit offsets:
// offsetMinutes is Dispatcharr's own (non-UTC-by-default) system timezone,
// which the time of day is shifted into, and kodiOffsetMinutesAt is the
// local timezone Kodi itself displays, asked about an instant because it
// follows daylight saving per date -- see below.
//
// weekdaysBitmask is in Kodi's local calendar, not UTC's and not
// Dispatcharr's, and firstDay's calendar *date* is too: see
// ComputeRecurringRuleDayShift() for why, and for what it does when the two
// zones differ. daysOfWeekOut is weekdaysBitmask rotated by that shift into
// Dispatcharr's calendar, and startDateOut is firstDay's Kodi-local date
// (read with Kodi's offset at firstDay itself) moved by the same shift -- the
// date, because Kodi's dialog only ever replaces the date part of first day
// and leaves whatever time of day the instant already had
// (SetDateFromIndex()), so that time carries no meaning. The shift is
// computed with Kodi's and Dispatcharr's offsets at startTime, the instant
// the translated time of day describes. Both are unchanged from before this
// distinction existed whenever the two zones agree (at every time of year).
// (This used to pass the bitmask through unshifted and take startDateOut
// from the Dispatcharr-local date of the firstDay *instant*, which
// disagrees with the weekday list in exactly the cases the shift is nonzero
// -- the open item this replaced, from a 33rd-pass audit, 2026-09-26,
// confirmed live 2026-09-30. A first attempt at the fix read Kodi's offset
// once, at "now", assuming Kodi applies one cached bias to every date; Kodi
// does not, and for a rule near midnight whose start date lay in the other
// daylight-saving period from today that rotated the weekdays of a Kodi and
// Dispatcharr in the *same* zone -- caught live the same day.)
//
// A firstDay <= 0 (Kodi didn't supply one) starts the rule on Dispatcharr's
// own current date -- an occurrence earlier than that is already in the past
// either way, so it never excludes one a caller could still record.
bool ComputeRecurringRuleFields(time_t startTime, time_t endTime, time_t firstDay, unsigned int weekdaysBitmask,
                                int offsetMinutes, const std::function<int(time_t)>& kodiOffsetMinutesAt, time_t nowUtc,
                                std::vector<int>& daysOfWeekOut, int& startSecondsOut, int& endSecondsOut,
                                time_t& startDateOut, std::string& error);

// The inverse of ComputeRecurringRuleFields() above --
// PVRDispatcharr::GetTimers()'s own recurring-rule display start/end
// time computation. Shifts Dispatcharr's own configured-system-
// timezone-local start/endTimeOfDaySeconds back to UTC (offsetMinutes,
// the same value ComputeRecurringRuleFields() applied in the opposite
// direction) before combining with the already-UTC startDate. Kodi only
// actually uses the time-of-day portion of these for a repeating
// timer's display; the date portion just needs to be *a* valid day, not
// necessarily the exact next occurrence.
//
// Adjusts endTimeOut forward by a day for an "overnight" rule whose
// endTimeOfDaySeconds is numerically <= startTimeOfDaySeconds (e.g. a
// 23:00-01:00 rule), added 2026-09-26 after confirming (via an 18th-pass
// audit that cloned Dispatcharr's own real current upstream source into
// a scratchpad, never committed to this repo) that its own scheduler
// treats such a rule identically: sync_recurring_rule_impl()
// (apps/channels/tasks.py) does `if end_dt <= start_dt: end_dt +=
// timedelta(days=1)`. Before this was confirmed, this function
// deliberately left such a rule's displayed end time landing before its
// start time -- see ../tests/test_recurring_rule_util.cpp for the
// now-corrected regression coverage.
//
// firstDayOut is the value PVRDispatcharr::GetTimers() should hand to
// kodi::addon::PVRTimer::SetFirstDay(): noon, in Kodi's own zone, of the
// Kodi-local date the rule's first occurrence falls on, which is startDate's
// date moved back by dayShiftOut. ComputeRecurringRuleFields()'s own
// `firstDay` handling reads that date back and moves it forward by the same
// shift, so handing Kodi this and taking back whatever it echoes on ANY
// later edit (timer.GetFirstDay(), unchanged if the user didn't touch that
// field) reconstructs startDate exactly. Noon rather than midnight because
// Kodi shows only the date of it: a time twelve hours from either midnight
// keeps the date right even where Kodi's offset at that instant differs from
// the one the guess was made with (a daylight-saving transition that day),
// and where local midnight doesn't exist or happens twice. An earlier
// version of this call site passed `startDate` straight through unshifted,
// then applied the zone offset a *second* time on the way back in -- for a
// zone behind UTC that floored to the previous day, so every single edit
// (even one that only toggled enabled/disabled) silently moved the rule's
// start date back a full day, compounding on each further edit. A later
// version shifted it by Dispatcharr's own offset, which round-tripped but
// named a first day that could be a day off from the one Kodi's own dialog
// shows whenever Kodi's zone isn't Dispatcharr's.
//
// dayShiftOut is ComputeRecurringRuleDayShift() for this rule's start time,
// the number of days to move the weekday list *back* by
// (RotateWeekdaysBitmask(mask, -dayShiftOut)) to get it into Kodi's
// calendar -- returned rather than applied so the caller, which already
// builds the bitmask itself, keeps doing that in one place.
//
// resolveOffsetMinutes takes a *candidate* UTC instant rather than a
// plain int, and this function resolves it via a fixed-point iteration
// (start from the offset in effect at startDate's own UTC midnight,
// tentatively convert, then re-resolve at that tentative instant and
// repeat), capped at a couple of extra iterations as a defensive bound,
// not because more are ever expected to matter -- this addon only ever
// computes a simple +/-1h DST delta (see TimeZoneUtil.cpp), so this
// converges immediately in every real case. Fix for a real, confirmed,
// LIVE bug (see docs/OPEN_ITEMS.md's 2026-09-29 live check): resolving
// the offset only once, at startDate's own UTC midnight, was wrong by
// exactly the DST delta for any rule whose startTimeOfDaySeconds itself
// falls on the *other* side of a transition landing that same calendar
// day (every DST-observing zone has two such days a year) -- confirmed
// live against a real fall-back-date rule, off by
// exactly one hour, wrong from the very first read of a freshly
// server-created rule, not just after an edit round trip. The start's
// refined offset is what firstDayOut/dayShiftOut use; endTimeOut resolves its
// own offset the same way, from the start's as its first guess, so a clock
// change between the rule's start and end time-of-day on its first day gives the
// end its real instant (2026-10-06, see docs/OPEN_ITEMS.md: it used to take the
// start's offset and show the end an hour off on exactly that one day).
void ComputeRecurringRuleDisplayTimes(time_t startDate, int startTimeOfDaySeconds, int endTimeOfDaySeconds,
                                      const std::function<int(time_t)>& resolveOffsetMinutes,
                                      const std::function<int(time_t)>& kodiOffsetMinutesAt, time_t& startTimeOut,
                                      time_t& endTimeOut, time_t& firstDayOut, int& dayShiftOut);

// Drops the seconds from a new recurring rule's start and end time of day
// (Dispatcharr-local seconds since midnight, as ComputeRecurringRuleFields()
// returns them). Kodi's timer dialog shows only minutes, but its default
// start time is "now" and keeps its seconds, and a time typed on the numeric
// pad keeps the seconds of the value it replaced -- so a rule made from the
// dialog reached Dispatcharr as HH:MM:28-HH:MM:28 and recorded from HH:MM:28,
// up to a minute after the time the user saw (found live 2026-09-30).
//
// For CREATING only. Kodi echoes back the seconds it was shown on every
// later edit, even a bare enable/disable toggle, so truncating on an edit
// would silently rewrite a rule whose server-side seconds are nonzero (a
// rule made in Dispatcharr's own UI, or by an older build of this addon).
//
// Leaves both untouched when truncating would make them equal although they
// weren't: Dispatcharr reads end <= start as an overnight rule, so a
// 40-second window inside one minute would turn into a 24-hour one.
void TruncateRuleTimesToWholeMinutes(int& startSeconds, int& endSeconds);

} // namespace dispatcharr
