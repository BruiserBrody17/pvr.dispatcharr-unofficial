*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**23 open, 352 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the two
2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); one comes from the Kodi 22
work on `Piers`: what remains of Kodi 22 support. Nothing is left from the 2026-10-18 read-only audit of the whole
project (addon, both plugins, tools, CI and packaging; 28 findings: seven fixed the same day, 14 on 2026-10-19, four on
2026-10-21, one on 2026-10-22 and the last two closed without a change on 2026-10-22, all in `CLOSED_ITEMS.md`). The
other 17 come from the 2026-10-23 bug-hunting sweep of the whole project (five read-only reviews by area, each finding
checked against the code before it was filed): one MEDIUM, the rest LOW.
The last three (2026-10-24) are live checks from a review of Dispatcharr 0.33.0's changelog against the addon and both
plugins, read against the v0.33.0 source; the lab was still on 0.32.0.

| Section | Entries |
|---|---|
| Open: Needs a live check | 4 |
| Open: Fix known, not yet done | 11 |
| Open: Architectural / concurrency | 1 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 3 |
| Closed (in CLOSED_ITEMS.md): Fixed | 306 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 37 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### A sustained server outage may still end live playback with no notice

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible from reading.** `ReadLiveTimeshiftStream()`
gives up after its 25 s `kOutageReadBudget` and answers a plain 0, but the stream is only marked fatal (and
`NotifyLiveStreamFatalOnce()` only has something to say) once `HandleLiveTimeshiftSegmentFetchFailure()`'s streak passes
30 s, which one call cannot reach. A 0 sets end of file in Kodi (`CLOSED_ITEMS.md`, "A live stream marked fatal leaves
Kodi buffering instead of ending"), so a server that stays unreachable may end playback silently, the symptom the
2026-10-20 notice was added for. Not settled by reading: the 2026-10-02 measurement (a 45 s outage ended playback at
about 40 s) suggests Kodi read again after the first 0, and the file-server-unreachable notice may already cover this
case. Check live with the server-down simulation; if it ends silently, mark the stream fatal and queue the notice when
the budget runs out.

#### Dispatcharr 0.33.0: guide history now builds up, and the guide fetch asks for all of it

**Opened 2026-10-24, from the review of Dispatcharr 0.33.0 against the addon; read from its changelog and source, not
yet seen live.** Up to 0.32.0 an EPG refresh replaced a channel's stored programmes with what the pull held, so the
server kept about two days of history, and `ComputeGuidePrevDays()` could ask for the longest catch-up window (up to 30
days) for free: 3, 7 and 30 days were measured byte-identical (`CLOSED_ITEMS.md`, "Guide fetch never includes
already-aired programmes"). 0.33.0 (upstream #1707) keeps ended programmes for the largest `catchup_days` of the
catch-up channels mapped to each EPG source (at least 1 day, at most 30), and Settings → EPG gains a minimum and
maximum retention (defaults 1 and 30 days, the maximum settable from 0 to 365). So on a server with long catch-up
windows the guide the addon downloads at every start grows for up to that many days after the upgrade. Check on the
lab after upgrading: the `/output/epg` size and parse time with the addon's `prev_days` over the first week, against
`kMaxXmlTvResponseBytes` (256 MB on a 32-bit build) and the slowest device. If it matters, ask for less history (the
guide only needs to reach as far back as catch-up can be browsed in Kodi) or make the depth a setting. The new maximum
also means `kServerMaxPrevDays` (30) is no longer the server's fixed clamp; a server set lower simply returns less.

#### Dispatcharr 0.33.0: catch-up seeks to a new start now reopen the provider

**Opened 2026-10-24, from the same review.** 0.33.0 (upstream #1540) changes catch-up seeking: a request for a
different programme start now asks the provider for that time and drops the cached CDN URL, where 0.32.0 mapped the new
start onto a byte offset in the file already open; the same start still reuses the cached file for Range reconnects.
The addon's session URLs (`/proxy/catchup/<uuid>?session_id=`) are served by the same `_serve_catchup()`
(`apps/timeshift/views.py`) as XC timeshift. With `enable_catchup_ffmpegdirect_seek` on, every seek builds a URL for a
new start, so seeks may now land closer than the 20 to 25 s short measured on 0.32.0 (`API_NOTES.md`, the 0.32.0
section), at the cost of a provider open per seek. Re-run that measurement after the upgrade, check that a seek with
the setting off (Range within one start) did not regress, and record the result in `docs/CATCHUP.md`.

#### Dispatcharr 0.33.0: live-proxy start-up and stream-switch changes to re-test

**Opened 2026-10-24, from the same review.** Several 0.33.0 proxy fixes touch things the addon worked around or
documented:
- **The Kodi 22 first-play failure** the warm-up exists for (`CLOSED_ITEMS.md`, "Kodi 22: the first attempt to play a
  channel nobody is watching failed in Off mode (HTTP 503)") is the proxy's "Channel is stopping" 503
  (`_channel_stopping_response()`, `apps/proxy/live_proxy/views.py`). 0.33.0 fixes a deliberate stop rewriting that
  state for up to 60 s (upstream #1811), several clients opening a stopped channel at once (upstream #1699; the warm-up
  and Kodi's own probes are such clients) and a new channel being stopped before its first viewer attached (upstream
  #1826). Check on Kodi 22 whether a cold first play still fails with the warm-up turned off; keep the warm-up for
  older servers either way.
- **Slow starts.** The locked FFmpeg profiles now set `-max_interleave_delta 100K` (upstream #1825): with `-map 0` and a
  stream that never sends a packet (an empty timed-ID3 track is the usual one), FFmpeg held about 10 s of media back at
  the start. That may be part of the 20 to 24 s starts in `CLOSED_ITEMS.md`'s "A channel that is slow to start" (the
  reporter linked theirs to the profile's stream mapping). Re-time a cold start with the default FFmpeg profile and, if
  it drops, add "update Dispatcharr" to `docs/TROUBLESHOOTING.md`'s entry. The new **Stream Startup Timeout** (default
  8 s) also asks for a switch to the channel's next stream when a new connection sends nothing for that long, so a slow
  channel with a backup stream may now change source during its start rather than wait.
- **Stream switches** now set the MPEG-TS `discontinuity_indicator` on the first packet of each PID from the new source.
  Re-run a stream switch (a channel with a second stream, the first one stalled, as in `CLOSED_ITEMS.md`'s "A silent
  upstream stall ended playback about 48 s before Dispatcharr healed it") in Off, Local and Server-side mode: Kodi's
  demuxer and the plugin's ffmpeg should both handle it at least as well as before, but neither has seen it.

### Fix known, not yet done

#### timeshift_buffer: removals under the 30 s start lock let a second start run beside the first

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; MEDIUM, reproduced with real filesystem operations and a
fake Redis/Popen.** `_START_BUFFER_LOCK_TTL` is 30 s and never renewed, but the "dead" branch of `start_buffer`
(`_remove_channel_files()` then an unconditional `_delete_buffer_state()`), the stale_config teardown,
`_clear_channel_dir()`'s in-place `rmtree` (under the slot lock too) and `_scrub_orphaned_dirs()` all remove directories
while holding it. The 2026-10-21 tombstone and compare-and-delete fix covered only teardowns outside the lock. A removal
that outlives the TTL (network storage, or a long buffer at the new 1440-minute cap) lets a second `start_buffer` for
the channel take both locks: in the clear-dir case both calls spawn ffmpeg into one directory, the second write wins
Redis and the owner file, and the other ffmpeg is tracked nowhere (a second provider connection, colliding `seg_%05d.ts`
names, the cap bypassed); in the dead case the first call's unconditional delete removes the second caller's state and
its token stops working. Fix: under the lock only rename to a tombstone (`_TOMBSTONE_RE`) and `rmtree` after release;
use `_delete_buffer_state_if_instance()` in the dead branch; optionally re-check the lock token before `Popen` and the
state write. Tests: a monkeypatched `rmtree` that expires the locks and runs a second start inside the removal.

#### The live heartbeat carries no access token, so a resumed viewer can stick to a replacement buffer

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading.** `SendTimeshiftHeartbeat()`
sends only the channel and viewer id, and the plugin's `_heartbeat()`/`_apply_heartbeat()` add that viewer to whatever
buffer is current. Since 0.8.15 a stop with an old token is declined. So: viewer A pauses past the idle timeout and is
reaped, B starts the channel, A resumes and its overdue heartbeat registers it on B's buffer, A then goes fatal on a 403
and its stop is declined, and when B stops the buffer and its provider slot stay up until the reaper prunes A. Fix: send
`access_token` with the heartbeat and have `_heartbeat()` skip and answer "replaced" when `_is_stale_access_token()`
matches, as `get_live_manifest` does. Also reword `CLOSED_ITEMS.md`'s 0.8.15 entry, which says heartbeats carried the
token in the probe (the heartbeat action's body does not).

#### OpenRecordedStream() writes the API-key owner stamps before the key they vouch for

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading, narrow window.** On a successful
`ObtainApiKey()` it calls `WriteApiKeyOwnerStampsIfChanged()` at once, but the adopted or regenerated key reaches
`api_key` only at the end of the open, via `PersistApiKeyIfChanged(keyBefore)`; the constructor does it in the right
order. If Kodi exits or crashes during the open (or the settings dialog is open and cancelled, so the `api_key` write is
dropped), the next start finds stamps for the new account beside the old account's key, sets
`m_apiKeyOwnershipVerified`, and on the same server that key authorises as the wrong user with no 401. Fix:
`PersistApiKeyIfChanged(keyBefore)` first, then the stamps, then refresh `keyBefore`. A glue variant of the
`owner_stamps` scenario can check the stamps are never on disk without their key.

#### Windows (and 32-bit) parse a pre-1970 (pre-1901) date to a floor that later subtractions overflow

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, reproduced with an injected `gm` under UBSan.**
`_mkgmtime` returns -1 outside 1970-3000, so since the 2026-10-18 floor fix any date before 1969 on Windows saturates to
`kEarliestRepresentableTime` (`INT64_MIN` plus a year) instead of -1; on 32-bit the same happens before 1901-12-13. The
floor leaves room for an offset, not for `now - t` or `end - start`: with a start of 1960 `ClampedDurationSeconds()`
overflows, `IsWithinCatchupWindow()` answers true and `CatchupSessionDurationMinutes()` a large negative number (also
`EpgProgramMatch.cpp` and `ComputeRecordingExtendMinutes`). Only a garbage or hostile server timestamp reaches it.
Related: a date after 3000 on Windows saturates to a value `gmtime_s` cannot format. Fix: a portable int64
days-from-civil in place of `_mkgmtime`/`timegm`, saturated to `time_t`, or have the parsers answer 0 (unknown) at the
floor. Tests: a Windows-like `gm` in `test_time_util.cpp` and floor-valued starts in the recording, EPG-tag and
catch-up-session tests (they fail under UBSan today).

#### ParseProgramLookupResponse() and the timer body copy server subtrees of any depth

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, reproduced on a 1 MiB stack.**
`TimerRequestBuilder.cpp` copies `id`/`title`/`sub_title`/`description`/`tvg_id` from the EPG search response (`out[key]
= *it`) and into the timer body (`program[key] = *it`) whatever their type, and the body is later `dump()`ed: the
recursive copy the 2026-10-19 `FieldRefOrNull` fix removed elsewhere. A `description` nested 12,000 deep segfaults on a
1 MiB thread. Dispatcharr sends strings, so only a hostile server reaches it. Fix: copy a key only when it is a string
(an integer for `id`). Test: a 100,000-deep value via `RunOnSmallStack` in `test_timer_request_builder.cpp`.

#### A search-mode series rule costs terms times text length per recording

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, measured.** `SeriesRuleTextMatch.cpp`: `Leaf()` rescans
the folded text for every operand, repeated terms included, `ContainsWholeWord()` visits every occurrence that fails the
word-boundary check, and `Combine()` evaluates every leaf. The 4 KiB query cap bounds the query, not the work: 680
copies of one short term ORed together take 105 ms against 10 KB of text and 10.8 s against 1 MB, per recording, on
every `GetTimers()`. Needs a runaway or hostile rule. Fix: memoize leaves per call, precompute a word-boundary table
once, and optionally answer `kUnsupported` past a terms-times-text budget. Test: a timing-bounded case and unchanged
results in `test_series_rule_text_match.cpp`.

#### Two text-handling gaps: SanitizeRecordingDirectory's std::isspace, and C1 controls in SanitizeServerErrorBody

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading (the locale case not reproduced
here).** (1) `RecordingDirectory.h` trims with `std::isspace`, which depends on the C locale Kodi sets from the user's
region; under a Windows code page that counts 0xA0 as a space, a title ending in "à" (UTF-8 `C3 A0`) loses its last byte
and the folder name is invalid UTF-8. It also trims only ASCII whitespace, where the function says it mirrors
Dispatcharr's `_safe_name()` (check whether that uses Python's Unicode `.strip()`). Fix: `StripUnicodeWhitespace()` from
`UnicodeText.h`; tests for "Voilà" and a U+3000 edge. (2) `SanitizeServerErrorBody()` (`JsonResponse.cpp`) blanks C0 and
DEL but passes C1 controls, raw 0x80-0x9F and UTF-8 U+0080-U+009F (`C2 9B` is CSI), into `kodi.log`. Fix: blank those
too, with a test in `test_json_response.cpp`.

#### timeshift_buffer: segment_seconds has no upper limit, and the 1440-minute cap makes manifests heavy

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, (1) confirmed by reading, (2) measured.** (1)
`segment_seconds` is read with `minimum=1` only, missed by the 2026-10-18 caps: a typo such as 3600 means no segment
ever closes, every open fails the addon's cold start with "live playlist not found", and one `.ts` file grows until the
buffer stops. Cap it (60, and no more than `buffer_minutes*60`) and say so in the help text. (2) At the new
`_MAX_BUFFER_MINUTES` with 2 s segments a cold manifest build takes 3.2 s on local disk (137 ms at the default), returns
5.9 MB of JSON, and holds the single per-worker `_manifest_cache_lock` throughout, serialising every channel's
`get_live_manifest` in that worker; the 16-entry cache was sized for the default. Fix: a lower cap, a byte-based cache
limit, or the stat sweep outside the lock.

#### timeshift_buffer: a Redis outage logs a traceback per file-server request and drops the connection

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading.** `_check_access_token()` is
called from the request handler with no try, and redis-py's `ConnectionError`/`TimeoutError` are not the built-in ones,
so each playlist or segment request during an outage (or with a corrupt state, through `ValueError`) reaches
`handle_error`'s `logger.exception` and the client gets a dropped connection instead of a status. The reaper rate-limits
the same errors. Fix: catch in `_resolve_and_authorize`, answer 503, and rate-limit the log as
`_should_log_reaper_error` does; test with a raising `_get_buffer_state` through `_fake_handler`.

#### RefreshAccessToken() drops a rotated refresh token

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible: depends on Dispatcharr's SimpleJWT
settings.** Only `access` is kept from the refresh response, unlike `Login()`. With `ROTATE_REFRESH_TOKENS` and
blacklisting on, every second refresh would send a blacklisted token, get a 401 (`kRejected`) and fall back to a full
password login, feeding the login rate limit `AuthBackoff` exists to avoid. Without rotation it costs nothing. Fix: keep
`refresh` when present, under the state lock; check Dispatcharr's `SIMPLE_JWT` settings while there.

#### CONNECTED is held back with no limit while the DVR access level is unknown

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible.** `TakePending(report, m_dvrAccessChecked)`
delivers `CONNECTED` only once `SyncDvrAccessFromDispatcharr()` has succeeded, which needs a numeric `user_level` from
`/api/accounts/users/me/`. A server or proxy that omits it, after a start that reported `UNREACHABLE`, leaves Kodi
showing the client unreachable and never re-reading `GetCapabilities()`; playback is unaffected. Every Dispatcharr seen
so far sends the field. Fix: treat a parse failure (as opposed to a network failure) as checked and fail open, or
deliver `CONNECTED` after a few failed reads; testable in `ConnectionReport`.

### Architectural / concurrency

#### Nothing stops an exception crossing from a PVR callback into Kodi

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, hardening, confirmed by reading.** The worker threads
and detached tasks run under `RunGuardedWorker()`/`RunBackgroundTask()`, but the `CInstancePVRClient` overrides Kodi
calls on its own threads (`GetTimers`, `GetChannels`, `UpdateTimer` and the rest) and the constructor reached from
`CreateInstance` in `addon.cpp` are not, and neither the dev-kit thunks nor `CPVRClient::DoAddonCall()` catch anything.
A `std::bad_alloc` copying the recordings or rules vectors on a 32-bit or low-memory box ends the Kodi process (and
crosses a CRT boundary on Windows). Fix: a small guard that catches `std::exception` and `...`, logs, and returns
`PVR_ERROR_FAILED`/`false`/`-1`, around each override, and around `new PVRDispatcharr` returning `ADDON_STATUS_UNKNOWN`.

### Design decision needed

#### timeshift_buffer: a stop from the last listed viewer ends a buffer another viewer is still reading

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible.** 0.8.15 stopped the reaper ending a buffer
whose viewers went stale while its segments are still read; `stop_buffer` has the same gap. If viewer A's API calls fail
for 30 s or more while its segment reads work, the reaper drops A from the list, and when B stops, the empty list tears
the buffer down under A (`abort_if=viewers` only). Choices: let the empty-list case defer to the reaper's buffer-wide
idle check (costing up to the idle timeout plus a tick of provider slot), or have segment fetches carry the viewer id so
they refresh per-viewer stamps.

### Release, CI and manual testing

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: cut the next `Piers` release from the final without the pre-release mark (`0.14.0-Piers` and `0.14.1-Piers` were cut from RC1 and both carry it) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

### Upstream (Dispatcharr) or documentation accuracy

None open.

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

#### build_kodi_repo.py index does not tie the tag's branch suffix to the Kodi line

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, reproduced on the published 0.14.1 zips.**
`tag_version` checks only `\d+\.\d+\.\d+(-[A-Za-z]+)?` and drops the suffix, and nothing checks the tag exists, so
`--kodi 22` with `0.14.1`, `0.14.1-Omega` or a typo such as `0.14.1-Pires` all succeed and write `<path>` under that
tag. The documented publish step pushes the result, so one typo publishes an index whose every download 404s for that
Kodi line. Fix: put the branch name in `KODI_LINES` and require the suffix to match (the plain form only exists for tags
that are never republished), and optionally `git rev-parse --verify refs/tags/<tag>`. Tests: mismatched and missing
suffixes raise `RepoError`.

#### build_kodi_repo.py repository stamps a zip of uncommitted source with the last commit's time

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading.** `source_time` uses `git log
-1 --format=%ct -- kodi-repository/...` while the content comes from the working tree, so a build before committing an
`addon.xml.in` change gets the previous commit's time, may publish content that was never committed, and differs from
the post-commit build under the same file name. Fix: without `--time`, refuse when `git status --porcelain --
kodi-repository/` is not empty; test with a temporary repository or a patched `subprocess.run`.

#### The release gate does not scan bytes outside a member's decompressed data

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, reproduced with three crafted zips.** `check_zip`
checks the archive comment but not per-entry comments (`info.comment`), skips an entry whose name ends in `/` before
reading it (a directory entry can carry a payload), and never looks at bytes before the first local header or between
entries (zipfile, Kodi and unzip all accept prepended data). A blocklisted string in any of the three passed as clean.
Every current pipeline runs `normalize_zip.py` first, which drops all three, but the gate's premise is that it holds
whoever built the zip. Fix: fail on any entry comment and any directory entry with a payload, and also scan the raw file
bytes (check the real 0.14.1 zips for false positives). Tests: the three crafted cases in `test_check_release_zip.py`.
