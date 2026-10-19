# Changelog

User-facing summary of what changed release to release. For the *why*
behind any of these -- root causes, investigations, things tried and
reverted -- see the `docs/` directory (start at
[docs/API_NOTES.md](docs/API_NOTES.md)); this file only covers what's
different, not the story behind it.

Versions before `0.2.0` aren't itemized here -- that was this project's
initial scaffold and buildout, before it had any tagged releases to
compare against.

## [Unreleased] -- addon

### Fixed

- **Pressing Play on a recording or a catch-up programme no longer freezes Kodi when the server stops answering**:
  Kodi waited the whole connection timeout (30 s by default, up to 3 minutes) before playback started or failed. A
  recording now skips that check entirely, and a catch-up programme gives up after a few seconds. Catch-up also asks
  for the programme's full length instead of up to a minute less.
- **A rare crash when Kodi exits or the addon restarts**: if the addon was just about to tell Kodi the connection
  state at that moment, Kodi could crash while closing it.
- **A finished recording whose live copy is gone opens from its file**: the server can leave a recording marked as
  still having its live copy after it ended, and the addon then failed to open it; it now plays the finished file.
- **Realtime updates work behind a server or proxy that writes `Upgrade:websocket` without a space.** The addon looked
  for the exact text `Upgrade: websocket` and, without it, silently fell back to refreshing on a timer. Dispatcharr's
  own server was never affected.
- **Hardening against bad server data**: a timeshift buffer address the plugin reports that would send its access token
  to another host is refused; a guide entry's season or episode below zero, a buffer playlist listing a segment twice,
  a series rule's weekday outside Monday to Sunday and a login reply without a usable token are rejected instead of
  passed on; a few date and catch-up calculations that could overflow on 32-bit devices no longer do; and search terms
  in requests are always escaped.
- **`timeshift_buffer` README**: the disk use per buffered channel is up to about twice the earlier estimate (about 9 GB
  for 60 minutes at 10 Mbit/s), and the port check now says that a `404` for the bare address means the server is
  reachable (the README and the troubleshooting guide said to expect `403`).

## `timeshift_buffer` [0.8.15] - Unreleased

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A buffer that is still being watched is no longer stopped when the viewer's check-ins fail for a while.** If the
  addon's periodic check-ins to the plugin failed for longer than the idle timeout (the server refusing requests for a
  moment, an expired login) while playback itself kept working, the buffer was stopped mid-playback. It now stays up as
  long as playback is reading from it. A viewer that crashed just before the last other viewer stopped now frees its
  provider slot up to the idle timeout plus 15 seconds after playback stopped, instead of up to 15 seconds.

## [0.13.0] - 2026-10-14

Addon changes since `0.12.0`. This release bundles `timeshift_buffer` `0.8.14` and `recording_edl` `0.2.3` as they
were in `0.12.0` (only the `timeshift_buffer` README gained a section on disk use), so there is nothing to redeploy to
Dispatcharr if you already run them. It carries the same eight downloads as `0.12.0`: Windows, Linux, macOS (Apple
silicon), Android (64-bit and 32-bit ARM), CoreELEC and the two Dispatcharr plugins. This list is the user-visible
part; the reasoning for each change is in `docs/CLOSED_ITEMS.md`'s Fixed entries. A short guide to common problems
is now in `docs/TROUBLESHOOTING.md`.

### Added

- **Kodi says when the address is not a Dispatcharr server**: if the host and port in the addon's settings reach something
  that answers but is not Dispatcharr (a plain web server, a proxy with nothing behind it, a login page), Kodi now shows
  "Server does not respond properly" with the address, and keeps it in the Event Log, instead of leaving an empty
  channel list and a line in the log file. It is reported for a 404, 405, 501, a redirect, or an answer to the login
  that has no token in it. A server that is simply down is still "Server is unreachable", and a refused password is
  still "Access denied". A proxy that answers 404 for a few seconds while Dispatcharr restarts shows the same message,
  followed by "Connection established" when the login works again.
- **Choose how a series rule made in Kodi matches the title**: a new setting, "Series rules made here: how the title is
  matched" (Exact by default, as before; Contains; Whole words). With Contains, a rule made from the guide for "News" also
  records "Late News" and "Newsreel"; Whole words matches "Late News" but not "Newsreel". It applies to rules created
  after you change it, never to rules that already exist, and rules made in Dispatcharr's web interface keep their own
  settings when edited from Kodi.
- **Connection problems are shown by Kodi**: when Dispatcharr cannot be reached, or refuses the addon's username and
  password, Kodi now says so itself ("Server is unreachable", "Access denied", and "Connection established" once it is
  back), with the host and port, in its own language, and keeps the entries in its Event Log. Before, the only trace was
  in the log file and the channel list simply stayed empty. Kodi does not pop up the first "unreachable" after it starts
  (a server that is not up yet at boot is common); it is in the Event Log. A server that goes away while Kodi is running
  shows "Connection lost" after about ten seconds of failed requests (a shorter blip says nothing), and "Connection
  established" when it answers again. This is noticed only while the addon is making requests, which it does at least
  every few minutes and constantly while a channel is playing.
- **Backend hostname**: the addon now tells Kodi the Dispatcharr host's name. Kodi uses it to tell whether a recording
  is running on the same machine as Kodi itself, and then asks before it powers that machine down mid-recording. Nothing
  changes when Dispatcharr is on another machine, and Kodi's information screen shows the same host and port as before.
- **Radio channels**: a channel Dispatcharr 0.32.0 flags as radio now appears in Kodi's Radio section instead of the TV
  list, with its own radio channel groups (a group holding both kinds is listed in both), and its recordings are marked
  as radio. A server older than 0.32.0 sends no flag, so every channel stays TV as before.

- **Recording posters and numbers**: a recording whose programme had artwork now shows it in Kodi's recordings list and
  information dialog, and a recording whose programme had a season and episode number shows them, the same as the guide
  entry it came from. Recordings without either look as before. Parental ratings are not shown: Kodi has no place for
  one on a recording.

### Fixed

- **Server-side live TV survives a stalled stream when Dispatcharr has a backup**: if a channel's stream stopped sending
  data without closing the connection, playback ended after about 10 seconds, although Dispatcharr switches to the
  channel's next stream after about a minute. In Server-side mode the addon now waits up to 70 seconds for it (the
  picture freezes, then resumes by itself), and Stop still takes effect within a few seconds. The wait is short as before
  when Dispatcharr or the network is down, and it ends at once when the buffer's ffmpeg has stopped. The price: a channel
  whose only stream stalls now shows a frozen picture for about a minute before playback ends, instead of 10 seconds.
- **A series rule with no title can no longer be deleted from Kodi, which could remove other recordings**: a series rule
  made in Dispatcharr's web interface with only a description filter (no title) shows in Kodi with an empty name.
  Dispatcharr's own delete of such a rule removes every upcoming recording on its channel (every one on the server for a
  rule with no channel), whichever rule or timer made them. Kodi's "Delete timer rule" on it now does nothing except
  say to delete it in Dispatcharr, and an edit that would change such a rule's channel or search text is refused the
  same way. Editing it in a way that keeps its identity, and deleting or editing any rule that has a title, work as before.
- **No more "Restart Kodi" advice for an account that cannot manage recordings**: if Kodi started while Dispatcharr was
  unreachable and the addon's account turned out to be view-only, the addon said to restart Kodi to hide the Record,
  delete and rename actions. Kodi hides them on its own once the connection is established, so the message was wrong
  and is gone.
- **A name you type for a manual timer is kept**: a timer made with "Add timer..." (not from the guide) and a name you
  typed showed that name for about three minutes and then fell back to the channel's name. The addon now stores the name
  on the server when it creates the timer, so it stays after that, after a restart of Kodi, on other Kodi devices and in
  Dispatcharr's own interface. A timer left at Kodi's default name,
  an instant recording and a timer made from the guide are unchanged and keep the title Dispatcharr finds for them. A
  manual timer you name by hand no longer gets a description or poster filled in by Dispatcharr, since a name set by hand
  takes priority.
- **A channel that is slow to start no longer fails the first open (Server-side live mode)**: the addon gave up on a
  channel's first segment after about 15 seconds, so a channel whose provider was slow to start showed a playback error
  and played at once on the second try. It now keeps waiting, up to about 30 seconds, for as long as the timeshift buffer
  plugin reports its stream is running but has produced nothing yet; a buffer whose stream has failed, or a server that
  stops answering, still fails as quickly as before. The log now also says when it gives up.
- **Recordings made from Kodi's guide keep their title and description**: a recording made from the guide could come out
  named "Recording <number>", with no description, when Dispatcharr's pre/post padding
  was large for the programme (a short show, or a few minutes of padding on a half-hour one) or when it spanned two
  programmes. The addon now sends the programme's details with the recording, the way Dispatcharr's own guide does.
  With an older Dispatcharr that cannot look the programme up, nothing changes from before.

- **A recording whose commercial markers cover all of it no longer plays to its end at once.** Comskip can mark a
  whole short recording as one commercial break; Kodi then skipped to the end and marked the recording watched. The
  addon now ignores a skip that covers nine tenths or more of the recording and plays it normally.
- **Stopping a recording that is still being written no longer waits for the next segment.** With playback caught up to
  the end of a recording in progress, Stop (or a channel change) could take up to six seconds, the same wait the live
  buffer had before its fix; it now takes about a second at most.
- **A server that stops answering while a channel is opening no longer holds the open for minutes.** If Dispatcharr
  accepted the request to start the live buffer and then stopped responding, Kodi's channel spinner could run for about
  fifteen minutes before an error. The open now gives up after about 30 seconds (one connection timeout, at the default) and
  releases the buffer it had started.
- **A server timeshift that cannot reach the buffer's file server now says so.** The plugin serves the buffer from its own
  port (`http_port`, 9192 by default), which the Dispatcharr container has to publish; when it was not reachable, the
  channel spun for about 30 seconds and then exited with no explanation. The addon now checks that port when the channel
  opens and shows a message naming it, within a few seconds.
- **A recording nothing titled now shows its channel's name instead of "Recording" and a number.** When a channel had no
  guide data for the time a recording was made, Dispatcharr had no programme to name it after and Kodi listed it as
  "Recording 1234". It now shows the channel's name, which on a channel named for its event is the event.

### Changed

- **A readable troubleshooting guide**: `docs/TROUBLESHOOTING.md` is rewritten as a short, symptom-first guide (what you
  see, why, what to do), linked from the README. New in it: slow channel starts on HLS sources (Dispatcharr's default
  FFmpeg stream profile opens every quality level first; the built-in Streamlink profile starts much sooner) and Kodi's
  own 20-second limit. The long investigations moved unchanged to `docs/TROUBLESHOOTING_DETAIL.md`. Documentation only.
- **`timeshift_buffer` README explains its disk use**: a short section says how much a buffered channel writes (about 4.5 GB
  an hour at 10 Mbit/s with the default 60-minute buffer length, and how to lower it), that a recording and the buffer on
  the same channel share one provider connection, and that answering "Play recording" at Kodi's prompt watches a
  running recording with pause and rewind and no buffer. Documentation only; the plugin itself is unchanged.
- **Clearer help for "EPG refresh interval"**: the setting's help text and the README now say what it is (how often the
  addon downloads the whole guide from Dispatcharr) and how it differs from Kodi's own Guide "Update interval" (how
  often Kodi re-reads the guide from the addon). Neither overrides the other. Nothing about how they behave changed.
- **Guide requests name the channel-id source**: the guide is now asked for with `tvg_id_source=channel_number`,
  Dispatcharr's own default, so a later Dispatcharr release that changed that default could not mismatch channels and
  their guides. The guide you get is identical.

- **Opening a recording where comskip found nothing no longer asks the `recording_edl` plugin.** The addon reads what
  comskip recorded about the recording and skips the request when there can be no markers (no commercials found,
  commercials already cut out, or comskip not installed); a recording with markers, and one comskip has said nothing
  about, are asked as before.
- Checked against Dispatcharr `0.32.0`: no addon change was needed. In-progress recording playback (Dispatcharr now
  writes those as event playlists), catch-up, timers and recurring rules, the real-time recording push and live
  timeshift all work against it, on a 64-bit Android phone and on the other devices tested.

## [0.12.0] - 2026-10-07

Addon changes since `0.11.0`. This release also bundles `timeshift_buffer` `0.8.14` and `recording_edl` `0.2.3`
(their own entries are below, from `0.6.3` and `0.2.1` on): **redeploy both plugins to Dispatcharr and restart
Dispatcharr** so every worker runs them. This list is the user-visible part; the reasoning for each change is in
`docs/CLOSED_ITEMS.md`'s Fixed entries (`docs/OPEN_ITEMS.md` in the 0.12.0 tag itself).

**Update 2026-10-10: `timeshift_buffer.zip` on this release was replaced with `0.8.14`** (it was `0.8.13`). A change of the host's clock could make the idle reaper stop buffers that were being watched, and closing one viewer could drop the others; both are fixed (the `timeshift_buffer` `0.8.14` entry below). The addon zips and `recording_edl.zip` are unchanged. If you installed `timeshift_buffer` `0.8.13` from this release, redeploy it and restart Dispatcharr.

**Release downloads**: every release now carries a zip for each platform: Windows, Linux, macOS (Apple silicon),
Android (64-bit and 32-bit ARM) and CoreELEC, plus the two Dispatcharr plugins. Each one is checked before it is
published for build-machine paths, leftover debug information and timestamps that reveal where it was built.

### Fixed

- **Faster updates from Dispatcharr** (with "Enable real-time recording/timer updates" on): when Dispatcharr
  finishes refreshing an EPG source, the addon fetches the new guide about six minutes later (after
  Dispatcharr's own cache of it has expired), and when an M3U refresh changes channels it fetches the new
  channel list within a minute, instead of waiting for the next polling interval (hours).
- **Slow servers**: how long stopping a stream, refreshing a playing stream and the startup check wait for a
  server that has stopped answering now follows the "Connection timeout" setting (a sixth of it, 5 seconds at
  the default of 30), so a server that answers slowly can be accommodated by raising that one setting.
- **Recurring timers**: if Dispatcharr applies a different UTC offset than the addon's built-in time zone table
  (a server with older or newer time zone data), the addon now notices from the occurrences Dispatcharr has
  already scheduled and uses the offset Dispatcharr applies, instead of recording an hour off. It looks only at
  occurrences still to come, accepts a difference of at most an hour, follows the zone's older rules instant by
  instant when that is what the server is doing (British Columbia and Alberta on a server with time zone data
  from before 2026c), and takes two checks in a row to agree before it changes anything.
- **Recurring timers**: on the one day a year a clock change falls between a rule's start and end time, the
  end time shown in Kodi's Timers list was an hour off (the stored rule was right).
- **Playback with an unresponsive server**: stopping live timeshift, a read waiting at the live edge and
  Kodi's startup each waited up to a full request timeout against a server that accepts connections and never
  answers; they now give up after about five seconds, authentication included (opening a stream still waits the
  configured timeout, so a slow server can start one). A recording whose file can no longer be read (removed,
  or refused by the server) now ends playback with a notification after a few seconds instead of retrying
  without end, and one segment Dispatcharr cannot size no longer makes the player re-check the whole backlog
  on every refresh or merge a long backlog one segment at a time. Stopping an in-progress recording while
  the server is unreachable waits seconds instead of minutes, and a proxy that drops `Range` no longer makes
  a recording read download the whole file before giving up. Opening a recording that has no file (stopped
  before anything was recorded) now says so instead of showing only a generic playback error. A recording read no longer ends for good
  on a few seconds of server errors (HTTP 500).
- **Recurring timers**
  - Times and weekdays are now converted correctly when Kodi and Dispatcharr are in different time zones,
    including around daylight-saving changes; British Columbia and Alberta (which stop changing their
    clocks on 2026-11-01) are handled.
  - An unchanged recurring rule is no longer re-sent, and its occurrences regenerated, every time it is
    edited; moving just the end time no longer shifts the start; seconds carried by Kodi's dialog are
    dropped; a rule with no end date can be edited again.
  - Moving a single occurrence of a recurring rule is refused (Dispatcharr would recreate the original slot),
    and rules the addon renews are marked so a rule you made in Dispatcharr is never renewed for you.
- **Series rules**: editing a rule no longer resets its hidden settings (title mode, description, EPG
  source) or deletes a source-pinned rule that shares its title; rules are linked to their recordings for
  every title mode Dispatcharr offers, in any script.
- **Recordings and timers**
  - Deleting or editing a timer that had already started (or already finished) no longer deletes the
    recording.
  - A recording whose title contains a slash no longer shows as a nested folder.
  - Pressing Record on a programme already under way now links the timer to the guide entry, with your
    padding applied.
  - A scheduled recording's channel change is no longer dropped.
  - Edits no longer overwrite a real title with a placeholder.
- **Playing a recording that is still being made**: opening one within its first seconds works; pausing past
  its end no longer loses the last minutes; a user Stop no longer cuts off the final segment; a recording
  deleted while you watch ends cleanly instead of hammering the server; one unreadable segment no longer
  stops playback.
- **Live timeshift**: a brief server outage no longer ends playback for good, a long pause no longer ends it
  when the server's rolling buffer moves on, and seeking forward at the live edge never moves backward.
- **Channels and guide**
  - Fractional channel numbers (5.1) and channels with no number get their own guide.
  - Two channels sharing a number no longer show each other's guide.
  - A server that was unreachable at Kodi's start no longer empties your channel list, and recovers by
    itself.
  - Catch-up is only offered when Dispatcharr allows it.
  - The guide now carries enough history for catch-up on a fresh install.
  - A guide entry's genre is no longer guessed from a keyword in the middle of a word, and a series rule whose
    title differs from a programme's only in capitals is matched to it.
  - A programme whose number is only given as "E12" (no season) now shows its episode number.
  - A channel group whose name is over 1023 bytes no longer shows as empty.
  - A series rule with nothing to record yet is no longer shown as 1 January 1970.
  - Choosing catch-up playback asks Dispatcharr for one session, not two, which starts it about 0.3 s sooner.
- **Account and security**
  - Wrong credentials, or an account whose role cannot use the API, are retried with a growing wait instead
    of on every request (which tripped Dispatcharr's own login rate limit).
  - A view-only Dispatcharr account is no longer offered timer, delete and rename actions it cannot use, and a
    finished recording keeps its link to its guide entry after the guide drops the programme.
  - A response with text that is not valid UTF-8 no longer terminates Kodi; the request fails cleanly.
  - The addon no longer replaces your Dispatcharr API key on first run or on a 401, which signed out every
    other client of the account.
  - The API key and login are never sent to another host, even through a redirect or a playlist that names
    one; a proxy that rewrites the WebSocket handshake is detected.
  - Oversized or malformed server responses are refused instead of exhausting memory.
- **Settings and shutdown**: the recording padding you set is no longer reverted by an older value, a saved
  API key is not lost while the settings dialog is open, and Kodi exits within seconds even while a request
  is hung.
- **Stability**: concurrent requests no longer risk crashing libcurl (the connection cache is no longer shared
  between threads), and text matching no longer misbehaves under a Turkish or other non-English character
  locale.
- **Packaging**: the add-on no longer declares an icon that was never shipped.

## `timeshift_buffer` [0.8.14] - 2026-10-09

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A change of the host's clock no longer makes the idle reaper stop buffers that are being watched.** A clock
  stepped forward by more than the idle timeout (a manual change, a VM resume, a clock correction) used to drop
  every viewer and end playback at once; a step backwards kept a really idle buffer, and its provider slot, until
  the clock caught up. Neither a clock step nor a suspend matters now. An idle buffer is reaped after the idle
  timeout plus up to 15 seconds.
- **One viewer stopping no longer drops the other viewers of the same buffer** after a clock step. A viewer that
  crashed without stopping still frees its provider slot within 15 seconds.

## `timeshift_buffer` [0.8.13] - 2026-10-06

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- The "help" link on the Plugins page points at the project's `Omega` branch (it named `master`, which the
  project no longer has).

## `timeshift_buffer` [0.8.12] - 2026-10-06

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **The file server can no longer be made to hold hundreds of megabytes by a client that sends no token.** A
  request whose headers run past 16 KiB (a real client sends under 1 KiB) is now dropped.
- **The per-client connection limit now works for IPv6**: a host with an IPv6 /64 could use a new source
  address for every connection and so never reach the limit; peers are counted by their /64 (an IPv4-mapped
  address by its IPv4 address).
- **Disabling or deleting the plugin while Redis is down** no longer leaves its file server listening in that
  worker until Dispatcharr restarts.
- **A single unreadable buffer entry in Redis** no longer stops the reaper, the orphan scrub and the list and
  stop-all actions; it is skipped and logged once. Listing buffers uses `SCAN` instead of `KEYS`, so it no
  longer blocks the Redis that also carries Dispatcharr's own traffic (a key `SCAN` returns twice is counted once).
- The orphan check compares a buffer's age on the same clock it was stamped with, and the `ffmpeg.log` trim
  uses the buffer's own storage path after the setting has changed.

## `timeshift_buffer` [0.8.11] - 2026-10-06

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **Clock differences between Dispatcharr workers no longer make buffers look idle.** The idle checks
  compared timestamps written by one worker with the clock of another, which differ when Redis runs on
  another host. They now use Redis's own clock, which all workers share. (This entry first said a clock
  stepped on the host itself was handled too; it is not, because Redis shares that clock: see 0.8.14.)

## `timeshift_buffer` [0.8.10] - 2026-10-05

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **"Max concurrent channel buffers" now holds across channels.** Two channels starting at the same moment each
  counted the same running buffers and could both start, going over the limit; counting and registering
  a new buffer is now one step, and a start that arrives during another one is told to retry in a moment.
- **A buffer's `ffmpeg.log` can no longer grow without limit.** A source that makes ffmpeg warn on every
  packet could add hundreds of megabytes a day; the log is trimmed to its last lines once it passes
  16 MB.
- The "Segment length" help text names the right ffmpeg option (`-hls_time`).

## `timeshift_buffer` [0.8.9] - 2026-10-05

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A Dispatcharr worker no longer keeps a cached playlist for every channel it ever served.** Each
  worker held one entry per channel (about 400 KB at the defaults) until the process ended, since only
  the worker that stopped a buffer dropped its entry; the cache is now limited to the 16 most recently
  used channels, and a dropped one is simply rebuilt on its next request.

## `timeshift_buffer` [0.8.8] - 2026-10-05

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **The manual test from the README can be followed again.** The file server answers 403 to
  any request without the buffer's own access token, and the Plugins page only shows an
  action's message, so a plain `http://<host>:<http_port>/<channel-uuid>/live.m3u8` always
  failed. "Start Test Buffer" now prints the playlist URL with its token, and the README says
  to use it. A buffer started by the Kodi addon never has its token printed.

## `timeshift_buffer` [0.8.7] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A step of the system clock no longer cuts short the two-second grace ffmpeg gets
  to exit before it is killed.** The grace now runs on a clock that cannot be set.
- **A Redis server that denies scripting to the plugin's user is handled.** Releasing
  the per-channel start lock and updating a buffer's state fall back to their
  non-script path for every "response error" Redis reports (a permission denial is a
  subclass of one), where the fallback only ran for the base class.
- **An infinite number in a numeric plugin setting falls back to the default** instead
  of breaking every action of the plugin.

## `timeshift_buffer` [0.8.6] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A request for `live.m3u8` that landed while ffmpeg was replacing it could get a
  truncated playlist.** The response size now comes from the file that was actually
  opened rather than an earlier look at the path.
- **Releasing a channel's start lock is now one atomic step** (where Redis allows
  scripts), so a lock that expired and was taken by the next caller can no longer be
  released by the previous one.

## `recording_edl` [0.2.3] - 2026-10-06

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- The "help" link on the Plugins page points at the project's `Omega` branch (it named `master`, which the
  project no longer has).

## `recording_edl` [0.2.2] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **`get_edl` no longer reads whatever file a recording's stored data names.** It only
  reads a regular `.edl` file that lies inside a DVR directory, never more than 1 MiB of
  it, and never opens a device or pipe. A path pointing at the recording's own video or at
  a device used to be read whole into memory until the Dispatcharr worker was killed.
- **`delete_orphaned_dvr_hls_dirs` re-checks each directory right before removing it**,
  so one that became live since the listing is left alone.

## `timeshift_buffer` [0.8.5] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **Stopping a buffer whose state had already been lost could remove a buffer started in
  the meantime.** That teardown now takes the channel's start lock and re-checks first.
- **A client draining a response body very slowly could hold a connection for far longer
  than intended.** A response body now has one overall deadline (the 60 second timeout
  plus the time the body needs at a 32 KiB/s floor) instead of a fresh timeout per chunk.
- **The idle-buffer reaper no longer stops for good if Redis is briefly unavailable when it
  starts,** and a Redis outage now logs one error per five minutes instead of one every
  fifteen seconds.

## `timeshift_buffer` [0.8.4] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **The orphan scrub could stop a buffer that had only just been started and
  delete its directory** (most likely right after Redis loses its state, when
  every buffer is an orphan at once). The scrub now takes each channel's start
  lock and re-checks it before touching anything.
- **The file server no longer serves `ffmpeg.log` or any file but the playlist
  and its segments, and sends responses in chunks** instead of reading a whole
  file into memory first.

### Documented

- A `storage_path` must not be shared between two Dispatcharr instances that can
  see each other's processes.

## `timeshift_buffer` [0.8.3] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A buffer whose state was lost (a Redis restart or flush while Dispatcharr kept
  running) no longer leaves its ffmpeg running forever, and the next start no longer
  runs a second ffmpeg into the same directory.** Each buffer's directory now records
  which ffmpeg owns it; the orphan scrub stops an untracked one that has been running
  for over five minutes, and a fresh start stops one before clearing the directory.
- **A large segment read slowly was cut off after about ten seconds** (0.8.1
  regression: the time left over from the request deadline became the write timeout).
- **One client address can no longer fill every connection slot** (64 per address of
  the 256), and refused connections are reported at most every 30 seconds.

## `timeshift_buffer` [0.8.2] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Changed

- **The access token placed on the buffer's ffmpeg command line now lasts two
  minutes instead of the account's default access lifetime.** The command line
  is readable by every local user through `/proc`, and ffmpeg authenticates
  only once, when it connects, so nothing needs the longer life.

## `timeshift_buffer` [0.8.1] - 2026-10-04

Plugin only. **Requires redeploying the updated plugin to Dispatcharr**, and a
Dispatcharr restart for every worker to pick it up.

### Fixed

- **A client that sent a request one byte at a time could hold a connection
  open indefinitely.** The file server now gives a request line plus headers
  one absolute 10-second deadline (a late client is disconnected), and
  caps concurrent connections at 256.

## `timeshift_buffer` [0.8.0] - 2026-10-02

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Changed

- **The buffer's ffmpeg now writes segments with the `hls` muxer instead of
  the `segment` muxer.** The `segment` muxer restarts every stream's MPEG-TS
  continuity counter in each file, and the addon splices the files into one
  byte stream, so every splice was a demuxer "Packet corrupt" line in Kodi's
  log (14 of 14 splices measured; none after). Same file names and playlist
  shape, so no addon change is needed; old segments are deleted rather than
  overwritten in place.

## `timeshift_buffer` [0.7.0] - 2026-10-02

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **A viewer paused or rewound behind live kept their rewind window when the
  buffer's ffmpeg died.** The first poll after ffmpeg exited used to tear the
  whole buffer down, wiping the segments that viewer could still have played.
  The manifest is now returned frozen and flagged `ended`, and the addon ends
  playback only once everything that was recorded has been played.

## `timeshift_buffer` [0.6.9] - 2026-10-02

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **A recycled process id is no longer mistaken for the buffer's own ffmpeg.**
  The buffer records its ffmpeg's start time and refuses to signal, or call
  alive, a process whose start time differs -- after a container restart the
  stored pid can belong to an unrelated process.

## `timeshift_buffer` [0.6.8] - 2026-10-02

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **Two requests landing together on a fresh worker could start two file
  servers or two reapers.** Starting either is now serialized, so the second
  caller finds the first one's instead of binding beside it.

## `timeshift_buffer` [0.6.7] - 2026-10-01

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **Buffer state updates no longer lose each other's writes.** Every
  read-modify-write of a buffer's state (heartbeats, viewer registration, the
  stopping marker) now goes through a compare-and-set, so a heartbeat can no
  longer overwrite a viewer another request just registered, or resurrect a
  buffer that was just torn down.
- **A plugin reload no longer leaves a stale file server and reaper running
  in every worker.** Threads left by an earlier load of the plugin are found
  and stopped on the next import.

## `timeshift_buffer` [0.6.6] - 2026-10-01

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **A fresh buffer no longer starts on top of a previous instance's leftover
  files.** A stale playlist with high sequence numbers used to stall a new
  viewer at the tail until the new numbers caught up, and an old directory
  timestamp exposed the fresh buffer to the orphan scrub.
- **A teardown decided from a stale copy of the state no longer destroys a
  buffer that was restarted meanwhile.**
- **The file server now binds a dual-stack IPv6 socket where one is
  available**, falling back to IPv4 only where there is no IPv6, so a client
  reaching Dispatcharr over IPv6 can reach the buffer's segments too.

## `recording_edl` [0.2.1] - 2026-10-01

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **The `.dvr_<id>_hls` staging-directory listing and cleanup now cover every
  directory a DVR path template points at**, not only the default
  `/data/recordings` root. Dispatcharr creates the staging directory beside the
  recording's final file, so an absolute template put it somewhere the scan
  never looked.

## `timeshift_buffer` [0.6.5] - 2026-09-29

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **A viewer pruned as stale during a long network stall is counted again on
  its next heartbeat** instead of being ignored for good, so its buffer is no
  longer torn down under it once every other viewer stops.
- **A client whose buffer was replaced by a newer one for the same channel
  is told so** (`fatal`) instead of being served the new buffer's unrelated
  segments.

## `timeshift_buffer` [0.6.4] - 2026-09-29

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **Two near-simultaneous `start_buffer` calls for one channel could each
  start their own ffmpeg, leaving one untracked forever.** A per-channel lock
  now serializes the start; the loser gets a retryable error and the addon
  retries briefly.

## `timeshift_buffer` [0.6.3] - 2026-09-29

Plugin only. **Requires redeploying the updated plugin to Dispatcharr.**

### Fixed

- **A paused viewer's own heartbeat went stale and could get its buffer torn
  down.** `get_live_manifest` -- the only call a paused client still makes --
  now refreshes that viewer's own heartbeat, not just the buffer-wide one.

## [0.11.0] - 2026-09-16

Addon only -- neither companion plugin changed for this pass.

### Added

- **Android support (arm/arm64).** Confirmed live end to end on two
  real physical devices, an older 32-bit ARM device and a newer 64-bit
  ARM device: real channel/EPG data, live playback, live
  timeshift seek (both Server-side and Local/`inputstream.ffmpegdirect`
  modes), recorded and in-progress recording playback and seek,
  catch-up playback, timer and recurring-rule creation, and real-time
  push updates. See [docs/BUILDING.md](docs/BUILDING.md)'s new Android
  section for build instructions and known limitations.

## [0.10.1] - 2026-09-15

Addon only -- neither companion plugin changed for this pass.

### Fixed

- **Channels with real, working catch-up/archive support were never
  actually marked as such to Kodi.** `GetChannels()` never set Kodi's own
  per-channel `hasarchive` flag, so it always reported `false` even
  though catch-up playback itself worked correctly once selected from
  the guide -- the channel-level flag Kodi (and, by extension, a skin's
  own catch-up-availability indicator) uses to know catch-up exists at
  all for a channel was simply never wired up. See
  [docs/CATCHUP.md](docs/CATCHUP.md) for the full root cause.

## [0.10.0] - 2026-09-11

Addon only, but this release also bundles the already-published
`timeshift_buffer` `0.6.2` security fix (see its own entry below) --
redeploy that plugin to Dispatcharr separately if you haven't already
applied it.

**The addon's Kodi id changed from `pvr.dispatcharr` to
`pvr.dispatcharr-unofficial`.** Same situation as the previous
`pvr.dispatcharrai` -> `pvr.dispatcharr` rename: Kodi treats an addon-id
change as a brand-new addon, not an in-place upgrade, so this is not a
seamless update -- install the new addon, copy your old `settings.xml`
over by hand (its format is unchanged), then disable the old addon once
the new one is confirmed working. See [README.md](README.md) for the
exact steps. The GitHub repository itself was also renamed to match
(`BruiserBrody17/pvr.dispatcharr-unofficial`); old repo URLs redirect
automatically.

The addon's name and description now explicitly say "Unofficial" and
state that this project isn't affiliated with, endorsed by, or
supported by the Dispatcharr project or by Kodi/Team Kodi -- visible in
Kodi's own add-on browser at install time, not just in this repo's own
docs.

## `timeshift_buffer` [0.6.2] - 2026-09-11

Plugin only -- the addon and `recording_edl` didn't change for this fix.
**Requires redeploying the updated plugin to Dispatcharr** for the fix to
take effect server-side.

### Fixed

- **Security: a caller-supplied `client_ip` on `start_buffer` was never
  validated before being interpolated into an HTTP header ffmpeg sends
  on its own connection to Dispatcharr's internal proxy.** A value
  containing embedded `\r\n` could smuggle a second, pipelined HTTP
  request onto that connection, appearing to originate from Dispatcharr's
  own loopback interface. Found via the same full-codebase security
  review that caught the `channel_uuid` path-traversal fix below.
  `client_ip` is now validated as a well-formed IP address before use;
  anything else is rejected (logged, buffer still starts normally
  without that attribution header) rather than used as-is. See
  `docs/TIMESHIFT.md`'s "client_ip header injection" section for the
  full write-up.

## [0.9.4] - 2026-09-10

Addon only -- neither companion plugin changed for this pass (a few of
this release's commits touch plugin doc/comment text for the rename
below, but not their behavior or version).

**The addon's Kodi id changed from `pvr.dispatcharrai` to
`pvr.dispatcharr`.** Kodi treats an addon-id change as a brand-new
addon, not an in-place upgrade, so this is not a seamless update:
install the new addon, copy your old `settings.xml` over by hand (its
format is unchanged), then disable the old addon once the new one is
confirmed working. See [README.md](README.md) for the exact steps. The
GitHub repository itself was also renamed to match
(`BruiserBrody17/pvr.dispatcharr`); old repo URLs redirect automatically.

### Fixed

- A series rule ("record all episodes"/"record only new episodes")
  could create successfully and report success, yet never actually
  schedule the upcoming episode. Root cause: a channel's own `tvg_id`
  can drift from what its effective (possibly overridden) EPG data
  actually resolves to, so the rule was silently created against the
  wrong EPG data. The addon now resolves the rule's tvg_id from the
  channel's real effective EPG data instead of the channel's own
  (possibly stale) field.
- A series rule's own row in Kodi's Timers list showed `12/31/1969` as
  its start/end time instead of a real date, since a series rule has no
  fixed schedule of its own. It now shows its earliest matched
  upcoming/in-progress recording's real time, and nests under it in
  Kodi's UI the same way a recurring rule already did.
- A recording's date badge could show a misleading, years-old date for
  a genuinely new episode of a long-running daily show -- the guide
  data's `<date>` field is a series-level placeholder for a show like
  that, not a real per-episode fact, when the guide provides no
  season/episode identifier alongside it. The addon now only shows a
  first-aired date/year when the guide data also identifies a real
  season or episode number.

### Changed

- A hot path during in-progress-recording playback that re-fetched and
  parsed every recording just to check one recording's status now
  fetches only that one recording instead.
- Recordings and timers are now cached for a short 2-second window
  (invalidated immediately on the addon's own writes) instead of being
  re-fetched from scratch on every one of Kodi's back-to-back
  count-then-list calls, cutting redundant REST traffic during normal
  browsing without adding any real staleness.

## [0.9.3] - 2026-09-10

Addon only, but this release also bundles the already-published
`timeshift_buffer` `0.6.1` security fix (see its own entry below) --
redeploy that plugin to Dispatcharr separately if you haven't already
applied it.

### Added

- Recording pre/post padding settings now grey out automatically when
  the configured Dispatcharr account isn't an admin -- pushing a
  padding change needs admin access, and Dispatcharr silently rejects
  the write otherwise, so this makes that limitation visible instead
  of a silent failure. Re-checked on every restart.
- A one-time startup warning if live TV pause/rewind
  (`live_timeshift_mode`) is set to Server-side but the configured
  account isn't an admin -- that combination fails live channel
  playback outright, not just timeshift, with previously nothing but a
  `kodi.log` line explaining why.

### Removed

- The "Extra padding for sports (minutes)" setting, and the automatic
  end-of-recording padding it applied to one-time recordings created
  from EPG entries tagged as sports. Originally added in `1.0.0`.

## `timeshift_buffer` [0.6.1] - 2026-09-10

Plugin only -- the addon and `recording_edl` didn't change for this fix.
**Requires redeploying the updated plugin to Dispatcharr** for the fix to
take effect server-side.

### Fixed

- **Security: a caller-supplied `channel_uuid` on any `run/` action
  (`start_buffer`, `stop_buffer`, `heartbeat`, `get_live_manifest`) was
  never validated as a real UUID before being used to build a
  filesystem path.** A value like `"../recordings"` could make
  `start_buffer`'s directory creation and a later `stop_buffer`/idle-
  reaper `shutil.rmtree()` operate entirely outside the plugin's own
  `storage_path`, deleting an arbitrary directory the Dispatcharr
  process can reach. Found via a full-codebase security review.
  `channel_uuid` is now validated as a well-formed UUID at the point
  every action resolves it, plus defensively wherever a filesystem path
  is actually built from it, in case a pre-fix Redis-stored buffer
  entry still carries an unvalidated value. See `docs/TIMESHIFT.md`'s
  "channel_uuid path traversal" section for the full write-up.

## [0.9.2] - 2026-09-09

Addon only -- neither companion plugin changed for this pass.

### Added

- Recordings can now be renamed from Kodi's own recordings list.
- Recordings now show their real file size.
- An in-progress recording can now be extended (add more time to a
  still-recording timer) directly from Kodi's Timers screen.
- The PVR backend version shown to Kodi is now Dispatcharr's real,
  live version instead of a placeholder string.
- Broader timezone coverage for the recurring-timer timezone setting
  (about twice as many zones now auto-detect their UTC offset,
  including daylight saving).

## [0.9.1] - 2026-09-08

Addon only -- neither companion plugin changed for this pass.

### Fixed

- The on-screen seek bar for server-side live TV pause/rewind didn't
  move when you seeked -- it kept climbing with real time regardless of
  where you'd rewound or fast-forwarded to, even though the actual
  playback position was correct. Now reflects the real position.
- The same seek bar bug, for playback of a recording that's still in
  progress.
- A rare but severe playback corruption (garbled video, audio badly out
  of sync, sometimes needing a restart) that some channels could hit
  after fast-forwarding all the way to the live edge. Fast-forwarding to
  live now leaves a little more buffer margin, which resolved it in
  testing.

## [0.9.0] - 2026-09-07

Versioning scheme change only -- no code changed in this entry. All three
pieces stepped back from `1.0.x` to `0.x`: addon `1.0.8` -> `0.9.0`,
`timeshift_buffer` `1.0.6` -> `0.6.0`, `recording_edl` `1.0.2` -> `0.2.0`.
This project is still single-user and still turning up real bugs in
testing, and depends on Dispatcharr, which is itself still pre-1.0
(`0.30.0`) -- `1.0.x` signaled more stability than actually existed. Per
SemVer, `0.x` means "still changing," which is the honest state of things
right now. Future `1.0.0` releases (for any of the three pieces) will
happen once that's genuinely true, not on a fixed schedule.

## [1.0.8] - 2026-09-07

Addon only -- neither companion plugin changed for this pass.

### Fixed

Three findings from a project-wide code review of the addon's full C++
source (following the same review already done for both companion
plugins), none reproduced live -- all defensive gaps caught by
deliberately re-checking the whole codebase for failure shapes already
found once elsewhere this cycle:

- The real-time-updates WebSocket client (`WebSocketClient::SendAll()`)
  had no overall deadline on a stalled/zombie connection, unlike its
  sibling read-side function -- could have silently hung the background
  update thread indefinitely. See `docs/API_NOTES.md`'s
  "`WebSocketClient::SendAll()` could hang this thread indefinitely"
  section.
- EPG broadcast IDs were computed from a channel id and only the low 16
  bits of each entry's start time, risking collisions between different
  programmes on the same channel roughly every 18.2 hours apart -- a
  real, non-negligible risk given how many entries a multi-day guide
  holds per channel. See `docs/EPG.md`'s "Broadcast IDs could collide
  across a channel's own EPG entries" section.
- An in-progress recording's HLS playlist duration parsing could hit
  undefined behavior (not a clean, catchable error) on a malformed
  `inf`/`nan` value. See `docs/RECORDINGS.md`'s entry on the same.

## `timeshift_buffer` [1.0.6] - 2026-09-07

Plugin only -- the addon didn't change for this fix, per the decoupled
versioning policy.

### Fixed

- A malformed `#EXTINF:` duration value (`inf`, not expected from
  ffmpeg in practice) in a channel's live playlist could fail the
  entire `get_live_manifest` fetch instead of just defaulting that one
  segment's duration to zero. Found via a follow-up audit prompted by
  an analogous fix in the companion `recording_edl` plugin (see below)
  -- every numeric conversion in this plugin was re-checked against the
  same failure shape. See `docs/TIMESHIFT.md`'s "A malformed `#EXTINF:`
  duration could fail the whole manifest fetch" section.

## `recording_edl` [1.0.2] - 2026-09-07

Plugin only -- the addon didn't change for this fix, per the decoupled
versioning policy.

### Fixed

- A single malformed line in a recording's `.edl` file (a `nan` or
  `inf` timestamp -- not expected from comskip in practice, but not
  something a parser reading a file should assume) could take down the
  entire `get_edl` result for that recording instead of just being
  skipped, showing zero commercial markers rather than whatever other
  entries were valid. Found via a comparative architecture review of
  the plugin (the same pass that reviewed `timeshift_buffer` earlier).
  See `docs/RECORDING_EDL.md`'s "A malformed `.edl` line could take
  down the whole result, not just itself" section.

## [1.0.7] - 2026-09-07

`timeshift_buffer` also bumped, to its own independent `1.0.5` -- this
release **requires redeploying the updated plugin to Dispatcharr** for
the plugin-side half to take effect.

### Fixed

- Live TV server-side timeshift: a second, distinct freeze from the
  1.0.6 fix below, reported immediately after upgrading -- playback
  could still stall permanently, this time within seconds of opening,
  with `ffmpeg`'s own demuxer logging `Packet corrupt`. Root cause: if
  the plugin ever reports a segment's byte size before a write has
  fully settled, this addon locks that size in permanently and every
  *later* segment's computed position silently drifts out of alignment
  with its real file for the rest of the session -- producing
  corrupted-looking playback with no error anywhere, since each
  individual read still "succeeds." Fixed at both ends: the plugin no
  longer trusts a cached size for the newest segment on any given call,
  and the addon now cross-checks the real size the file server reports
  on every read against what it has cached, treating any disagreement
  as an immediate, clearly-logged failure instead of silent corruption.
  See `docs/TIMESHIFT.md`'s "1.0.6 follow-up: a second, distinct freeze"
  section for the full investigation, including what was ruled out.
- `timeshift_buffer`: found via the diagnostic above actually catching a
  real, live disagreement -- switching away from a channel and back
  could still hit the same "wrong cached segment size" failure, this
  time because the plugin's own manifest cache wasn't invalidated
  reliably across a buffer restart in a multi-worker deployment (a
  channel's new ffmpeg process reuses the same segment filenames and
  sequence numbers as its previous instance, and a worker holding a
  stale cache entry from the old instance had no way to know it was
  gone). An initial fix tying cache entries to the buffer's process ID
  was itself confirmed live to have a gap under heavy testing churn --
  OS pids get recycled, and a stale entry tagged with a since-reassigned
  pid passed the check it should have failed. Fixed properly by keying
  on the buffer's own access token instead (already a fresh, random
  value per genuine instance, with no reuse risk regardless of churn).
  See `docs/TIMESHIFT.md`'s "1.0.7 follow-up #2" section, including the
  "pid-based version wasn't good enough" update.

## [1.0.6] - 2026-09-07

Addon only -- `timeshift_buffer` didn't change for this fix.

### Fixed

- **1.0.5 regression:** Live TV server-side timeshift could get
  permanently stuck in "buffering" and never recover, reproduced on the
  very first playback attempt after upgrading to 1.0.5. Caused by the new
  per-viewer heartbeat (added in 1.0.5) riding along on the same thread
  Kodi's own demuxer depends on for continuous reads, with no bound on
  how long that call could take -- an ordinary transient network hiccup
  at the wrong moment (roughly every 4 minutes, whenever the access token
  needed a routine refresh) could stack into minutes of blocking on a
  single read call, long enough to permanently stall playback even after
  the slow call eventually completed. Fixed by bounding the heartbeat to
  a short, fixed 2-second timeout and never letting it trigger a token
  refresh or re-login itself. See `docs/TIMESHIFT.md`'s "1.0.5
  regression: the new per-viewer heartbeat could stall playback
  permanently" section for the full root cause.

## [1.0.5] - 2026-09-07

Addon only. `timeshift_buffer` also bumped, to its own independent
`1.0.2` (per the decoupled versioning policy) -- the addon-side and
plugin-side halves below are paired, so this release **requires
redeploying the updated `timeshift_buffer` plugin to Dispatcharr** for
any of it to take effect server-side. All three items found via a
comparative architecture review of the plugin's own implementation, not
user reports or live incidents.

### Fixed

- Live TV server-side timeshift: a viewer that crashed (force-quit,
  network drop) without cleanly stopping could leave its own reference-
  count entry stuck on the shared buffer forever, silently preventing the
  *next* viewer's clean stop from actually tearing the buffer down --
  defeating the fast-teardown guarantee the whole viewer reference-
  counting design depends on. Fixed by tracking each viewer's own
  last-seen time separately instead of one buffer-wide heartbeat; the
  addon now pings the plugin with a per-viewer heartbeat every 10s while
  a stream is open. See `docs/TIMESHIFT.md`'s "A crashed viewer's own
  reference-count entry never got cleaned up" section.
- Live TV server-side timeshift: the plugin's own segment/playlist file
  server (a separate port from Dispatcharr's own, exposed the same way
  per the plugin's own setup instructions) had no access control at all
  -- anyone who could reach that port could read any channel's currently-
  buffered live segments with zero Dispatcharr credentials. Fixed by
  requiring a per-buffer access token, issued only via the already
  admin-gated `start_buffer` action. See `docs/TIMESHIFT.md`'s "The
  plugin's own file server had no access control at all" section.

### Changed

- `timeshift_buffer`'s `get_live_manifest` action no longer rebuilds its
  entire response from scratch on every call -- at this plugin's own
  defaults that was up to 1,800 `stat()` syscalls and a full playlist
  re-parse for a call that found nothing new, and it's called far more
  often than the buffer could possibly have grown. Now caches per-worker-
  process and only re-stats genuinely new segments. See
  `docs/TIMESHIFT.md`'s "`get_live_manifest` rebuilt its whole response
  from scratch on every call" section.

## [1.0.4] - 2026-09-06

Addon bumped to 1.0.4. Both companion plugins also bumped, to their own
independent `1.0.1` (per the decoupled versioning policy) -- not tied to
this addon release number.

### Changed

- Trimmed the longest addon-settings help texts (`live_timeshift_mode`,
  the recording-padding/recurring-timezone settings, sports padding,
  catch-up seek, real-time updates -- several were 800-1,260 characters,
  slow-scrolling walls of text in Kodi's help popup). Cut down to the
  essential decision-relevant info, with a pointer to the relevant
  `docs/*.md` file for anyone who wants the full explanation. Also fixed
  a rendering artifact (extra-looking spacing) in `live_timeshift_mode`'s
  help text caused by a repeated `" -- "` separator.
- `Recurring timer: manual timezone offset from UTC` is now greyed out
  and uneditable whenever `Recurring timer: timezone` is set to anything
  other than "Manual", since it has no effect in that case.
- Trimmed the longest help text in both companion plugins' own settings
  (`timeshift_buffer`'s idle-timeout and test-channel-UUID fields,
  `recording_edl`'s test-recording-ID field) the same way, for the same
  reason -- same content, tighter wording. Both plugins bumped to
  `1.0.1` for this (their own independent version, per the decoupled
  versioning policy -- the addon didn't change).

## [1.0.3] - 2026-09-06

Addon only.

### Fixed

- Live TV server-side timeshift: if the buffer died mid-playback (not
  just failing to start), playback froze indefinitely with no error --
  the addon kept silently retrying forever instead of recognizing the
  buffer was gone. Found via a comparative-architecture review against
  `pvr.hts`/Tvheadend. See `docs/TIMESHIFT.md` for the full root cause.
- If Kodi ever requested a second concurrent PVR instance, creating it
  could silently break settings-apply-live for the first one. Not known
  to have happened in practice -- hardened defensively. See
  `docs/API_NOTES.md`'s "Single-instance assumption" section.

### Added

- The optional real-time-updates WebSocket now reconnects immediately on
  an OS/device wake from sleep, instead of waiting out however much of
  its current (up to 60s) reconnect backoff was still left. See
  `docs/API_NOTES.md`'s "OS sleep/wake" section.
- A one-time recording created directly from an EPG entry Dispatcharr's
  guide data tags as sports can now get extra end-of-recording padding on
  top of the normal recording padding, since sports broadcasts commonly
  run long in a way scripted programming doesn't (new
  `sports_extra_padding_minutes` setting -- off by default, opt-in).
  Doesn't apply if the timer's end time has already been manually
  adjusted, or to recurring/series rules. See `docs/EPG.md`'s "Sports
  events get extra recording padding automatically" section (removed
  in a later release).

## [1.0.2] - 2026-09-06

Addon only, same as 1.0.1.

### Fixed

- **1.0.1 regression, macOS only:** opening an in-progress recording could
  crash Kodi outright (a real concurrency bug in macOS's own system
  libcurl, triggered by 1.0.1's concurrent segment-probing). Fixed by no
  longer sharing the connection cache specifically for that concurrent
  probe burst -- see `docs/RECORDINGS.md` for the full root cause. The
  speed fix from 1.0.1 is unaffected: still ~5-6s to open a multi-hour
  in-progress recording cold, near-instant on a reopen.
- A self-heal API-key regeneration during an in-progress recording's open
  or a completed recording's read could immediately kill the playback
  that had just started, prompting a spurious "needs to restart" dialog.
  Not a regression from the macOS crash fix above or the 1.0.1 speed fix
  below -- an older, separate bug this session's testing happened to
  surface. See `docs/RECORDINGS.md` for the
  full root cause.

## [1.0.1] - 2026-09-06

Addon only -- neither companion plugin changed, so neither's own version
moved (see `CLAUDE.md`'s versioning note: the addon and each plugin version
independently as of this release).

### Fixed

- Opening an in-progress recording got slower the longer it had already
  been recording, and re-paid that full cost on every open, not just the
  first -- a ~2h-in recording took 29.4s to open. Fixed by probing new
  segments' byte sizes concurrently instead of one at a time, and caching
  already-probed segments across opens for the same recording (see
  `docs/RECORDINGS.md` for the full root cause). Confirmed live: the same
  kind of open now takes ~4.7s cold, and well under a tenth of a second on
  a reopen.

## [1.0.0] - 2026-09-05

### Changed

- `live_timeshift_mode` now defaults to `Off` instead of `Server-side`.
  A fresh install with no admin account or `timeshift_buffer` plugin set
  up was hard-failing every live channel; `Off` plays live TV immediately
  with zero extra setup. Existing installs are unaffected -- this only
  changes what a brand-new profile starts with.
- Clarified in the docs (no behavior change): both companion plugins
  (`timeshift_buffer` and `recording_edl`) require a genuine Dispatcharr
  **admin** account -- a blanket restriction in Dispatcharr's own plugin
  API, not something specific to either plugin. Native channel/EPG/
  recording access does not need admin.
- Clarified in the docs (no behavior change): pushing a recording-padding
  change back to Dispatcharr also needs an admin account (reading the
  current value doesn't). A non-admin push currently fails silently --
  not yet fixed, just now documented.
- All three READMEs rewritten to be concise; engineering narrative moved
  into `docs/`.
- `timeshift_buffer` plugin tuning, found via real hardware/load testing:
  `segment_seconds` default lowered 6s -> 2s and `idle_timeout_seconds`
  default lowered 120s -> 30s (both for snappier catch-up and faster
  cleanup of abandoned buffers), and ffmpeg's SIGTERM-to-SIGKILL grace
  period on stop shortened 5s -> 2s. A buffer that can't start because a
  provider's own concurrent-stream limit is already exhausted now fails
  fast instead of hanging for a slow timeout.
- `recording_edl` plugin: diagnostic action results (e.g. the `.dvr_*_hls`
  cleanup actions) now surface through Dispatcharr's own result toast
  instead of a field nothing displayed; added a `test_recording_id`
  setting for easier manual testing from the Plugins page.

### Added

- **`Local` live-TV pause/rewind reintroduced** (`live_timeshift_mode`
  value `1`): real pause/rewind buffered entirely on the Kodi device via
  `inputstream.ffmpegdirect`, needing no Dispatcharr admin account and no
  server-side plugin. Fills the gap `Off`'s new default leaves for anyone
  who wants live pause/rewind without granting admin access.
- `recording_edl` plugin: orphaned `.edl`/`.logo.txt` sidecar file cleanup,
  and `.dvr_*_hls` staging-directory diagnostics plus cleanup of
  confirmed-orphaned ones.
- Recurring (day-of-week) timers now auto-compute their UTC offset for
  ~25 common timezones instead of requiring manual entry, staying correct
  across DST transitions.
- Both companion plugins (`timeshift_buffer`, `recording_edl`) are now
  packaged as downloadable zip assets on GitHub Releases, alongside the
  addon itself -- previously only the addon had a release zip.
- `inputstream.ffmpegdirect` declared as an optional addon dependency, so
  Kodi's own addon info reflects the relationship.

### Removed

- `timeshift_buffer`'s `snapshot_buffer` plugin action -- superseded by
  later fixes to seeking directly against the live buffer, so the
  workaround it existed for is no longer needed.

### Fixed

- Recording playback failing immediately after stopping a recording,
  while Dispatcharr was still finalizing the HLS-to-MKV concat in the
  background.
- Live-edge seek stall on in-progress (still-recording) playback.
- A crash-prone catch-up-retry calculation (could collapse to almost no
  retry budget on an unlucky short segment, ending playback outright) --
  previously fixed only for live-TV timeshift, now also applied to
  in-progress-recording playback, which had the same bug.
- An invalid XML comment that broke CoreELEC builds specifically (not
  caught by Windows/macOS/Linux builds, which don't validate addon.xml as
  strictly).
- README incorrectly claimed a second device joining an already-running
  server-side timeshift buffer could rewind into another device's earlier
  viewing history. Live-tested and found false: the underlying buffer
  *process* is genuinely shared per-channel, but each device's own
  rewind window is still capped to its own viewing session either way --
  corrected, with the accurate explanation moved into
  `timeshift_buffer`'s own README.

## [1.0.0-beta.3] - 2026-09-04

### Fixed

- Provider concurrent-stream-limit failures when switching or stopping a
  live channel.

## [1.0.0-beta.2] - 2026-09-04

### Fixed

- Concurrent live-timeshift viewers killing each other's buffers -- a
  second device opening the same channel could kill the first device's
  still-playing buffer outright.
- CI packaging so a tagged release actually gets its build zips attached
  (was silently broken).

## [1.0.0-beta.1] - 2026-09-04

Re-verified `0.4.0`'s fixes on real Linux hardware; no
functional changes of its own.

## [0.4.0] - 2026-09-04

### Fixed

- Recurring-rule flooding, a settings-restart quirk, and live-timeshift
  stability issues -- all found via real CoreELEC/ODROID N2+ hardware
  testing.

## [0.3.0] - 2026-09-03

### Added

- Timer editing in place (`UpdateTimer()`) for all timer types, instead
  of delete-and-recreate.
- Recording folder organization and global recording padding as an addon
  setting.
- Recurring (day-of-week) timer rules.
- Commercial-break markers (comskip EDL) on the recording seekbar.

### Changed

- Settings apply live instead of requiring a Kodi restart.
- Channel/EPG loading moved to a background thread instead of blocking
  Kodi's calling thread.
- Reintroduced `Off` as a `live_timeshift_mode` option.

### Fixed

- Data races on the JWT token pair and API key across concurrent threads.

## [0.2.0] and earlier

Initial development and scaffold -- this project's first tagged release.
