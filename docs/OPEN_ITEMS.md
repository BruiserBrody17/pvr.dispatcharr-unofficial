*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**4 open, 276 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)).

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 3 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 242 |
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

### Upstream (Dispatcharr) or documentation accuracy

### Known gaps, deliberately deferred

#### Order-sensitive response decisions still inline in `DispatcharrClient`

**Opened 2026-10-06 by the sixteenth hardening sweep; two of the six chains extracted 2026-10-09.** The recording read and the live segment fetch moved into `ClassifyRecordingReadResponse()`/`ClassifyLiveSegmentResponse()`, and now `PerformWithSafeRedirects()`'s order (no Location, then an unsafe target, then the hop limit: `DecideRedirectStep()`) and `EnsureAuthenticated()`'s gate order (valid token, login backoff, transient cooldown, all before any refresh or login: `DecideAuthGate()`) are pure, table-tested functions. The same kind of chain remains, untested as a whole, in the in-progress segment fetch (`ReadInProgressRecordingStream()`), the recording open probe (`OpenRecordingStream()`: 401 retry, failure, an in-progress HLS redirect, a `CURLE_FILESIZE_EXCEEDED` read as success) and the segment span arithmetic shared by three read paths (a helper for it would need a guard for a non-positive available count). Each is a small enum-returning extraction like the others. Deferred: none has a known bug, and each extraction touches a hot, lock-sensitive path (the glue harness in CI covers the behaviour; a live check is still wanted after).

#### Nothing on a 64-bit build checks that the date parsers use the saturating time conversion

**Opened 2026-10-06 by the sixteenth hardening sweep (recorded before only inside closed entries).** `SaturatingTimeGm()` makes a date from 2038 on read correctly on a 32-bit `time_t`, and the tests that cover it run on the build host, where `time_t` is 64 bits and a plain `timegm()` would give the same answers: nothing fails if a parser goes back to the plain call. Closing it needs a 32-bit build in CI (the Linux runner can compile with `-m32` where the multilib packages exist) or a test seam that substitutes a narrow `time_t`. Deferred as low value: the only parsers affected are the ones a recurring rule's end date and an XMLTV time go through, and a regression would show on the Android and 32-bit ARM builds this project does not ship from CI.

#### `stop_buffer` still drops other viewers by comparing two stamps, so a host clock step can end a buffer under a viewer

**Opened 2026-10-09 when the reaper half of the old "host clock step" item was fixed (`timeshift_buffer` 0.8.14).** `stop_buffer`'s `drop_viewer` runs `_prune_stale_viewers()` in the request worker that handles the stop: it removes the stopping viewer and also every other viewer whose last heartbeat is more than `idle_timeout_seconds` older than the shared clock. The reaper no longer does that (it ages heartbeats on a monotonic clock, `_IdleTracker`), but a request worker has no memory between calls, so it can only subtract stamps. If the host clock stepped forward by more than the timeout shortly before one viewer stops, the other viewers' stamps look stale, they are dropped, the list is empty and the buffer is torn down under viewers that are still watching; their next fetch fails and Kodi starts a new buffer. It needs a step bigger than the timeout and a stop in the window before the next heartbeats land. A fix would stop pruning there and leave stale viewers to the reaper (which can now do it safely), at the cost of a crashed viewer's phantom entry surviving until the reaper's next tick (15 seconds) when the last real viewer stops: the buffer then stops one tick later instead of at once. Deferred: rare, self-healing, and the change alters `stop_buffer`'s documented immediate teardown.

### Tooling (tools/)
