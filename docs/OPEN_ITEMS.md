*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**5 open, 413 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One from the 2026-11-12 live checks on Kodi 21: timers made on the server showed about 75 s after the realtime event.

Nothing is left from the 2026-11-10 bug-hunting sweep, made on `Piers` and checked against `Omega`'s code (nine of its
eleven findings held here, the other two being `Piers` only; all fixed by 2026-11-12), nor from the 2026-10-18 read-only audit, made on `Piers` and checked against `Omega`'s code (all 21 findings
held here), from the 2026-10-19 live check of four audit fixes on `Piers`, from the 2026-10-23 sweep, made on `Piers`
and checked against `Omega`'s code (all 17 findings held here, the last fixed on 2026-11-04), from the 2026-10-24 sweep (one reviewer
over the whole project, seven findings), or from the 2026-10-31 sweep, made on `Piers` and checked against `Omega`'s
code (all 11 findings held here; nine fixed, two closed without a change, the last on 2026-11-04 by keeping a
recording's 20 s retry budget).

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 2 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 360 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 43 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

#### Kodi 21 showed timers made on the server about 75 s after the realtime event

**Opened 2026-11-12, seen once during the live check of the 2026-11-10 sweep's timer-id fix; not investigated.** On
Kodi 21.3 with the `Omega` CI build (realtime updates on, no proxy), a recurring rule and a one-off recording created
through Dispatcharr's API were followed within seconds by `recordings_refreshed` and `recording_updated` realtime
events, which the addon logged; `PVR.GetTimers` still had neither 15 s and 35 s later, and the addon's next "recordings
cache refreshed" and "timer-rules cache refreshed" lines came about 75 s after the events, after which Kodi listed
both. On Kodi 22.0-RC1 with the `Piers` build, the same steps showed the timers within 15 s. The handler calls
`InvalidateAndTriggerTimerUpdate()` (caches cleared, then `TriggerTimerUpdate()`), so the addon asked at once; the gap
is in when Kodi 21 next called `GetTimers()`. Unconfirmed lead: it was about a minute after Kodi started, while Kodi was
still importing the guide (its startup log has many `GetEPGForChannel` errors in
the same minute), so Kodi's PVR job queue may simply have been busy. Next step: repeat on Kodi 21 well after startup, with
Kodi's PVR component logging on (`debug.setextraloglevel`, LOGPVR) to see when the timer update job runs; if it is
prompt then, close this as a startup-load effect.

### Fix known, not yet done

None open.

### Architectural / concurrency

None open.

### Design decision needed

None open.

### Release, CI and manual testing

None open.

### Upstream (Dispatcharr) or documentation accuracy

#### Dispatcharr schedules a recurring rule's occurrence that starts inside a spring-forward gap to end before it starts

**Opened 2026-11-01, from the live check of the display item it closed (`CLOSED_ITEMS.md`, "A recurring rule starting
inside a spring-forward gap on its first day is shown ending before it starts"); confirmed against Dispatcharr 0.33.0.**
`sync_recurring_rule_impl()` (`apps/channels/tasks.py`) builds both times with `timezone.make_aware(datetime.combine(
...), tz)` on a `ZoneInfo` and rolls the end over only `if end_dt <= start_dt`. Two aware datetimes sharing one
`tzinfo` compare by wall clock, so a 02:30 that does not exist is still "before" 03:15 and nothing rolls over; stored,
02:30 takes the offset before the change. A disposable rule from 02:30 to 03:15 on the next spring-forward day of a
zone whose clocks go from 02:00 to 03:00 got an occurrence stored ending 15 minutes before it starts. What the recorder then
does with it
was not run (it is months away). Not this addon's bug, and only a rule whose start falls in the skipped hour on a
change day. Worth reporting to Dispatcharr: compare in UTC, or normalise the start out of the gap first.

#### Dispatcharr's own DVR, HDHR and M3U output follow a non-MPEG-TS Default Output Format

**Opened 2026-10-25, from the review of Dispatcharr 0.33.0; read in the `v0.33.0` source, not run.** For a request that
names no `output_format` and has no per-user override, `_resolve_output_format()` (`apps/proxy/live_proxy/views.py`)
falls back to Settings → Stream Settings → Default Output Format. Dispatcharr's own DVR builds its capture URL without
one (`_dvr_capture_url()`, `apps/channels/tasks.py`: `/proxy/ts/stream/<uuid>`, plus `?output_profile=` when set), the
HDHR lineup does the same (`apps/hdhr/api_views.py`), and the M3U output adds `output_format` to its stream URLs only
when the playlist request itself carried one (`apps/output/views.py`). So a server set to HLS hands its own recorder a
redirect to an HLS playlist and set to fMP4 hands it MP4 fragments; what ffmpeg then records, and what an HDHR client
makes of either, was not checked. Not this addon's bug: the addon and `timeshift_buffer` ask for `mpegts` since
`CLOSED_ITEMS.md`'s "Pin the live proxy's output format to MPEG-TS". Worth reporting to Dispatcharr once seen: on a
server nobody else uses (the lab's setting is shared with its other clients), set Default Output Format to HLS, record a
short programme, and check the file.

### Known gaps, deliberately deferred

#### Wake-on-LAN for a Dispatcharr host that sleeps

**Opened 2026-10-10, from the pvr.hts comparison; deferred.** pvr.hts sends a Wake-on-LAN packet to a configured MAC
address (`kodi::network::WakeOnLan`, part of Kodi's addon API) before its first connect, for a backend that sleeps when
idle. Cheap to add as an optional setting, but Dispatcharr is usually a container on an always-on host and nobody has
asked. Pick it up if a user reports a sleeping server; the whole change is the setting, one call before the first
request, and a note in the README.

#### Multiple Dispatcharr servers from one Kodi

**Opened 2026-10-10, from the pvr.hts comparison; deferred.** Kodi 20 and later let one PVR addon run several backend
instances (`instance-settings.xml` next to `settings.xml`); pvr.hts added it in 20.4.0 with a one-time migration of the
pre-instance settings in its `addon.cpp`. This addon is one instance, and `docs/API_NOTES.md`'s "Single-instance
assumption: partially hardened, not fully" section lists what else assumes that (the API-key persistence, the recurring
rule tag). Worth doing only if someone runs two Dispatcharr servers, or wants two accounts (admin for the companion
plugins, non-admin otherwise) against one server from one Kodi; the migration step and the per-instance settings
handling in `OnAddonSettingChanged()` are the work. Not started.

### Tooling (tools/)

None open.
