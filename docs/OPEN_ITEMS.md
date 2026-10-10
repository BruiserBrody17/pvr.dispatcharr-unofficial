*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**1 open, 279 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)).

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 1 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 245 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 25 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done

### Architectural / concurrency

### Design decision needed

### Release, CI and manual testing

### Upstream (Dispatcharr) or documentation accuracy

### Known gaps, deliberately deferred

#### `stop_buffer` still drops other viewers by comparing two stamps, so a host clock step can end a buffer under a viewer

**Opened 2026-10-09 when the reaper half of the old "host clock step" item was fixed (`timeshift_buffer` 0.8.14).** `stop_buffer`'s `drop_viewer` runs `_prune_stale_viewers()` in the request worker that handles the stop: it removes the stopping viewer and also every other viewer whose last heartbeat is more than `idle_timeout_seconds` older than the shared clock. The reaper no longer does that (it ages heartbeats on a monotonic clock, `_IdleTracker`), but a request worker has no memory between calls, so it can only subtract stamps. If the host clock stepped forward by more than the timeout shortly before one viewer stops, the other viewers' stamps look stale, they are dropped, the list is empty and the buffer is torn down under viewers that are still watching; their next fetch fails and Kodi starts a new buffer. It needs a step bigger than the timeout and a stop in the window before the next heartbeats land. A fix would stop pruning there and leave stale viewers to the reaper (which can now do it safely), at the cost of a crashed viewer's phantom entry surviving until the reaper's next tick (15 seconds) when the last real viewer stops: the buffer then stops one tick later instead of at once. Deferred: rare, self-healing, and the change alters `stop_buffer`'s documented immediate teardown.

### Tooling (tools/)
