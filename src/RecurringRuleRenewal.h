#pragma once

#include "DispatcharrClient.h"

#include <ctime>
#include <vector>

namespace dispatcharr
{

// UPDATE (2026-10-02, ManagedRecurringRule.h): only a rule this addon owns -- one
// whose name carries the `[Kodi]` tag -- is ever renewed, which closes the "can't
// tell addon-managed rules from user-created ones" gap several of the notes below
// call a separate, bigger one; and an owned, enabled rule whose end date has
// already passed (or that has none) is now REVIVED instead of left dead, since it
// is ours and nothing says it was meant to end. The notes below on an expired
// rule apply to a rule that is not owned.
//
// Pure decision core of PVRDispatcharr::RenewRecurringRules()'s per-rule
// loop -- decides whether a recurring rule's end_date should be pushed
// forward this cycle. Returns false for:
//   - a disabled rule (nothing materializes for it anyway);
//   - one whose end_date has already passed (fixed for a real, confirmed
//     bug found via a project-wide review, not itself independently
//     reproduced: the "comfortably inside the window" check below is a
//     `daysLeft >= windowDays / 2` comparison, and a negative `daysLeft`
//     -- an already-expired rule -- was never caught by it, so an
//     expired-but-still-enabled rule fell through to the same renewal
//     logic as one genuinely nearing its window limit, silently
//     resurrecting it. GetRecurringRules() returns every recurring rule
//     Dispatcharr knows about, not just ones this addon itself created
//     with its own rolling-window convention -- see docs/RECURRING_RULES.md
//     -- so this also matters for a rule a user created directly via
//     Dispatcharr's own web UI with a deliberate, finite end_date;
//     distinguishing addon-managed rules from others is a separate,
//     bigger gap, not fixed here);
//   - one still comfortably inside its rolling window (more than half of
//     `windowDays` remaining, checked here rather than waiting until it's
//     about to actually run out, so most cycles do nothing at all -- see
//     PVRDispatcharr.h's own kRecurringRuleWindowDays comment);
//   - one with an occurrence currently recording or starting within
//     `safetyMarginSeconds` (defense in depth -- live testing confirmed
//     Dispatcharr's own regeneration on this kind of update already
//     leaves an in-progress/completed occurrence alone, see
//     ExtendRecurringRuleEndDate()'s own comment and
//     docs/RECURRING_RULES.md -- but this doesn't rely on that alone).
//     This "imminent occurrence" check also requires `rec.endTime > now`
//     (added 2026-09-26, fixing a real bug found via a project-wide
//     review, not itself independently reproduced): a recurring
//     occurrence that never actually ran -- e.g. Dispatcharr/Celery was
//     down for its whole window -- stays at `status=="scheduled"`
//     forever (see ParseRecordingFields()), with both start and end time
//     now in the past. Without this guard, such an occurrence's
//     `rec.startTime - now` is a large negative number, always less than
//     `safetyMarginSeconds`, so it read as permanently "imminent" and
//     silently blocked this rule from ever renewing again -- right up
//     until its own end_date ran out, at which point the check above
//     started refusing it as expired instead. One missed occurrence
//     should never be able to end a rolling recurring rule's own life
//     early like that;
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

// Pure decision core of PVRDispatcharr::UpdateTimer()'s own recurring-rule
// branch -- whether this edit should also push the rule's end_date
// forward (via ExtendRecurringRuleEndDate(), alongside whatever else
// UpdateRecurringRule() itself changes), given `wasEnabled` (the rule's
// own cached `enabled` from *before* this edit, in m_cachedRecurringRules),
// `enabled` (the state this edit results in), and `cachedEndDate` (the
// addon's own last-known end_date for this rule -- may be a call or two
// stale, but that's fine here, same tolerance ShouldRenewRecurringRule()
// itself already has for its own periodic check).
//
// Needed specifically for a gap ShouldRenewRecurringRule()'s own
// already-expired-rule fix (above) deliberately opened, found via a
// project-wide review, not itself independently reproduced:
// UpdateRecurringRule()'s own PATCH deliberately omits end_date unless the edit's
// patch carries one (RecurringRuleEdit.h; see its own comment), so disabling a rule and leaving it disabled long enough
// for its existing end_date to fall into the past, then re-enabling it
// from Kodi, left it enabled with an end_date already in the past --
// which the periodic renewal loop now correctly refuses to touch, since
// it can no longer tell that case apart from a rule a user deliberately
// created with a genuine, intentionally-finite end_date (the same
// "can't tell addon-managed rules apart from others" limitation that fix
// already documents).
//
// Requires a genuine `!wasEnabled && enabled` transition, not just the
// post-edit `enabled` value on its own -- a real, confirmed gap found
// via a project-wide review in a later pass than the one that introduced
// this function, not itself independently reproduced: checking only the
// post-edit state fired on *any* edit of an already-enabled rule (a
// rename, a schedule tweak), not just an explicit re-enable, silently
// resurrecting exactly the kind of deliberately-finite, already-expired,
// still-enabled web-UI rule the "don't resurrect an expired rule" fix
// above exists to leave alone -- reopening that same protection through
// an edit that had nothing to do with enabling anything. Requiring the
// transition means the user's own explicit re-enable action is still the
// signal (so this deliberately doesn't need the occurrence-safety checks
// ShouldRenewRecurringRule() itself needs for its own unattended,
// periodic version of the same decision), without also firing on an
// edit that doesn't touch enabled state at all.
//
// Uses the same `daysLeft < windowDays / 2` threshold as
// ShouldRenewRecurringRule(), just without excluding a negative
// `daysLeft` -- that's precisely the case this needs to catch.
bool ShouldExtendRecurringRuleEndDateOnUpdate(bool wasEnabled, bool enabled, time_t cachedEndDate, time_t now,
                                              int windowDays);

// Whether editing a recurring rule's own start_date (Kodi's "First Day",
// i.e. `newStartDate`, this edit's own freshly-computed
// ComputeRecurringRuleFields() output) needs its end_date pushed forward
// in the same PVRDispatcharr::UpdateTimer() edit -- and if so, what to
// push it to. Returns 0 when no extension is needed.
//
// Added 2026-09-26 (a 19th-pass audit), fixing a real bug confirmed
// against Dispatcharr's own real current upstream source, not itself
// independently reproduced: UpdateRecurringRule()'s own PATCH
// deliberately omits end_date unless its edit patch carries one (RecurringRuleEdit.h;
// an ordinary schedule edit carries none), relying on
// RecurringRecordingRuleSerializer's partial-update fallback to the
// instance's *existing* end_date -- but that same validate() rejects
// `end_date < start_date`, and (an overnight rule's own case, where the
// two dates can be equal) `combine(end_date, end_time) <=
// combine(start_date, start_time)` too. Kodi's own First Day picker
// offers dates up to a year out, and a renewed rule's own cached
// end_date sits at most `windowDays` out (see
// PVRDispatcharr.h's own kRecurringRuleWindowDays comment) -- so simply
// moving First Day forward past (or onto, for an overnight rule) the
// rule's own current end_date fails this edit outright with a 400,
// entirely independent of `ShouldExtendRecurringRuleEndDateOnUpdate()`
// above (that one only fires on a genuine disabled->enabled transition;
// this can happen to an already-enabled rule on a plain schedule edit).
// Uses `newStartDate >= cachedEndDate`, not `>`, to also catch the
// overnight equal-dates case. `cachedEndDate <= 0` (the same
// "open-ended rule" sentinel used elsewhere) always returns 0 -- an
// open-ended rule has no end_date to protect in the first place.
time_t ComputeRecurringRuleEndDateForStartDateChange(time_t newStartDate, time_t cachedEndDate, int windowDays);

// Whether an enabled recurring rule's own end_date has already passed --
// the pure condition behind PVRDispatcharr::GetTimers()'s own recurring-
// rule display state, added 2026-09-26 (a 31st-pass audit, fixing a real,
// confirmed gap found via a project-wide review, not itself independently
// reproduced). `GetTimers()` previously set PVR_TIMER_STATE_SCHEDULED for
// any rule with `rule.enabled` true, with no check against `endDate` at
// all -- but Dispatcharr's own scheduler (apps/channels/tasks.py) never
// generates an occurrence past a rule's own end_date, confirmed against
// its real current upstream source, regardless of whether the rule
// itself is still nominally "enabled" server-side. This addon's own
// ShouldRenewRecurringRule() (above) already refuses to push an expired
// rule's end_date forward again -- deliberately, since it can't tell an
// addon-managed rolling-window rule apart from one a user created
// directly via Dispatcharr's own web UI with a genuine, intentionally-
// finite end_date (see that function's own comment) -- so a rule in
// exactly this state (enabled, but past its own end_date, whichever way
// it got there) displayed as a perfectly healthy SCHEDULED repeating
// timer in Kodi's Timers list while silently recording nothing at all,
// with no signal telling the user anything was wrong.
//
// `endDate <= 0` (the same "no end_date at all" sentinel
// ShouldExtendRecurringRuleEndDateOnUpdate()/
// ComputeRecurringRuleEndDateForStartDateChange() above already rely on
// for a null/unparseable end_date) never counts as expired -- there's no
// window to have closed. A disabled rule isn't this function's concern
// either; GetTimers() already has its own, separate PVR_TIMER_STATE_DISABLED
// branch for that.
//
// `offsetMinutes` (added 2026-09-26, a 36th-pass audit, fixing a real,
// confirmed off-by-one-day bug found via a project-wide review, confirmed
// against Dispatcharr's own real current upstream source, not itself
// independently reproduced) bridges a genuine mismatch this function's
// own original implementation (a plain `endDate <= now`, `endDate` being
// UTC midnight of the raw `end_date` calendar-date string,
// TimeFromDateString()) missed: Dispatcharr's own `end_date` is
// *inclusive* -- `sync_recurring_rule_impl()`'s own day-generation loop
// (`apps/channels/tasks.py`) covers `end_window - start_window + 1` days
// and only skips a day once `target_date > end_limit`, so the end_date
// day itself (in Dispatcharr's own configured system timezone, not UTC)
// still gets a real occurrence. The old `endDate <= now` flagged a rule
// as expired the instant UTC clock reached midnight of the end_date
// calendar day -- for any zone behind UTC (e.g. America/New_York), that
// moment is still the *previous evening* in Dispatcharr's own local
// time, so the rule was wrongly shown as PVR_TIMER_STATE_ERROR up to a
// full day or more before its own last occurrence had even aired.
// `endDate + 86400 - offsetSeconds` is the UTC instant marking
// Dispatcharr's own local calendar moving past end_date entirely (local
// midnight beginning the day after end_date, converted to UTC via the
// same offset convention ComputeRecurringRuleDisplayTimes()'s own
// firstDayOut already uses) -- a deliberately conservative, whole-extra-
// day margin rather than also accounting for the end_date occurrence's
// own end-of-day/overnight-rollover time, matching this codebase's own
// established tolerance elsewhere for "a valid day, not necessarily the
// exact moment" (see ComputeRecurringRuleDisplayTimes()'s own comment).
bool HasRecurringRuleEndDatePassed(bool enabled, time_t endDate, time_t now, int offsetMinutes);

} // namespace dispatcharr
