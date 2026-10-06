*(part of the pvr.dispatcharr-unofficial notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Recurring (day-of-week) timer rules

Implemented on top of Dispatcharr's own `RecurringRecordingRule` model,
which already existed in its source with nothing on this addon's side
wired up to it. Confirmed fully live/reachable before building against it
(not assumed): model -> serializer -> `RecurringRecordingRuleViewSet`
(`GET`/`POST /api/channels/recurring-rules/`, `GET`/`PUT`/`PATCH`/`DELETE
.../{id}/`) -> an hourly Celery beat task
(`maintain_recurring_recordings`) -> `sync_recurring_rule_impl`, which
materializes real `Recording` rows up to 14 days ahead for every enabled
rule, tagging each with `custom_properties.rule = {"type": "recurring",
"id": <rule id>, ...}`.

## Kodi-side representation: parent rule + child instances

Uses Kodi's standard repeating-timer convention (`PVRTimer::
SetParentClientIndex()`, confirmed in kodi-dev-kit's `pvr_timers.h`) --
not a bespoke scheme:

- The rule itself is one "parent" timer (`kTimerTypeRecurring`,
  `PVR_TIMER_TYPE_IS_REPEATING`), `ClientIndex = rule.id |
  kRecurringRuleIndexFlag` (`0x20000000`, distinct from series rules'
  own `0x40000000` hash-based namespace -- recurring rules have a real
  numeric id, so no hashing is needed here, just a separate bit).
- Each already-materialized `Recording` (an upcoming or in-progress
  occurrence) is a plain one-time timer, same as ever, but with
  `SetParentClientIndex(rule.id | kRecurringRuleIndexFlag)` set when its
  `custom_properties.rule.id` is present -- linking it back to its
  parent in Kodi's own timer list UI.
- Deleting the parent (`DeleteTimer()`) deletes the Dispatcharr rule,
  which also purges its own *future* (`start_time__gte=now`) recordings
  server-side -- confirmed live: deleting a rule while its current
  occurrence was actively recording correctly left that one alone
  (Dispatcharr's own purge query only matches future recordings), while
  an earlier test deleting a rule *before* its occurrence's start time
  removed both the rule and the not-yet-started recording together.
  Deleting an individual child instance instead falls through to the
  ordinary one-time-recording delete path (`DeleteRecording()`) --
  Dispatcharr's own idempotency check has no "user explicitly skipped
  this day" concept, so the next hourly sync could re-materialize a
  deleted single occurrence if it's still within the rule's own window;
  this is a Dispatcharr-side limitation, not something addressed here.

**Update: `UpdateTimer()` is now implemented** (see the "UpdateTimer()"
entry in `docs/RECORDINGS.md` for the full cross-timer-type story), so
`PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE` is declared for this type and a
rule's `enabled` flag can be flipped straight from Kodi's timer list --
Kodi implements that action by calling `UpdateTimer()` with everything
else unchanged and just the state flipped, the same call full edits use.
Deleting the timer is still the way to stop a rule for good, not just
pause it.

No per-timer "end date" either: Kodi's own repeating-timer type
attributes have `PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY` but no equivalent
"last day" flag (confirmed against kodi-dev-kit's full
`PVR_TIMER_TYPE_SUPPORTS_*` list), while Dispatcharr's serializer
requires a real `end_date` on every create regardless. Delete the timer
whenever you actually want it to stop.

**Update (2026-09-26): a new recurring timer created already-disabled
was created enabled instead, found via a project-wide review, not
itself independently reproduced.** `PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE`
being a per-timer-*type* capability (not an edit-only affordance) means
Kodi's shared Add/Edit timer-settings dialog shows the same Enabled
toggle when *creating* a new recurring timer, not just when editing an
existing one -- but `AddTimer()`'s own recurring-rule branch never read
`timer.GetState()` at all, and `CreateRecurringRule()` always sent
`enabled: true` unconditionally. A user creating a new recurring timer
with Enabled unchecked (e.g. to stage it for later) instead got a rule
that started materializing and recording occurrences immediately, with
Kodi showing it as Scheduled on the next refresh -- the opposite of
what was asked for, with no error anywhere. Fixed by giving
`CreateRecurringRule()` an explicit `enabled` parameter and having
`AddTimer()` pass `timer.GetState() != PVR_TIMER_STATE_DISABLED`, the
same check `UpdateTimer()`'s own edit path already used.

**Update: a rolling `end_date` window, not a flat 3-year one.** This
used to set `end_date` to `start_date + 3 years` on creation, on the
theory that Dispatcharr only lazily materializes ~14 days ahead
regardless of how far out `end_date` sits, making a far-future value
"free". **Confirmed live that theory was wrong**: creating a real weekly
rule with a 3-year `end_date` caused Dispatcharr to eagerly materialize
*every* occurrence between `start_date` and `end_date` synchronously,
right at creation -- 157 real `Recording` rows for one rule, which would
have shown as 157 child timers in Kodi's own timer list (more for a rule
repeating on several days a week). Now a rule's `end_date` is kept to a
rolling `kRecurringRuleWindowDays` (30) window instead, topped back up
periodically by `RenewRecurringRules()` (piggybacking on the existing
recording-refresh background thread, no separate thread or setting)
once less than half that window remains -- so a recurring rule still
behaves like "create once, forget about it" from the user's side, just
without the eager-materialization cost. Confirmed live, separately, that
extending `end_date` is safe to do while an occurrence is actively
recording: it only regenerates *future* (not yet started) occurrences
-- a real in-progress recording, id 1, kept its id, `started_at`, and
file path completely unchanged across a mid-recording `end_date` PATCH,
and went on to complete normally (`status: "completed"`,
`remux_success: true`, real bytes written). `RenewRecurringRules()`
still skips a rule with an occurrence currently recording or starting
within the next hour regardless, as defense in depth rather than relying
solely on that server-side scoping.

**Update (2026-09-26, a 31st-pass audit, confirmed against Dispatcharr's
own real current upstream source, not itself independently reproduced):
"create once, forget about it" has two real edge cases, both logged to
docs/OPEN_ITEMS.md rather than fixed blind, since both need a design
decision this addon can't make on its own.** First, `RenewRecurringRules()`
acts on *every* enabled recurring rule `GetRecurringRules()` returns --
it has no way to tell an addon-managed rolling-window rule apart from
one a user created directly via Dispatcharr's own web UI (which always
has its own genuine, user-chosen, finite `end_date` -- Dispatcharr's own
serializer requires one), so a web-UI rule left running anywhere near
this addon gets its own deliberately-chosen `end_date` silently pushed
forward too, converting it into an unintended rolling recurring rule.
Second, the flip side of the pass-19 "never resurrect an expired rule"
fix above: if no Kodi instance running this addon renews a rule for
longer than the window left since its last renewal (roughly 15-30
days, depending on when in its own cycle it was), its `end_date` simply
runs out and this addon then correctly refuses to touch it again --
`GetTimers()` now shows that state distinctly (`PVR_TIMER_STATE_ERROR`,
also fixed this pass, `RecurringRuleRenewal.h`'s `HasRecurringRuleEndDatePassed()`),
so at least it no longer looks like a healthy `SCHEDULED` timer, but the
rule itself needs a manual disable/re-enable in Kodi to actually
recover -- see `docs/OPEN_ITEMS.md` for the fuller account of both
gaps.

## The timezone problem, and why it needed a user-facing setting

Confirmed against Dispatcharr's own source (`sync_recurring_rule_impl`
in `apps/channels/tasks.py`), not assumed: a rule's `start_time`/
`end_time` are naive `"HH:MM:SS"` values with **no timezone attached**,
combined with each target date using Dispatcharr's own configured
system timezone CoreSetting (`CoreSettings.get_system_time_zone()`,
readable via `GET /api/core/settings/`'s `system_settings.time_zone`
value -- a plain IANA zone name string, confirmed live against a real
instance). There is no server-side normalization at all
-- the `RecurringRecordingRuleSerializer` only does naive
same-timezone comparisons for validation ordering.

This addon operates entirely in UTC internally (every other timestamp
it handles is UTC, confirmed consistent throughout its whole history),
and doesn't bundle a real IANA timezone database -- adding one for
correct automatic DST-aware conversion across Windows/macOS/CoreELEC
would be a large, disproportionate dependency for this one feature.
Given the real, practical alternative -- **refuse to create a recurring
rule at all unless Dispatcharr's configured system timezone happens to
be UTC** -- would have blocked the feature outright for a real
instance confirmed configured to a non-UTC zone, the
setting `recurring_rule_utc_offset_minutes` (default `0`) was added
instead: a plain manual UTC-offset the user sets to match Dispatcharr's
*current* offset, used to shift Kodi's UTC-based timer start/end
time-of-day into Dispatcharr's own wall-clock convention before sending
it, and shift back the same way when reading rules back for display.
Deliberately simple, not a real timezone library: a zone that observes
DST needs this setting flipped by 60 minutes twice a year, or a
recurring timer will fire an hour off until it's updated -- explained
directly in the setting's own help text.

The conversion math itself needs no timezone library either way: every
value that actually matters here (Kodi's `GetStartTime()`/
`GetEndTime()`/`GetFirstDay()`) is already a UTC `time_t`, and a UTC
`time_t`'s own modulo-86400 gives an exact, DST-free calendar-day/
time-of-day split with no `gmtime`/`timegm` round-trip needed -- the
place a real timezone enters is the explicit, user-supplied offset shift
(plus, since 2026-09-30, Kodi's own local offset, for the weekday list and
first day only -- see "Weekdays and first day are read in Kodi's own
calendar" at the end of this file).

## Confirmed live, end-to-end

Bypassed Kodi's own timer-creation GUI for the create/read verification
(Kodi's JSON-RPC `PVR.AddTimer` only supports the "record from EPG
broadcast" flow, confirmed via `JSONRPC.Introspect` -- no field-level
control over weekdays/times/channel for a manual recurring rule), so
this addon's own client-side conversion math was verified independently
against a real API call using the exact values it would compute, then
against the addon's actual `GetTimers()` output over JSON-RPC once the
rule existed server-side:

1. Directly created a real rule via the API (channel with a real,
   working stream and real EPG; a single weekday bit set, with the
   local `start_time`/`end_time` deliberately chosen a couple of
   minutes apart, matching what this
   addon's own code would compute for the equivalent Kodi timer,
   `recurring_rule_utc_offset_minutes` set to a representative
   DST-observing test offset). Confirmed the rule's own `perform_create`
   immediately materialized a real `Recording` (not waiting on the hourly
   task) at exactly the UTC instant the offset math predicts for that
   local start time, confirming the conversion and Dispatcharr's own
   interpretation of it match.
2. Restarted Kodi and read the rule back through the addon's actual
   `GetTimers()` over JSON-RPC: `weekdays` correctly reported the one
   weekday set (confirms the bitmask round-trip), `starttime`/`endtime`
   matching the exact UTC values from step 1 (this addon's own reverse
   conversion, independently reproducing them), `firstday` matching the
   expected calendar date,
   `istimerrule: true` for the parent (`timerid 2`) and
   `istimerrule: false` for its child instance (`timerid 3`, correctly
   showing `state: "recording"` once its start time arrived -- also
   confirmed via a real-time-updates `recording_started` event in
   `kodi.log` at that same local start time).
3. Deleted the parent timer via `PVR.DeleteTimer` while its child
   instance was actively recording: the rule and Kodi's own parent
   timer entry disappeared, but the still-recording child correctly
   remained (Dispatcharr's own purge only removes *future*
   recordings) -- it went on to finish normally
   (`status: "completed"`, `remux_success: true`, real bytes written)
   as a genuine, playable recording, confirming the whole pipeline
   works for real, not just at the metadata level.

Cleaned up all test rules/recordings afterward.

## Auto-computing the offset for common zones, instead of asking for it

The manual `recurring_rule_utc_offset_minutes` setting above works, but has
an obvious flaw a user pointed out directly: if it's correctly set, it
should always equal whatever Dispatcharr's own configured timezone
currently is -- there's no legitimate reason for it to differ, and if it
drifts (most commonly: a DST-observing zone that needs flipping twice a
year, and nobody remembers to), recurring timers silently fire at the
wrong wall-clock time with no error surfaced anywhere.

**Step one, surfacing the zone name.** Dispatcharr's configured system
timezone is readable as a plain IANA zone name via
`GET /api/core/settings/`'s `system_settings.time_zone` field (confirmed
live against a real instance) -- this was already true, just not read
by this addon. Added `DispatcharrClient::GetSystemTimeZone()` (generalizing the
existing single-purpose `FindDvrSettingsRow()` into
`FindCoreSettingsRow(key, ...)` so both the DVR-padding row and this new
`system_settings` row share the same lookup) and a new read-only
`dispatcharr_timezone_info` setting, synced on every restart the same
"only rewrite if actually different" way the DVR padding settings already
work. Doesn't solve the drift problem by itself -- just gives a concrete
reference instead of making the user separately check Dispatcharr's own
admin panel -- but is a genuine, if small, improvement on its own.

**Step two, actually computing the offset.** A zone *name* alone doesn't
give a numeric offset without real DST transition rules, which is exactly
why a full IANA timezone database was ruled out for this addon to bundle.
But DST rules for a handful of common zones are simple, stable, and
well-documented enough to hardcode directly:

- **US/Canada** (Energy Policy Act of 2005, in effect since 2007): starts
  2:00am local *standard* time on the 2nd Sunday of March, ends 2:00am
  local *daylight* time on the 1st Sunday of November.
- **UK/EU**: starts 01:00 UTC on the last Sunday of March, ends 01:00 UTC
  on the last Sunday of October -- defined directly in UTC, no per-zone
  offset needed for the transition moments themselves, unlike the
  US/Canada rule.

Implemented as `DispatcharrClient::ComputeKnownZoneOffsetMinutes()` --
pure UTC `time_t` arithmetic (nth-weekday-of-month / last-weekday-of-month
helpers, reusing the existing `PortableTimeGm()` cross-platform helper, no
platform timezone API or bundled database involved at all), covering 25
zones (US Eastern/Central/Mountain/Pacific/Alaska + Arizona/Hawaii's
no-DST cases, five Canadian zones, UK/Ireland, six Central European zones,
three Eastern European zones, plus UTC itself). Deliberately excludes
Southern Hemisphere zones (Australia's DST runs the opposite direction,
October-April, with its own date rules) and anywhere else not on this
list -- out of scope for this pass, falls back to the existing manual
entry.

**Verified independently before trusting it against real timers**: the
exact same nth-weekday/last-weekday/UTC-conversion algorithm was
reimplemented in Python (using its trusted stdlib `datetime`, not this
addon's own logic, as the independent check) and run against published,
verifiable US and EU DST transition dates for 2025-2027 -- every single
computed date matched exactly. Also cross-checked live against a real
instance's own configured zone: computing that zone's offset for
"right now" matched the real, already-known-correct value already
configured there.

**A real correctness bug found and fixed before it ever shipped as
final**: the first version of this computed the offset once at addon
*startup* and wrote it into `recurring_rule_utc_offset_minutes` as a plain
number -- self-healing on every restart, but stale for anyone who leaves
Kodi running continuously across an actual DST transition (rare, but a
real gap, not hypothetical: Kodi is exactly the kind of app people leave
running for weeks). Fixed by not caching a *computed offset* at all --
instead a new `recurring_rule_timezone` setting stores the *zone name*
(auto-selected to match Dispatcharr's own reported zone when it's one of
the 25 known ones, "manual" otherwise), and
`PVRDispatcharr::EffectiveRecurringRuleUtcOffsetMinutes()` calls
`ComputeKnownZoneOffsetMinutes()` fresh at the two actual points the
offset is used (`GetTimers()`'s read-back, and
`ComputeRecurringRuleFields()`'s create/update path) rather than once at
construction. Deliberately not cached in a member either, unlike most
other settings this class caches in an `std::atomic` -- both call sites
are synchronous PVR callbacks on Kodi's own thread, not a background
polling loop, so there's no thread-safety reason to cache it the way
`m_recordingRefreshMinutes` needs to for its own thread.

Confirmed live end-to-end after the fix: deliberately set the manual
offset to a wrong value (`0`) with `recurring_rule_timezone` still on
a real instance's own known zone, restarted, and confirmed via
`kodi.log` it corrected back to reading through the zone-based
computation rather than trusting the stale manual value -- a log line
of the form `setting recurring_rule_timezone=<zone> (known zone)`
confirms the sync fired and the dropdown, not the manual number, is
what's actually authoritative for a known zone.

**Update (2026-09-09): broadened from 25 to ~50 known zones, and an
unrelated dropdown regression fixed along the way.**
Checked against Dispatcharr's real `GET /api/core/timezones/` (see
`docs/API_NOTES.md`) confirmed the practical ceiling is still DST *rule*
coverage, not the zone name list -- so this stayed within the same two
hand-verified rule families (`kUsCanada`, `kEu`) plus confirmed no-DST
zones, rather than claiming the full ~440-zone list is usable. Added:
three more US zones (Detroit, Indiana/Indianapolis, Boise), one more
Canadian no-DST zone (Regina/Saskatchewan), Mexico City (DST abolished
nationally in 2022, now fixed offset), nine more `kEu`-family zones
across Western/Central Europe, four more in Eastern Europe, and seven
fixed-offset Asia/Africa zones with no DST at all. Still deliberately
excludes Southern Hemisphere zones for the same reason as the original
25 -- their DST runs the opposite calendar direction, and neither coded
rule engine models that.

While re-reading this exact table, found a real, unrelated regression:
a dropdown entry in `kKnownTimeZones`/`settings.xml`/`strings.po` had
been accidentally broken in three functional places, silently breaking
DST auto-detection for that zone. Restored.
Confirmed via code review that all three functional locations show the correct entry again, and via a restart that `recurring_rule_timezone` auto-detection works correctly for known zones generally.

**Update (2026-09-30): older spellings of those zones are recognized too.**
Dispatcharr's settings page fills its timezone picker from the browser's
`Intl.supportedValuesOf('timeZone')`, and Chromium's engine still lists
the pre-rename ids -- checked against a real V8 (Node 22, ICU 78): it
offers `Asia/Calcutta`, `Europe/Kiev` and `America/Indianapolis`, and none
of `Asia/Kolkata`, `Europe/Kyiv` or `America/Indiana/Indianapolis` (every
other table zone is offered under its table name). Picking India, Ukraine
or Indianapolis in Chrome therefore stored a name the exact-match lookup
rejected, silently leaving recurring timers on the manual offset. An alias
table in `TimeZoneUtil.cpp` (those three plus IANA's `backward` links to
table zones: `US/*`, `Canada/*`, the old country-name ids) and
`CanonicalKnownZoneName()` now map them onto the table;
`ComputeKnownZoneOffsetMinutes()` accepts an alias, and
`SyncTimezoneFromDispatcharr()` selects the canonical name in the dropdown
(which only lists the modern one) while `dispatcharr_timezone_info` keeps
showing what Dispatcharr actually reports. Confirmed live with a forwarder
rewriting the real instance's reported zone to `Asia/Calcutta`: the addon
logged `setting recurring_rule_timezone=Asia/Kolkata (known zone)`.

Also added `DispatcharrClient::GetSupportedTimezones()` (the same real
endpoint), used only in the startup diagnostic when a zone is genuinely
unrecognized and debug logging is on -- distinguishes "a real IANA
zone, no DST rule for it yet" from "not a recognized zone at all" in
the log line, rather than one generic "unrecognized" message for both.
Confirmed live via a temporary forced-unknown test: correctly
identified the real instance's actually-configured zone as present in
Dispatcharr's real ~440-zone list even while its own DST-family lookup
was artificially forced to fail, proving the endpoint call, auth, and
the differentiation logic all work end-to-end.

**Update (2026-09-26): `GetTimers()`'s own display path now resolves
DST at the rule's own `startDate`, not "now" -- a real, confirmed
display bug found via a project-wide review, not itself independently
reproduced.** `EffectiveRecurringRuleUtcOffsetMinutes()` (as described
above) calls `ComputeKnownZoneOffsetMinutes()` fresh at its two call
sites rather than caching a computed offset -- correct for
`AddTimer()`/`UpdateTimer()`'s create/edit direction, where Kodi's own
start-time date is normally today or the near future, so "now"'s DST
state is the right one. But `GetTimers()`'s own display direction fed
that same "now"-based offset into `ComputeRecurringRuleDisplayTimes()`
alongside `rule.startDate` -- which can be many months old, potentially
on the *other side* of a DST transition from today. For a rule created
back when DST was in a different state than it is now, this rendered
the wrong wall-clock hour (typically off by 60 minutes) in Kodi's
Timers list for roughly half the year -- display-only, since the actual
recording schedule was computed once, correctly, at create/edit time
via the same function's own "now" default. Fixed by giving
`EffectiveRecurringRuleUtcOffsetMinutes()` an explicit `at` parameter
(defaulting to `time(nullptr)`, unchanged for the create/edit call
site) and having `GetTimers()` pass `rule.startDate` explicitly instead.

**Update (2026-09-26, later the same day): the above introduced a real
regression, caught within a later pass of the same project-wide review
before it was reported as a separate incident.** The "display-only,
unaffected" claim two paragraphs up stopped being true the moment
`GetTimers()`'s own offset diverged from `AddTimer()`/`UpdateTimer()`'s:
`UpdateTimer()` receives back exactly the `startTime`/`endTime`
`GetTimers()` just displayed (unchanged on an edit that doesn't touch
the time fields at all, e.g. toggling enabled/disabled), and re-derives
`startSecondsOut`/`endSecondsOut` from whatever offset it resolves --
still "now", unchanged by the fix above. Whenever `rule.startDate` and
"now" fell on opposite sides of a DST transition, the two different
offsets applied on each leg of the round trip didn't cancel out,
silently shifting the rule's actual recording time by the DST delta on
every such edit -- worse than the display bug being fixed, since this
one *does* touch the real schedule. Fixed by having
`ComputeRecurringRuleFields()`'s own wrapper resolve the offset at
`timer.GetStartTime()` instead of relying on the "now" default --
correct for `UpdateTimer()` (matches what `GetTimers()` used to produce
that same value) and at least as correct as "now" for `AddTimer()` (a
genuinely new rule's own start time is normally today or the near
future anyway).

A second, independent bug surfaced by the same later review, also
fixed alongside: `GetTimers()`'s `SetFirstDay(rule.startDate)` passed
`startDate` (Dispatcharr's own already-offset-shifted UTC-midnight
value) straight through unshifted. Whatever Kodi echoes back as
`timer.GetFirstDay()` on a later edit feeds straight into
`ComputeRecurringRuleFields()`'s own `firstDay` parameter, which applies
its own `+ offsetSeconds` shift again -- a *second* application of the
same shift. For a zone behind UTC specifically, `utcMidnight()` of an
already-exact-midnight value plus a negative sub-day offset always
floors to the *previous* day, so every single edit (including a bare
enabled/disabled toggle) silently moved the rule's own start date back
one full day, compounding further on each additional edit -- present
since this addon first supported recurring rules, not something either
DST fix above introduced. `ComputeRecurringRuleDisplayTimes()` now also
returns a `firstDayOut` (startDate shifted back by the same offset
already applied to `startTimeOut`/`endTimeOut`), which `GetTimers()`
passes to `SetFirstDay()` instead -- re-applying
`ComputeRecurringRuleFields()`'s forward shift to it reconstructs
`startDate` exactly, closing the round trip. New
`tests/test_recurring_rule_util.cpp` cases exercise the exact
edit-echoes-a-value-back round trip both fixes now correctly survive,
including the specific negative-offset day-drift scenario. Neither was
independently reproduced live.

**Update (2026-09-26, a later pass of the same project-wide review):
never-renew-an-expired-rule (above, in the "rolling `end_date` window"
section) opened a narrow gap of its own, now closed.** A rule left
disabled long enough for its existing `end_date` to pass, then
re-enabled from Kodi, stayed enabled with an `end_date` already in the
past -- `UpdateRecurringRule()`'s own PATCH deliberately omits
`end_date` unless the edit's patch carries one (adoption, or the rolling-window end date an
open-ended rule needs; `RecurringRuleEdit.h`) (so an ordinary edit doesn't need a separate fetch to
preserve it), and the periodic renewal loop now correctly refuses to
touch an already-expired one, since it can't tell that case apart from
a rule a user created directly via Dispatcharr's own web UI with a
genuine, intentionally-finite `end_date`. Fixed by having `UpdateTimer()`'s
own recurring-rule branch check, on every edit that (re-)enables a
rule, whether its own cached `end_date` needs restoring (new
`dispatcharr::ShouldExtendRecurringRuleEndDateOnUpdate()`,
`RecurringRuleRenewal.{h,cpp}`) and issuing the same
`ExtendRecurringRuleEndDate()` PATCH the renewal loop itself uses if so
-- the explicit re-enable action is the signal here, not a background
guess, so this doesn't need (and doesn't apply) the occurrence-safety
checks the periodic version needs. Same tradeoff as the fix it
complements: re-enabling a deliberately finite, already-expired web-UI
rule from Kodi also extends it now, arguably the right behavior for an
explicit user action either way. Found via a project-wide review, not
itself independently reproduced; needs a live check on whether
Dispatcharr's own serializer would even have accepted an enabled rule
with a past `end_date` in the first place (if it rejects that PATCH
outright, the previous bug surfaced as a failed toggle rather than a
silently-stuck rule -- still broken, just differently).

**Update (2026-09-26, a later pass again): the "same tradeoff" call
above was actually broader than intended, found via a project-wide
review, not itself independently reproduced.** The fix only checked the
*post-edit* `enabled` value, not whether the rule was actually
transitioning from disabled to enabled -- so it fired on *any* edit of
an already-enabled rule (a rename, a schedule tweak), not just an
explicit re-enable. That reopened the "don't resurrect an expired rule"
protection this whole section is about, through an edit that had
nothing to do with enabling anything: a deliberately-finite,
already-expired, still-enabled rule (the exact case that fix exists to
leave alone) would get its `end_date` silently pushed forward by an
unrelated edit. `ShouldExtendRecurringRuleEndDateOnUpdate()` now also
takes the rule's own cached `enabled` from *before* the edit
(`wasEnabled`) and only restores `end_date` on a genuine
`wasEnabled=false -> enabled=true` transition.

**Update (2026-09-26, yet another pass of the same review): the fix
still didn't guard against a genuinely open-ended rule.** A zero or
unparseable `end_date` (a null value, or one `TimeFromDateString()`
couldn't parse) parses to `endDate = 0` -- the same sentinel
`ShouldRenewRecurringRule()`'s own already-expired-rule check already
relies on to leave a genuinely open-ended rule alone. But
`ShouldExtendRecurringRuleEndDateOnUpdate()` had no equivalent guard:
`(0 - now)` is a large negative number, which passed the "less than
half the window remains" check just like a genuinely expired rule
would -- re-enabling an intentionally permanent, open-ended rule from
Kodi would silently give it a brand-new finite `end_date`, converting
"records forever" into "records only while this addon keeps running to
periodically renew it." `UpdateRecurringRule()`'s own PATCH already
never touches `end_date` on its own, so simply not extending in this
case leaves an open-ended rule exactly as it was. Found via a
project-wide review, not itself independently reproduced -- needs a
null/open-ended `end_date` to actually exist server-side, which
`CreateRecurringRule()`'s own comment notes Dispatcharr's serializer
requires on *create* (though the model itself declares it nullable, so
a rule reaching this addon via a different path isn't ruled out).

**Update (2026-09-26, a 20th-pass audit): "leaves an open-ended rule
exactly as it was" above is itself wrong for such a rule, confirmed
against Dispatcharr's own real current upstream source (cloned into a
scratchpad, never committed to this repo -- stronger than the API
shape alone, not the same standard as a live test).**
`RecurringRecordingRuleSerializer.validate()` raises "End date is
required" whenever *neither* the PATCH body *nor* the existing instance
has an `end_date` -- so `UpdateRecurringRule()`'s own omit-`end_date`
PATCH doesn't leave a genuinely null-`end_date` rule "exactly as it
was" at all; it fails the entire edit outright with a 400, including a
bare enable/disable toggle or a rename that never touched scheduling.
Not fixed this pass: this needs a real null-`end_date` rule to actually
exist first, which -- per the paragraph above -- can't happen through
this addon's own `CreateRecurringRule()` (Dispatcharr's serializer
requires one on create), only through a legacy row or one created
another way entirely (Django admin/shell, a schema predating this
validation). Logged to `docs/OPEN_ITEMS.md` pending confirmation such a
rule can genuinely reach this addon in practice.

**Update (2026-09-26, a later pass of the same project-wide review): a
rule with no name could have that name permanently overwritten by an
unrelated edit, found via a project-wide review, not itself
independently reproduced.** `GetTimers()` falls back to a synthesized
"Recurring recording `<id>`" placeholder title when a rule's own `name`
is empty (e.g. a rule created directly via Dispatcharr's own web UI
with no name at all) -- the exact same shape as the one-time-recording
"Recording `<id>`" placeholder problem `ShouldRenameOnTimerEdit()`
(`TimerIdentity.h`) already guards against, but this branch never got
the equivalent guard when that fix was added. Unlike a one-time
recording, `UpdateRecurringRule()`'s single PATCH has no separate
"don't touch name" option -- it always sends whatever `name` value it's
given -- so any edit that doesn't actually rename anything (including a
bare enable/disable toggle, which Kodi implements as `UpdateTimer()`
with every other field, including the displayed placeholder title,
echoed back unchanged) sent that placeholder straight back as the
rule's own new, permanent name, silently overwriting an intentionally-
empty one server-side. Fixed by new
`dispatcharr::ResolveRecurringRuleNameForUpdate()` (`TimerIdentity.h`):
resolves the actual value to send as `name` -- the Kodi title unless
it's exactly this rule's own placeholder, in which case the rule's
cached (possibly still-empty) real name is sent instead.

**An overnight recurring rule (e.g. 23:00-01:00) could display an end
time before its start time in Kodi, found via a project-wide review and
confirmed by an 18th-pass audit (2026-09-26) that cloned Dispatcharr's
own real current upstream source into a scratchpad, never committed to
this repo.** `PVRDispatcharr::GetTimers()` computes a recurring rule's
display start/end as `rule.startDate + {start,end}TimeOfDaySeconds -
offsetSeconds` -- for a 23:00-01:00 rule, `endTimeOfDaySeconds` (01:00,
i.e. 3600) is numerically less than `startTimeOfDaySeconds` (23:00,
i.e. 82800), so the computed end time landed before the start time.
This is reachable from Kodi's own timer dialog directly, not a
hypothetical input -- `ComputeRecurringRuleFields()`'s own create path
already passes such a rule through unchanged. Confirmed Dispatcharr's
own scheduler already treats this as crossing midnight
(`sync_recurring_rule_impl()`, `apps/channels/tasks.py`: `if end_dt <=
start_dt: end_dt += timedelta(days=1)`), so `ComputeRecurringRuleDisplayTimes()`
now applies the same check and rolls the displayed end time forward a
day when it fires -- matching what Dispatcharr's own scheduler actually
does, not just a display-only guess.

**Update (2026-09-26, a 19th-pass audit): a single missed occurrence
could permanently stop a rolling rule from ever renewing again, found
via a project-wide review, not itself independently reproduced.** If
Dispatcharr (or its Celery worker) is down for the entire window of one
recurring occurrence -- an overnight NAS reboot, say -- that occurrence's
`custom_properties.status` stays `"scheduled"` forever, with both its
`start_time` and `end_time` now in the past (confirmed against
Dispatcharr's own real upstream source: `purge_recurring_rule_impl` only
deletes rows with `start_time__gte=now`, and `recover_recordings_on_startup`
only resumes/finalizes rows still in-window or with `status=="recording"`
-- a stale `"scheduled"` row is untouched by either). `ParseRecordingFields()`
correctly treats `status=="scheduled"` as authoritatively upcoming (see
above), but `ShouldRenewRecurringRule()`'s own "occurrence currently
recording or starting soon" check only looked at `rec.startTime - now <
safetyMarginSeconds` -- a large negative number for a missed occurrence,
always less than the margin, so it read as permanently "imminent" and
silently blocked this rule from renewing on every single cycle, right up
until its own `end_date` ran out -- at which point the (separate, correct)
already-expired-rule check started refusing it too, ending the rule's
recording life early with no error anywhere. Fixed by also requiring
`rec.endTime > now` before treating an occurrence as blocking renewal.

**Update (2026-09-26, a 19th-pass audit): editing a recurring rule's
First Day forward past its own current `end_date` failed outright,
confirmed against Dispatcharr's own real upstream source, not itself
independently reproduced.** `UpdateRecurringRule()`'s own PATCH
deliberately omits `end_date` for an ordinary edit (an edit patch carries one only for adoption or
an open-ended rule, `RecurringRuleEdit.h`; see its own comment above), relying on
`RecurringRecordingRuleSerializer`'s partial-update fallback to the
instance's *existing* value -- but that same `validate()` rejects
`end_date < start_date`, and, for an overnight rule where the two dates
land equal, `combine(end_date, end_time) <= combine(start_date,
start_time)` too. Kodi's own First Day picker offers dates up to a year
out, while a renewed rule's own cached `end_date` sits at most
`kRecurringRuleWindowDays` out -- so simply moving First Day forward
past (or, for an overnight rule, onto) the rule's own current `end_date`
failed this edit with a 400, entirely independent of the
already-disabled->enabled-transition fix above (this can happen to an
already-*enabled* rule on a plain schedule edit). Fixed by new
`dispatcharr::ComputeRecurringRuleEndDateForStartDateChange()`
(`RecurringRuleRenewal.h`): `UpdateTimer()`'s recurring-rule branch now
extends `end_date` first, ahead of the main PATCH, whenever the new
First Day would reach or pass it.

**Update (2026-09-27, a 47th-pass audit): the timezone sync only ran
once, at construction, with no retry on failure, found via a
project-wide review, not itself independently reproduced.** The
constructor's own sync of `recurring_rule_timezone`/`m_recurringRuleUtcOffsetMinutes`
from Dispatcharr's real configured system timezone (see "Auto-computing
the offset for common zones" above) only ever ran once, and simply
logged a debug message on failure with no retry -- the same
startup-timing class of gap already fixed elsewhere in this codebase
(channels/EPG, the API key). If Dispatcharr isn't reachable yet at Kodi
startup (a slower-booting server, or a network path that comes up after
Kodi does), `recurring_rule_timezone` stayed at its `"manual"` default
with a `0` offset for the entire session -- silently scheduling every
recurring rule created or edited that session at the wrong real-world
time, until a later restart happened to reach the server. Fixed by
extracting the sync into its own method, `SyncTimezoneFromDispatcharr()`,
and having `StartChannelEpgRefreshThread()`'s own background loop retry
it once per cycle for as long as it hasn't succeeded yet -- cheap once
synced (the sync itself isn't attempted again at all once it succeeds),
so a transient startup failure now only costs one refresh cycle instead
of the rest of the session.

**Update (2026-09-27, a 48th-pass audit): the retry the fix above added
made a real, confirmed scheduling bug reachable that couldn't happen
before, found via a project-wide review, confirmed against this
addon's own real current source, not itself independently reproduced.**
Before that fix, the timezone sync could only ever run once, at
construction, before Kodi had loaded any timers at all. Once the retry
let it succeed *later* -- possibly well after Kodi has already fetched
and cached recurring-rule times computed with the *old* offset --
nothing told Kodi to re-fetch them with the new one. `GetTimers()`'s own
`ComputeRecurringRuleDisplayTimes()` (`RecurringRuleUtil.cpp`) subtracts
the offset in effect *at display time* to build the time Kodi caches and
shows; `UpdateTimer()`'s own `ComputeRecurringRuleFields()` adds back
whatever offset is in effect *at edit time* to convert that same cached,
possibly-stale-offset time back to Dispatcharr's own representation. If
the offset changed in between -- even from an edit as small as a plain
enable/disable toggle, which still resends `start_time`/`end_time`/
`start_date` (`BuildRecurringRuleUpdateBody()`, `TimerRequestBuilder.cpp`)
-- the round trip shifts the rule by the full difference between the old
and new offset, not by nothing (e.g. offset 0 -> America/New_York moves
a 20:00 rule to 16:00). Fixed by having `SyncTimezoneFromDispatcharr()`
report whether it actually changed `recurring_rule_timezone`, and having
the background thread's own retry (not the constructor's first call,
which has nothing loaded yet for a trigger to matter for) call
`InvalidateAndTriggerTimerUpdate()` when it does, so Kodi's own cached
display times catch up to the new offset before any edit can read a
stale one back.

**Update (2026-09-27, a 49th-pass audit): the 48th-pass fix above only
covered the automatic (Dispatcharr-timezone-sync) path, not a manual
change made directly through this addon's own settings dialog, found via
a project-wide review, not itself independently reproduced.** A user
editing `recurring_rule_utc_offset_minutes` directly (the "manual"
override path -- see "The manual `recurring_rule_utc_offset_minutes`
setting" above) or switching `recurring_rule_timezone` between two known
zones has exactly the same stale-cached-display-time exposure
`SyncTimezoneFromDispatcharr()`'s own retry was fixed for, just reached
through `OnAddonSettingChanged()` instead of the background sync
thread -- and that path had no trigger at all. Fixed with two new
branches in `OnAddonSettingChanged()`, one per setting, each comparing
the newly-delivered value against what's actually in effect
(`m_recurringRuleUtcOffsetMinutes.exchange()`'s own previous value for
the offset; a new `m_lastAppliedRecurringRuleTimezone` member,
constructor-initialized from the setting's own current value, for the
zone name) and calling `InvalidateAndTriggerTimerUpdate()` only on a
genuine change -- avoiding a needless refresh on every settings-dialog
save that happens to re-deliver an unchanged value, the same
spurious-renotification concern `m_lastAppliedConfig`'s own comment
already documents elsewhere in this codebase.

## Weekdays and first day are read in Kodi's own calendar

(Fixed 2026-09-30, the full account of why and how; `docs/OPEN_ITEMS.md`
has the short entry and its live-verification record.)

Everything above treats Kodi's times as UTC and only bridges to
*Dispatcharr's* zone. That is right for start and end time, but a repeating
timer's weekday list and first day are not UTC values: Kodi's timer dialog
(`GUIDialogPVRTimerSettings.cpp`) sets the weekday bitmask straight from
its checkboxes with no conversion, and first day is only ever edited as a
*date* -- `SetDateFromIndex()` swaps the date part of the local first-day
value and leaves the time of day it already had, so that time is
meaningless. Together with a start time shown on the local clock, a
repeating timer means "these weekdays by the local clock, at this local
time, from this local date." Dispatcharr's `days_of_week` and `start_date`
mean the same thing in *its* zone. The two agree on which day an occurrence
falls on unless the occurrence's UTC time of day `U` lands on different
sides of midnight in the two zones, where Dispatcharr's date is
`floor((U + offD) / 86400) - floor((U + offK) / 86400)` days later than
Kodi's. That is independent of the date -- the same time of day is a fixed
number of calendar days apart in two fixed-offset zones on any date -- which
is what lets a single rotation translate a whole weekday list, and
`start_date` is Kodi's first-day *date* moved by the same number.
Everything is identical whenever the two zones agree, so the common
same-zone setup is untouched. For a server left on UTC and a Kodi in the
Americas every evening rule is affected: 9 PM in US Eastern summer time is
01:00 UTC the next morning, and "Monday to Friday" used to reach Dispatcharr as Monday to
Friday *mornings UTC*, i.e. Sunday to Thursday evenings.

**Kodi's offset is per instant, not one bias.** Kodi converts each timer
time with libc's `localtime()` of the instant in question
(`CPVRTimerInfoTag::ConvertUTCToLocalTime()`/`ConvertLocalTimeToUTC()`), so it
follows daylight saving per date exactly like Dispatcharr's own zone does --
confirmed live, a rule with a start date in the other daylight-saving period is shown at its real local
time today. (`CDateTime::GetTimezoneBias()`, a single cached bias, does
exist and looks like the thing to mirror, but timer conversion doesn't use
it; a first version of this fix read the offset once at "now" on that
assumption and rotated the weekdays of a same-zone Kodi whose rule started in
the other daylight-saving period.) So the addon asks for both zones' offsets
at the rule's own start instant, which is the instant the translated time of
day describes (`TimeUtil.h`'s `LocalUtcOffsetMinutes(at)` for Kodi's side), and
for Kodi's offset at first day's own instant when reading its date. Across a
transition in either zone the true shift can change for a rule near midnight;
a single weekday list cannot express that, in Kodi or in Dispatcharr, and the
rule is translated as of its first occurrence. First day is named to Kodi
as local *noon* of the right date rather than midnight, so the date survives a
transition that day and a local midnight that doesn't exist or happens twice.

Two live-testing notes that are not obvious. Kodi's JSON-RPC cannot edit a
timer (`PVR.ToggleTimer` and `PVR.AddTimer` both take a *broadcast*), so
the edit direction can only be exercised through Kodi's own Timers dialog:
`GUI.ActivateWindow` `tvtimerrules`, then `Input.ExecuteAction`
(`select`, `down`, `right`, `contextmenu`, `number0`-`number9`) and
`Input.SendText` for the name, reading where focus is with
`GUI.GetProperties` `currentwindow`/`currentcontrol`. A screenshot is
`Input.ExecuteAction` `screenshot` after pointing `debug.screenshotpath` at
an existing directory under `special://temp` (GNOME's own screenshot D-Bus
call refuses an SSH session). Selecting a rule in that list opens its
occurrences, not its settings; `contextmenu` -> Edit does. In the dialog
`select` on the First day row advances it by a day (`right` there moves
focus to the OK button rather than changing the value), a time row opens a
numeric pad that takes `number0`-`number9` in 24-hour HH:MM and closes with
`select` on Done, and `right` then `select` from a row presses OK. And
Kodi's zone can be changed while it is stopped by editing `locale.timezone`
and `locale.timezonecountry` in `guisettings.xml` (a Flatpak `TZ` variable is
overridden by that setting).

## Edits send only what changed; an open-ended rule gets an end date (2026-10-02)

Two changes to `UpdateTimer()`'s recurring-rule branch, both confirmed live (see `docs/OPEN_ITEMS.md`, "Recurring-rule edits send back Kodi's possibly-stale cached fields" and "Recurring rule with a null end_date can't be edited").

**Only what the user changed is PATCHed.** `ComputeRecurringRuleEditPatch()` (`RecurringRuleEdit.h`) compares the saved fields with the rule as Kodi was last shown it and `BuildRecurringRuleUpdateBody()` sends just the differences; a save that changes nothing sends no request, which also spares the server from dropping and regenerating every future occurrence. The baseline is `m_reportedRecurringRules` -- the rules exactly as `GetTimers()` last handed them to Kodi -- and not the cached list: the cache is refreshed in the gap between Kodi filling its dialog and the user saving, and diffing against it treated a change made elsewhere in that gap (the web UI, a second Kodi) as the user's own and sent it back. That is the version that shipped first and failed the live test: a rule renamed and re-dayed from outside, then deactivated in Kodi, came back with its old name and days. With the reported snapshot the same sequence changes only `enabled`. Comparison is by value, with the weekdays as sets and the start date by calendar day, relying on the display-then-edit round trip being the identity (it is tested to be). A rule `GetTimers()` never reported (so not editable anyway) falls back to the cache, and a rule with no baseline at all sends every field as before. The rule is also re-read from the server on each edit (`GetRecurringRuleById()`), for its end date and to refuse an edit of a rule that no longer exists (404).

**An open-ended rule is given an end date so it can be edited at all.** Dispatcharr's serializer answers 400 "End date is required" to any save of a rule with no `end_date` that is not itself being given one -- even a bare enable/disable toggle -- and such a rule is reachable (a PATCH of `{"end_date": null}` is accepted, the create path refuses it). This corrects the earlier claim here that an edit "leaves an open-ended rule exactly as it was": it fails. `ComputeEndDateForOpenEndedRuleEdit()` adds a rolling window (`kRecurringRuleWindowDays`) counted from today, or from the start date when that is still ahead, so the end can never precede the start. Verified against the real server first (partial PATCH accepted and leaves the other fields alone; toggle without an end date 400; toggle with one 200), then live from Kodi: a rule stripped of its end date and Activated from the Timer rules window succeeded with the end date 30 days out. The cost is that "records until you stop it" becomes "records while a Kodi running this addon renews it".

## Ownership: which rules the addon renews, and reviving one that lapsed (2026-10-02)

The addon used to renew every enabled recurring rule it could see, including one made in Dispatcharr's web UI with a deliberate end date, and -- unable to tell them apart -- refused to extend any rule that had already expired, so a rule of its own that lapsed while no Kodi ran stayed dead. Ownership is now explicit (`ManagedRecurringRule.h`; `docs/OPEN_ITEMS.md`, "Can't tell addon-managed rolling rules..." and "Rolling recurring rules die..."):

- **The tag.** A rule has no free-form field, so ownership is a `[Kodi]` at the end of its name (`Evening News at 10pm [Kodi]`, or the bare tag for an empty name). `AddManagedRuleMarker()`/`StripManagedRuleMarker()`/`IsManagedRuleName()`. A rule created from Kodi gets it; Kodi is shown the name without it (the Timers list, the edit dialog -- confirmed live); a rename from Kodi puts it back, and a rule without the tag is not made owned by being edited. Only a tag at the end, as its own word, counts.
- **Renewal.** `ShouldRenewRecurringRule()` returns false for any rule without the tag. For an owned, enabled one it renews inside the window as before, and *revives* one past its end date (or with none) with a new window -- the occurrence-safety check still applies, and a disabled rule is left alone. Recordings missed while no Kodi ran are not recovered.
- **One-time tagging of existing rules.** Until now every rule was renewed, so the first run of a build with ownership tags the rules already there (`EvaluateInitialAdoption()`, state in `recurring_rule_adoption.json`): every untagged rule that has not run its course. An enabled rule already past its end date is the "deliberately finished" case and is not tagged; a rule with no end date is tagged and given one. Nothing is tagged when there are no rules (so a web-UI rule made later is never swept up) or when a tagged rule already exists (another install got there first). A rule with a recording running or about to start waits for a later cycle, because a rule PATCH makes the server regenerate its future occurrences; the pending list is persisted so a restart does not lose it. A notification tells the user once when rules were tagged.
- **Occurrence titles.** An occurrence takes its rule's name as its title, so the tag reached every timer and recording the rule generated (found live); `ParseRecordingFields()` strips it from a recording that belongs to a recurring rule. The folder and file Dispatcharr writes on the server are named from the rule name and still carry it.
- **Stopping an owned rule.** Disable or delete it, or remove the tag; lowering its end date in the web UI is undone at the next cycle.

## A rule typed across a clock change: Kodi converts both times the same way (2026-10-06)

`CPVRTimerInfoTag::ConvertLocalTimeToUTC()` (Kodi Omega, `PVRTimerInfoTag.cpp`; the timer dialog calls it on every save)
takes the daylight-saving flag from `localtime()` of the typed local fields **read as a UTC instant**, then calls `mktime()`.
In a zone west of UTC an early-morning time read that way falls on the previous evening, before a spring-forward, so for a
rule whose first occurrence spans the change (typed on a transition day, or overnight the evening before) both times arrive
with the same, pre-change flag: 01:30 to 03:30 on a spring-forward day arrives as 06:30Z to 08:30Z, and one offset undoes
both. `ComputeRecurringRuleFields()` therefore converts the end with the start's offset. A first attempt the same day gave
the end the offset at its own instant (the model "Kodi converts each instant with its own offset"): it stored 01:30-03:30
as 01:30-04:30, drifted an hour on every no-op edit (03:30, 04:30, 05:30 ...), stored a fall-back rule 00:30-03:00 as
00:30-02:00 and grew an overnight rule by an hour per edit; a review that simulated Kodi's real conversion against the real
code found it, and the tests now model that conversion (`test_recurring_rule_util.cpp`, POSIX only, a POSIX TZ string).
East of UTC Kodi's own start conversion is already an hour off for these times, so neither choice is right there.

## The zone table is checked against what the server actually scheduled (2026-10-06)

The built-in zone table (`TimeZoneUtil.cpp`) is the addon's own copy of when each zone changes its clocks, so it is
wrong wherever the server's tz data differs from it: a Dispatcharr image with tz data from before 2026c still changes
the clocks of British Columbia and Alberta on 2026-11-01, which the table (updated for the permanent daylight time those
two moved to) no longer does, and a rule made from the wrong offset records an hour off. The server's real offset does
not need a database to be found. A rule is stored as a local time of day, and every occurrence Dispatcharr materializes
from it carries the UTC instant the server computed, so the pair gives the offset the server applied that day
(`ServerOffsetCrossCheck.h`): a 19:30 rule whose occurrence starts at 10:30 UTC was scheduled under UTC+9. An
occurrence's start is the rule's own, not padded (`sync_recurring_rule_impl()` creates it straight from the rule's start and
end, confirmed against Dispatcharr's source, see the first section of `docs/RECORDINGS.md` on programme windows).

`GetTimers()` runs the check and uses the result in `EffectiveRecurringRuleUtcOffsetMinutes()`:

- **Samples are the six soonest occurrences that have not started** (`SelectOffsetSamples()`). A first version took the six
  nearest to now in either direction, and an occurrence from before the server's clock changed (which legitimately agrees
  with the table) then outvoted the ones after it: on 2026-11-01 a weekly rule would have kept the table in charge for
  about three weeks. An occurrence still to come is also what the server will record, so it is the offset a rule made or edited now
  gets, and a rule whose time was just edited does not have old occurrences in the future (the server regenerates them).
- A time of day fixes an offset only modulo a whole day (+13 h and -11 h read the same), so the offset is the one closest to
  the table's own answer, rounded to the nearest minute (an occurrence carries seconds the rule does not; truncating toward
  zero gave 59 for +60).
- **A sample implying more than an hour from the table is not evidence** (`kMaxPlausibleOffsetDeltaMinutes`): no clock
  difference between tz-data versions is larger, and a larger apparent one is a rule edited beside its old occurrences (a
  90-minute edit read as a -150 minute zone and would have moved every rule).
- A sample within a day of a clock change in the table is skipped: a local time that does not exist (a spring-forward gap)
  or exists twice (fall-back) is resolved by the server in a way that reads as the other offset with nothing wrong.
- Any sample that agrees with the table wins; disagreement needs every usable sample to name the same other offset against
  the same table offset. Mixed evidence leaves the table in charge.
- **What a disagreement means** (`JudgeServerOffset()`): when the zone's table with its recent rule change left out
  (`ComputeKnownZoneOffsetMinutes(..., applyZoneRuleChanges=false)`, today British Columbia and Alberta staying on daylight
  time) explains the samples, the server is applying the older rules and the addon follows that table instant by instant, so a
  rule for a date after the server's next clock change (March 2027) is converted correctly too. Any other difference is a
  constant delta, applied only where the table gives the offset the delta was measured against: one constant for every date
  would put a summer-dated rule an hour off.
- **An override takes two evaluations in a row** (`UpdateServerOffsetTracker()`): one bad snapshot (a stale cache holding a
  rule's new time beside its old occurrences, which differ by exactly an hour only by coincidence) must not move every
  rule. An agreeing table clears an override only when the agreement contradicts it: a server on a zone's older rules gives
  the same answer as the current table all summer (the zone stopped changing its clocks), so a legacy-zone override stays
  while the legacy table agrees too, and is cleared when the table agrees and the legacy one does not (the server updated);
  a constant difference stays until samples at the table offset it was measured against agree. A disagreement the legacy
  table cannot judge (every sample within a day of its own clock change) is no evidence rather than a guessed constant
  difference. No evidence leaves an override in force, and a change of the zone setting (or "manual") forgets everything
  learnt about the previous zone, so an override cannot carry over to another zone's table (the new zone's first
  judgement still counts).
- Only a table zone is checked (a "manual" offset is the user's own number). A disagreement logs a warning and queues a
  notification when an override comes into force or changes.

Not live-checked: a server whose tz data really disagrees with the table does not exist yet (the first day it can is
2026-11-01); the unit tests and the glue scenario `offset_cross_check` (a fake server in a fixed-offset zone whose occurrence
implies an hour less than the table) stand in for it. The scenario's rule and occurrence are dated from the clock, because the
check reads only occurrences that have not started, and its zone has no daylight saving so it does not depend on the time of year.
