*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**11 open, 344 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the two
2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); one comes from the Kodi 22
work on `Piers`: what remains of Kodi 22 support. One came from the 2026-10-19 live check of four audit fixes (a second, also from it, was fixed the same day). One tracks publishing the Kodi repository added on 2026-10-19. The other 6 come from the 2026-10-18 read-only audit of the whole
project (addon, both plugins, tools, CI and packaging; findings only, nothing changed; 28 at first, seven fixed the same
day, 14 on 2026-10-19 and one on 2026-10-21, all moved to `CLOSED_ITEMS.md`), each marked with how sure the audit was: *confirmed* (reproduced, or the whole code
path read end to end), *plausible* (the logic is there, the trigger is not demonstrated) or *low confidence*.

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 2 |
| Open: Architectural / concurrency | 2 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 3 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 300 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 35 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### The addon never calls `curl_global_init` itself

**Opened 2026-10-18, from the read-only audit; low confidence.** libcurl initialises itself lazily on the first
`curl_easy_init`, which is not thread-safe before curl 7.84, and the addon starts transfers from several threads at once.
The Windows build bundles curl 7.67.0 and Android links a static curl. To check: whether Kodi's own earlier
initialisation covers the addon's copy on those platforms. If it does not, one `curl_global_init(CURL_GLOBAL_ALL)` in the
addon's create path (and the matching cleanup on destroy) is the fix.

### Fix known, not yet done

#### timeshift_buffer: a corrupt buffer state aborts the orphan scrub, and client disconnects print tracebacks

**Opened 2026-10-18, from the read-only audit; confirmed by reading.** (1) A buffer state key in Redis that is not valid
JSON makes `_get_buffer_state` raise inside `_is_untracked_orphan`, which ends the whole scrub pass rather than skipping
that one entry. (2) A client that disconnects mid-response raises `BrokenPipeError` or `ConnectionResetError` into
`socketserver`'s default `handle_error`, which prints a raw traceback; override it on the file server to log one line.
Regression tests in the plugin's `tests/`.

#### timeshift_buffer: smaller hardening (setting caps, ffmpeg I/O timeout, process liveness, token rows)

**Opened 2026-10-18, from the read-only audit.** (1) `buffer_minutes`, `max_concurrent_buffers` and
`idle_timeout_seconds` have no upper limit; cap them. (2) The ffmpeg input has no I/O timeout; add `-rw_timeout` (low
confidence that it helps with the provider streams in use). (3) `_is_process_alive` treats a `PermissionError` as alive
before it compares the start ticks (low confidence; depends on whether the container runs as root). (4)
`RefreshToken.for_user` may add a database row per buffer start if Django's `token_blacklist` app is installed (low
confidence; not checked on a real server).

### Architectural / concurrency

#### timeshift_buffer: a teardown that outlasts its grace period can delete a restarted buffer

**Opened 2026-10-18, from the read-only audit; plausible.** `_teardown_buffer` ends with an unconditional
`_delete_buffer_state` and does not hold the start lock; the "stopping" marker keeps `start_buffer` away for only
`_TEARDOWN_GRACE_SECONDS` (30 s). If removing the segments takes longer (thousands of them, slow storage; with no cap on
`buffer_minutes`), a `start_buffer` for the same channel classifies the buffer as dead and starts a new ffmpeg, and the
old teardown then deletes the new state. The viewer gets 403s and the new ffmpeg holds a provider slot, untracked, until
the orphan scrub about 300 s later; the final `rmdir` of the old rmtree can also remove the new directory.
`docs/TEST_COVERAGE.md` describes this race as closed by the grace period, which does not cover a teardown longer than
30 s; correct it with the fix. Fix: a compare-and-delete for the state (a Lua script that deletes only if the pid and
`started_at` still match), and rename the directory to a tombstone before removing it. In the same area: `stop_buffer` is
not bound to one buffer instance, so a late stop can tear down a replacement; send and check its `access_token`.
Regression tests in the plugin's `tests/`.

#### A refresh while Kodi's timer dialog is open can revert another client's change to a series rule

**Opened 2026-10-18, from the read-only audit; plausible.** `UpdateTimer()` diffs the dialog's values against
`m_reportedRecurringRules` and `m_reportedRecurringRuleTimes`, but any `GetTimers()` that finishes while the dialog is
open overwrites that baseline. If another client changes the rule's name or days meanwhile and a WebSocket event or the
5-minute refresh picks it up, the dialog's old values look like edits and are PATCHed back: the revert the 2026-10-02
fix was meant to stop. Smaller race in the same code: `GetTimers()` clears the times map and refills it rule by rule, so
an `UpdateTimer()` in that gap falls back to re-derived times and brings back the DST time-shift edit. Fix: keep the
snapshot the dialog was opened from per rule, and build the times map locally and swap it in under one lock. The baseline
choice is testable in `RecurringRuleEdit`.

### Design decision needed

#### recording_edl: a DVR path template may resolve outside the recordings root

**Opened 2026-10-18, from the read-only audit; deliberate today.** `_resolve_scan_root` refuses only the recordings root
itself or an ancestor of it; a template that is absolute, or whose `..` segments lead somewhere else, is scanned and
scrubbed there on purpose (its docstring explains why: such recordings' sidecars still need finding). Templates are set
by a Dispatcharr admin, so this is not reachable by an ordinary user. Decision: keep it as is, or require the resolved
root to be inside a configured allow-list of directories.

### Release, CI and manual testing

#### The Kodi repository is built but not published

**Opened 2026-10-19, when the repository was added** (`tools/build_kodi_repo.py`, `kodi-repository/`; how it works and
what was confirmed live: `docs/BUILDING.md`'s "Distribution: the Kodi repository"). Nothing is public yet, and
README.md's "Installing" already describes it, so the site has to exist before README.md reaches the public repository.
First publish: (1) create the `gh-pages` branch on the public repository holding only the site: `index` for Kodi 21
from `0.13.0` and for Kodi 22 from `0.14.0-Piers` (the gated zips), then `repository`; gate the repository zip;
(2) let the local pre-push hook allow `gh-pages` on the public remote; (3) turn on GitHub Pages for that branch and
check the site URL and a range request (206); (4) install the repository on a real Kodi from the public URL and
install the addon from it. Then (5) port the tool, the repository addon and README.md's "Installing" to `Omega`
(GitHub's default branch, whose README Kodi 21 users read), and record it in `docs/BRANCHES.md`.

#### The glue harness's TSan build crashes at start about one run in four

**Opened 2026-10-19, seen while checking the shutdown-task fix.** On unmodified `Piers`,
`BIN=pvr_harness tests/glue/run_tsan.sh functional` exited with rc 139 before printing "constructed" in 3 of 12 runs, with
no ThreadSanitizer report; the fix's branch showed the same (2 of 12). `run_tsan.sh` retries only on TSan's "unexpected
memory mapping", so these count as failures. Probably the same address-space layout problem showing as a segfault rather
than that message; not investigated. The TSan variant is run by hand, not in CI, so it costs reruns, not red builds.
Candidates: retry on a signal exit with no output as well, or start the harness with address randomisation turned off.

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: take the pre-release mark off `0.14.0-Piers` (or cut `0.14.1` from the final) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

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

None open.
