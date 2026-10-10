*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**3 open, 277 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)).

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 243 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 25 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### Closing a live timeshift stream waited for the reader's live-edge wait (4 to 6 seconds measured); now sliced, one slow-device check outstanding

**Found 2026-10-07 in the logs of the 0.12.0 device runs; sliced 2026-10-09.** From Kodi's `CloseFile` to the addon's `CloseLiveStream` took 4.0 to 5.9 seconds on all three devices (the N2+: 5.2 s; the 32-bit phone: 5.9 s; the 64-bit phone: 4.0 s), because the reader thread was parked in `ReadLiveTimeshiftStream()`'s catch-up-to-tail wait and Kodi acts on a close only between reads (its demuxer checks its abort flag before each call into the addon, and a `Player.Open` for the next channel sent in that window was dropped). The wait cannot be interrupted from the addon, and a shorter budget would bring back the early end of playback it exists to prevent (`docs/CLOSED_ITEMS.md`'s "A brief server outage ended playback for good"), so the wait is now taken in slices (`kLiveTailWaitSliceMs`, one second, CatchUpUtil.h): a slice that finds nothing returns `-1`, which Kodi's demuxer retries (a `0` ends playback), the budget is counted across the calls at the same position (`TailWaitEpisode`), and the stream still gives up with `0` only once the whole budget is used; a seek probe still gets its quick answer. **Measured:** the glue harness's `live_tail_bound` scenario (a frozen live tail) shows the longest read at the tail going from 4515 ms to 1004 ms, the total wait before giving up unchanged, and the stream resuming when segments arrive; on the Linux test VM against the real server (four sessions, baseline and candidate alternating) the new path ran (13 slices per session), playback never stalled, and the demuxer, ffmpeg and stall messages in Kodi's log were the same count as the baseline's. **Not shown:** an improvement in the lab (its segments are two seconds long, so the old wait was already about a second) and the one thing the lab cannot answer, whether ffmpeg's flush after each `-1` is visible as a glitch on a slow device that waits at the tail for seconds. Needed: the same close and zap timing, plus a few minutes of live viewing at the live edge, on the N2+ (or either phone); if a glitch shows, the slice length is `kLiveTailWaitSliceMs` and the old behaviour is a slice as long as the budget. Not for a release before that.

### Fix known, not yet done

### Architectural / concurrency

### Design decision needed

### Release, CI and manual testing

### Upstream (Dispatcharr) or documentation accuracy

### Known gaps, deliberately deferred

#### Order-sensitive response decisions still inline in `DispatcharrClient`

**Opened 2026-10-06 by the sixteenth hardening sweep; two of the six chains extracted 2026-10-09.** The recording read and the live segment fetch moved into `ClassifyRecordingReadResponse()`/`ClassifyLiveSegmentResponse()`, and now `PerformWithSafeRedirects()`'s order (no Location, then an unsafe target, then the hop limit: `DecideRedirectStep()`) and `EnsureAuthenticated()`'s gate order (valid token, login backoff, transient cooldown, all before any refresh or login: `DecideAuthGate()`) are pure, table-tested functions. The same kind of chain remains, untested as a whole, in the in-progress segment fetch (`ReadInProgressRecordingStream()`), the recording open probe (`OpenRecordingStream()`: 401 retry, failure, an in-progress HLS redirect, a `CURLE_FILESIZE_EXCEEDED` read as success) and the segment span arithmetic shared by three read paths (a helper for it would need a guard for a non-positive available count). Each is a small enum-returning extraction like the others. Deferred: none has a known bug, and each extraction touches a hot, lock-sensitive path (the glue harness in CI covers the behaviour; a live check is still wanted after).

#### `stop_buffer` still drops other viewers by comparing two stamps, so a host clock step can end a buffer under a viewer

**Opened 2026-10-09 when the reaper half of the old "host clock step" item was fixed (`timeshift_buffer` 0.8.14).** `stop_buffer`'s `drop_viewer` runs `_prune_stale_viewers()` in the request worker that handles the stop: it removes the stopping viewer and also every other viewer whose last heartbeat is more than `idle_timeout_seconds` older than the shared clock. The reaper no longer does that (it ages heartbeats on a monotonic clock, `_IdleTracker`), but a request worker has no memory between calls, so it can only subtract stamps. If the host clock stepped forward by more than the timeout shortly before one viewer stops, the other viewers' stamps look stale, they are dropped, the list is empty and the buffer is torn down under viewers that are still watching; their next fetch fails and Kodi starts a new buffer. It needs a step bigger than the timeout and a stop in the window before the next heartbeats land. A fix would stop pruning there and leave stale viewers to the reaper (which can now do it safely), at the cost of a crashed viewer's phantom entry surviving until the reaper's next tick (15 seconds) when the last real viewer stops: the buffer then stops one tick later instead of at once. Deferred: rare, self-healing, and the change alters `stop_buffer`'s documented immediate teardown.

### Tooling (tools/)
