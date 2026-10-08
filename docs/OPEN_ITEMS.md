*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**6 open, 273 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)).

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 4 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 239 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 25 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done

### Architectural / concurrency

### Design decision needed

#### Closing a live timeshift stream waits for the reader's live-edge wait (4 to 6 seconds measured)

**Found 2026-10-07 in the logs of the 0.12.0 device runs; not a regression and not specific to Dispatcharr 0.32.0.** From Kodi's `CloseFile` to the addon's `CloseLiveStream` took 4.0 to 5.9 seconds on all three devices (the N2+: 5.2 s; the 32-bit phone: 5.9 s; the 64-bit phone: 4.0 s). In each case the reader thread was parked in `ReadLiveTimeshiftStream()`'s catch-up-to-tail wait ("catch-up-to-tail loop used 16/25 attempts, 4.201s (budget 6.2s)" is logged inside the window), and Kodi's main thread waits for its demux thread before it closes the stream. A `Player.Open` for the next channel sent in that window is dropped by Kodi (the harness's second live check saw the closing player and then reported `canseek=false`), and a user zapping channels feels the same delay. The wait cannot be interrupted from the addon today: Kodi calls `CloseLiveStream` only after the reading thread has returned, so there is no signal to raise. What would change it is a shorter wait at the live edge (the wait exists because a zero-byte read ended playback after about five seconds when nothing was buffered ahead, `docs/CLOSED_ITEMS.md`'s "A brief server outage ended playback for good"), or returning short reads that let Kodi's own loop notice the close sooner; both trade responsiveness against stalls and need a live comparison, which is why this is a decision and not a fix. Measured by pairing the `CloseFile` and `ClosePVRStream` lines in each device's `kodi.log`.

### Release, CI and manual testing

#### Release upload shares jobs with pull-request builds

**Split out 2026-10-06 from the closed entry on the moving Kodi branch.** The jobs that hold `contents: write` for the release upload (`build-unix`, `package-dispatcharr-plugins`, `build-windows`) also compile pull-request code from same-repo branches, so a branch's build step runs with a token that can write releases. Fork pull requests are already excluded from the Windows job and run with a read-only token elsewhere, and only the maintainer can push same-repo branches, so the exposure is small. The fix is a tag-only release job that `needs:` the build jobs and downloads their artifacts, with `contents: write` only there. Not done: it moves every release asset through artifact upload and download (the workflow already uploads with the current artifact action on this runner; downloading in a later job has not been tried here), and a release can only be exercised by cutting a tag, so it should be done together with the next release rather than blind. **Added 2026-10-06 (sixteenth sweep):** the same job should `needs:` every test job (`lint`, the unit-test jobs, `glue-harness`): today the three upload jobs have no `needs:`, so a tag whose tests fail still publishes its assets, and the three jobs create the same release at once (a `softprops/action-gh-release` race).

### Upstream (Dispatcharr) or documentation accuracy

### Known gaps, deliberately deferred

#### Order-sensitive response decisions still inline in `DispatcharrClient`

**Opened 2026-10-06 by the sixteenth hardening sweep.** The recording read and the live segment fetch moved into `ClassifyRecordingReadResponse()`/`ClassifyLiveSegmentResponse()`; the same kind of chain remains, untested as a whole, in the in-progress segment fetch (`ReadInProgressRecordingStream()`), the recording open probe (`OpenRecordingStream()`: 401 retry, failure, an in-progress HLS redirect, a `CURLE_FILESIZE_EXCEEDED` read as success), `EnsureAuthenticated()`'s gate order (fast path, credential backoff, transient cooldown, refresh, classify, login: moving the cooldown after the refresh attempt reintroduces the 47th-pass storm and only the glue harness might catch it), `PerformWithSafeRedirects()` (unsafe-target check before the hop limit) and the segment span arithmetic shared by three read paths (a helper for it would need a guard for a non-positive available count). Each is a small enum-returning extraction like the two above. Deferred: none has a known bug, and each extraction touches a hot, lock-sensitive path that wants a live check after.

#### Nothing on a 64-bit build checks that the date parsers use the saturating time conversion

**Opened 2026-10-06 by the sixteenth hardening sweep (recorded before only inside closed entries).** `SaturatingTimeGm()` makes a date from 2038 on read correctly on a 32-bit `time_t`, and the tests that cover it run on the build host, where `time_t` is 64 bits and a plain `timegm()` would give the same answers: nothing fails if a parser goes back to the plain call. Closing it needs a 32-bit build in CI (the Linux runner can compile with `-m32` where the multilib packages exist) or a test seam that substitutes a narrow `time_t`. Deferred as low value: the only parsers affected are the ones a recurring rule's end date and an XMLTV time go through, and a regression would show on the Android and 32-bit ARM builds this project does not ship from CI.

#### A stop while the realtime socket is blocked in a send is not tested

**Opened 2026-10-06 by the sixteenth hardening sweep (recorded before only inside a closed entry).** `WebSocketClient`'s sends (`SendAll()`) wait on the peer's receive window, and the stop check added with the 2026-10-04 sweep is made between waits of 500 ms; a stop that arrives while a send is blocked on a full send buffer (a peer that has stopped reading) is believed to end within one slice but no test fills a send buffer to prove it. The local-server fixture could do it with a peer that never reads. Deferred: a pong or close frame is under 140 bytes, so the buffer would need to be full already.

#### A host clock step still misfires the reaper's idle checks (0.8.11 only fixed cross-host skew)

**Opened 2026-10-06 by the sixteenth hardening sweep, from reading.** `_shared_now()` (0.8.11) reads Redis's own `TIME`, which fixes timestamps written by a worker on one host and compared by another whose clock differs. It does not protect against the host's own clock being stepped (a manual change, a VM resume): Redis `TIME` is the Redis host's wall clock, containers on one host share it (the usual deployment; the all-in-one image runs Redis in the same container), so it moves exactly as far as `time.time()`, and the reaper's `now - last_heartbeat` compares misfire the way they did before. 0.8.11's changelog and docstring said otherwise and were corrected. A real fix makes the elected reaper measure age on its own monotonic clock: remember, per buffer and viewer, when it last saw the heartbeat value change, and reap on that age. Deferred: it is a redesign of the reaper's bookkeeping for a rare event (a clock stepped by more than `idle_timeout_seconds`, which makes viewers look idle, so buffers are stopped and the addon reports them gone), and a viewer's next start recovers.

### Tooling (tools/)
