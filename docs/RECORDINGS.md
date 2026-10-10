*(part of the pvr.dispatcharr-unofficial notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Recordings/timers: confirmed end-to-end against real data

Once the account's permissions were raised (see the permissions note
below), a real recording and two real series rules were created, listed,
and deleted -- both directly over the API and through Kodi's own PVR
manager (`PVR.GetTimers`/`PVR.DeleteTimer` via JSON-RPC) -- confirming:

- **Enrichment ignores a channel's EPG override (Dispatcharr-side, 2026-10-02 note).** Dispatcharr's
  `prefetch_recording_artwork`/`run_recording` match a recording against `rec.channel.epg_data`, the channel's
  raw EPG link, not its effective/override one (`ChannelOverride.epg_data_id`, which this addon's guide
  already follows). Seen live on a channel whose raw and effective `epg_data_id` differed: a recording a day
  out never got a `title`/`sub_title`/`description`/`id` (the poster fell back to the channel logo) although
  Kodi's guide showed a programme in that slot, while one a few minutes out enriched within seconds -- most
  likely the raw source simply does not reach as far ahead (not confirmed; that needs the server's database).
  Nothing to fix here, and this addon's own recordings still resolve guide data through the effective chain;
  it is why a recording's title can stay blank until the programme itself is matched.
- A `Recording` created with **no** `custom_properties` gets auto-enriched
  by Dispatcharr itself from whatever EPG programme was actually airing,
  nested as `custom_properties.program.{title,sub_title,description}`
  (alongside `status`, `file_url`/`output_file_url` pointing at an
  in-progress HLS playlist, `file_name`/`file_path` for the eventual MKV,
  and `poster_logo_id`). Sending your own `custom_properties` on create
  **replaces** this entirely rather than merging with it -- confirmed by
  comparing a recording created with an explicit `custom_properties.title`
  (which got exactly that flat object back, nothing else) against one
  created with none (which got the full auto-populated object above).
  `CreateOneTimeRecording()` no longer sends its own `custom_properties`
  as a result, and `GetRecordings()` reads the nested `program.*` fields
  first, falling back to flat `custom_properties.title` etc. for anything
  that did set them directly.
  **Confirmed (this was previously flagged unconfirmed in
  `docs/TROUBLESHOOTING.md`): a recording that genuinely can't match any
  EPG programme gets no title-shaped field from Dispatcharr at all, ever
  -- not even a placeholder.** Created a real recording (no
  `custom_properties` sent) on a channel with `epg_data_id: null` (one of
  the provider's auto-created event-placeholder channels, which carry no
  EPG data whatsoever, so there's nothing to enrich from by construction,
  not just bad luck on timing). Checked its `custom_properties` at both
  `status: "recording"` and, after the channel's placeholder stream
  predictably had nothing to actually record,
  `status: "interrupted"` -- neither ever contained `program`, `title`,
  or anything else title-shaped; `file_name`/`file_path` were a bare
  timestamp (`19990101_000000.mkv`), not a channel- or title-derived
  name. So the pending-title cache and Kodi's own manual-timer title
  really are the *only* source of a title in this case, not just the
  more-visible one -- Dispatcharr's own data never independently agrees
  or disagrees, because it never expresses an opinion at all.
  Independently confirmed by reading Dispatcharr's own source, not just
  this one live test: `Recording` (`apps/channels/models.py`) has no
  `title` field at all, only `custom_properties`, and the EPG-enrichment
  matcher (`_match_epg_program_by_timeslot` in `apps/channels/tasks.py`)
  requires a programme covering at least 80% of the recording window --
  its own docstring calls out that a recording spanning multiple
  programmes with no dominant show, or matching none at all, "return[s]
  None (displayed as 'Custom Recording')". That string is purely a
  frontend display fallback (`frontend/src/components/cards/
  RecordingCard.jsx`), never written back to the Recording row --
  confirming an external API consumer (this addon included) never sees
  it, only an absent field.
- A series rule has **no numeric id field at all** -- a real one is just
  `{mode, title, tvg_id, channel_id, title_mode, description,
  description_mode}`. `GetTimers()` previously used `rule.id` (always 0)
  to build each series timer's Kodi `ClientIndex`, which would collide for
  any second series rule; now hashes `(title, tvgId)` instead -- confirmed
  with two simultaneous rules that they now show as distinct timers.
- Dispatcharr's `SeriesRuleRequest.mode` (`"all"` vs `"new"`, i.e. record
  every episode including reruns vs first-run only) wasn't exposed at all
  when creating a series rule -- it always used the server's `"all"`
  default. Kodi's PVR API has a purpose-built control for exactly this,
  `PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES` (paired with
  `PVRTimer::SetPreventDuplicateEpisodes()`), which surfaces as a normal
  "Prevent duplicate episodes: Record all episodes / Record only new
  episodes" field in Kodi's own Timer Settings dialog when creating an
  "Add timer" (series) rule from the guide. Wired up and confirmed
  end-to-end through that real dialog: selecting "Record only new
  episodes" and saving produced a rule with `"mode":"new"` on the server.
  `GetTimerRules()` also reads the field back for existing rules, so an
  already-created rule shows the right selection if inspected again.
  **Update: confirmed this does take effect server-side, now that
  `UpdateTimer()` is implemented** -- see the "UpdateTimer()" entry
  further down in this file.
- `DeleteSeriesRule()`'s title+tvg_id query-param delete and
  `DeleteRecording()`'s path-id delete were both confirmed to actually
  remove the item server-side (checked directly against the API after
  deleting through Kodi), not just update Kodi's local view of it.
- **Pressing Kodi's "Record" button on an EPG guide entry never actually
  created a recording** -- reported as: a brief delay, then a repeating
  "Switch / Record / Cancel" dialog that just re-appears no matter which
  button is pressed. Root cause confirmed against Kodi's own source
  (`xbmc/pvr/timers/PVRTimerInfoTag.cpp`, `CreateFromEpg()`): that code
  path requires a timer type with neither `PVR_TIMER_TYPE_IS_MANUAL` nor
  `PVR_TIMER_TYPE_IS_REPEATING` set. This addon's two timer types had
  `IS_MANUAL` (the one-time type, needed for Kodi's separate "new manual
  timer, no EPG event" flow) and `IS_REPEATING` (the series type) --
  neither qualified, so Kodi could never build a timer object for a plain
  "record this guide entry" press, regardless of anything the addon's own
  `AddTimer()` does (it was never being reached at all). Fixed by adding a
  third type (`kTimerTypeOneTimeEpgBased`) with neither flag; confirmed by
  actually driving Kodi's guide UI (context menu -> Record) end-to-end and
  watching a real Dispatcharr recording appear and start writing an MKV.
  The dialog itself is likely `CPVRGUIActionsTimers::AnnounceReminder()` --
  wasn't reproduced directly, but its button set is the only exact match
  in Kodi's source for those three labels with an auto-close countdown,
  and Kodi's UI paths documented above only ever show a plain one-button
  "Timer creation failed" dialog for this specific failure, so there may
  be a Kodi-version-specific difference in exactly which dialog surfaces
  it -- the underlying missing-timer-type cause and its fix are confirmed
  either way.
- **A new recording never showed up under Recordings until Kodi was
  restarted.** `AddTimer()` called `TriggerTimerUpdate()` but never
  `TriggerRecordingUpdate()` -- Kodi has no reason to re-poll
  `GetRecordings()` on its own just because a timer was added, and a
  recording that starts at or near "now" (including Kodi's "Record"
  button on a live guide entry, see above) may already be actively
  recording by the time `AddTimer()` returns. Confirmed by waiting 90+
  seconds after creating a recording with the old code -- it never
  appeared until a full restart. Now calls both triggers.
- **A completed recording still showed as "Recording &lt;id&gt;" instead
  of its real title, indefinitely -- even after the recording finished.**
  Confirmed directly against the server for a real, fully-completed
  recording: `custom_properties.program.title` was correct there the
  whole time. The problem is on Kodi's side: Dispatcharr only populates
  `custom_properties` a moment *after* a recording actually starts (right
  at creation it's `{}`), but nothing prompts Kodi to look at a *already
  known* recording's metadata again once it's cached it -- confirmed
  nothing else about the recording being displayed differently at any
  point in its life re-triggers a refetch, so whatever `GetRecordings()`
  returned on Kodi's very first look (our `"Recording <id>"` fallback,
  since enrichment hadn't happened yet) stuck around forever, completed
  recording or not. The `TriggerRecordingUpdate()` call in `AddTimer()`
  fired immediately, before that enrichment window, which is exactly
  what caused Kodi's first look to be too early. Fixed with a second,
  delayed (5s) `TriggerRecordingUpdate()` call for one-time recordings,
  giving Dispatcharr time to populate the title before Kodi's next
  fetch. If you already have a recording stuck showing "Recording
  &lt;id&gt;" from before this fix, its title is genuinely correct
  server-side already -- a plain Kodi restart will pick it up, no need
  to touch anything on Dispatcharr's side.
- **Stopping an in-progress recording from Kodi deleted it entirely
  instead of just stopping it -- confirmed to have actually destroyed a
  real recording, not just a theoretical risk.** Root cause, confirmed
  against Kodi's own source (`xbmc/pvr/timers/PVRTimers.cpp`): Kodi's
  `DeleteTimer()` addon call takes a `forceDelete` flag that specifically
  means "this timer is still actively recording" -- both the dedicated
  "Stop Recording" action and choosing "Delete" on a timer Kodi already
  knows is recording pass `forceDelete=true`; a timer that's merely
  scheduled (not yet started) passes `false`. This addon's `DeleteTimer()`
  ignored that flag entirely and always called `DeleteRecording()` --
  `DELETE /api/channels/recordings/{id}/`, which "removes the associated
  file(s) from disk" per its own description -- regardless of whether the
  recording was still being written. Confirmed against the live schema
  that Dispatcharr has a separate, purpose-built endpoint for exactly
  this: `POST /api/channels/recordings/{id}/stop/`, documented as "Stop a
  recording early while retaining the partial content for playback."
  Fixed: `forceDelete=true` now calls `StopRecording()` (the `/stop/`
  endpoint) instead. Verified end-to-end through Kodi's real "Stop
  recording" UI action (not just a direct API call): the recording
  remained listed afterward with real bytes written, `remux_success:
  true`, and a normal (non-HLS) `/file/` URL -- fully playable, exactly
  as documented -- and that a genuinely non-recording timer's delete
  (`forceDelete=false`) still correctly removes it entirely via
  `DeleteRecording()`.
- **A completed recording that finished within Kodi's own stale-timer-cache
  window could still be deleted entirely instead of just having its
  now-stale Timers entry drop off, found via a project-wide review
  (2026-09-26), not itself independently reproduced.** `forceDelete`
  (see the bullet above) comes from Kodi's own *cached* copy of a
  timer's state, only refreshed by this addon's own `GetTimers()` -- up
  to `recording_refresh_minutes` stale (5 minutes by default, up to 60).
  A separate, earlier fix (`dispatcharr::ShouldStopInsteadOfDelete()`,
  `RecordingVisibility.h`) already closed the "recording started within
  that stale window" half: a fresh server-side lookup at delete time
  correctly routes to `StopRecording()` even when Kodi's own cache still
  thought nothing was recording yet. But a recording short enough to
  both *start and finish* inside one `recording_refresh_minutes`
  interval (any recording shorter than that setting, e.g. a 30-minute
  show against the default 5-minute refresh) can still show as a
  SCHEDULED timer to Kodi's own stale cache even though the server-side
  recording already completed normally -- `forceDelete=false` and the
  fresh lookup's own `isInProgress=false` (correctly, it's done) both
  routed to `DeleteRecording()`, permanently deleting the just-completed
  recording's real, wanted content, not merely cancelling a timer that
  never ran. Fixed by checking, before that existing Stop-vs-Delete
  decision, whether the fresh lookup confirms the recording is neither
  in progress nor upcoming at all (`dispatcharr::IsAlreadyFinishedRecording()`,
  same header) -- when true, `DeleteTimer()` does neither: nothing to
  stop or delete, Kodi's own next refresh naturally drops the now-stale
  Timers entry once this addon's own cache catches up.
- **Update (2026-09-26): the exact same stale-cache window applies to
  `UpdateTimer()` too, the missed counterpart of the fix above, found via
  a project-wide review, not itself independently reproduced.** An edit
  on an already-finished recording caught in that window used to fall
  into `UpdateTimer()`'s own "not yet started" branch, which PATCHes
  `start_time`/`end_time` via `UpdateOneTimeRecording()` -- resending an
  `end_time` already in the past, which Dispatcharr's own
  `RecordingSerializer.validate()` explicitly rejects (`end_time < now`,
  already confirmed live -- see `UpdateOneTimeRecording()`'s own
  comment). That failed even a harmless title-only edit outright, since
  `RenameRecording()` was never reached once that PATCH failed first.
  Fixed the same way: `IsAlreadyFinishedRecording()` checked before the
  existing not-yet-started branch, and when true, only a rename applies
  -- any other field Kodi's dialog might have changed (start/end time)
  is silently ignored, since there's nothing left to reschedule.

  **Update (2026-09-26, a 21st-pass audit): a second, distinct
  stale-cache window hits the same "not yet started" branch the same
  way, found via a project-wide review, not itself independently
  reproduced.** A recurring occurrence stuck at `status=="scheduled"`
  forever (see `IsMissedOccurrence()`'s own comment, `RecordingVisibility.h`)
  is already excluded from Kodi's Timers list -- but if Kodi's own
  cached copy of it was fetched before that occurrence's window fully
  elapsed (a slower client refresh, a timer-edit dialog left open), an
  edit on it still reaches `UpdateTimer()` with a genuinely stale
  `rec.isUpcoming=true` looking fresh. Same failure as above: it falls
  into the "not yet started" branch, PATCHes an already-past `end_time`,
  and Dispatcharr rejects it. Fixed the same way -- `IsMissedOccurrence()`
  checked as its own branch before the existing not-yet-started one
  (deliberately *not* folded into `IsAlreadyFinishedRecording()` itself,
  since `DeleteTimer()` also uses that function and widening it would
  make Delete a silent no-op for a missed occurrence a user is actually
  trying to remove) -- only a rename applies.
- Stopping a recording early leaves its `end_time` at the originally
  *scheduled* value -- Dispatcharr doesn't rewrite it to the actual stop
  time, only `custom_properties.stopped_at` reflects that. `isInProgress`
  was computed purely from `start_time <= now < end_time`, so a
  recording stopped well before its scheduled end kept showing as
  actively recording in Kodi's timer list for the entire remainder of
  that original window, even though `custom_properties.status` was
  already `"stopped"` and the file was already complete and playable.
  Confirmed end-to-end (stopped a real in-progress recording via Kodi,
  it kept showing as a timer). Fixed by trusting `custom_properties.status`
  over the time window when present: `"recording"` means in-progress,
  any other non-empty value means finished, matching the confirmed
  values (`"recording"`/`"completed"`/`"stopped"`/`"interrupted"`)
  without assuming that's a closed set. Also added a
  `TriggerRecordingUpdate()` after a successful stop/delete (previously
  only `TriggerTimerUpdate()`), for the same reason `AddTimer()` needed
  one: the change affects the Recordings view too, not just Timers.

  **Update (2026-09-26, a 26th-pass audit): the same untouched `end_time`
  also left the recording's own *duration* wrong, not just its
  in-progress status, confirmed against Dispatcharr's own real current
  upstream source (cloned into a scratchpad, never committed to this
  repo -- stronger than the API shape alone, not the same standard as a
  live test).** `ParseRecordingFields()`'s own `durationSeconds` was
  computed from `end_time - start_time` unconditionally -- so a
  recording stopped 10 minutes into a scheduled hour still reported a
  full hour's duration in Kodi's recordings list and info panel, and its
  JSON-RPC `endtime` stayed in the future. Fixed the same way as the
  in-progress fix above: `custom_properties.stopped_at`
  (`"YYYY-MM-DD HH:MM:SS.ffffff+00:00"`, already parseable by
  `TimeFromIso()`'s own fixed-digit-position parse, indifferent to the
  `T`-vs-space separator -- *update, 2026-09-26, a 37th-pass audit:*
  `TimeFromIso()` now also reads and applies a trailing offset (a
  35th-pass fix), but `stopped_at`'s own `+00:00` is a
  genuine zero, so this still parses the same real instant either way)
  recomputes `durationSeconds` as `stopped_at - start_time` whenever it parses to a
  sane value strictly between `start_time` and `end_time`. Deliberately
  scoped to `status == "stopped"` only (not `"interrupted"`, which is
  set from several different code paths server-side with less certain
  per-instance timing semantics -- not chased this pass).
- **A recording that *did* show up under Recordings still failed to
  play, silently ("Error creating demuxer" in the log, no player ever
  started).** This took real digging, and an earlier note in this file
  claiming in-progress recording playback worked end-to-end was wrong --
  it did once, but wasn't actually reproducible, and the real mechanism
  turned out to be different from what that note assumed. Confirmed
  against Kodi's own source
  (`xbmc/cores/VideoPlayer/DVDInputStreams/DVDFactoryInputStream.cpp`):
  any `pvr://recordings/...` path is demuxed through
  `CInputStreamPVRRecording`, which calls the addon's
  `OpenRecordedStream()`/`ReadRecordedStream()`/`SeekRecordedStream()`/
  `LengthRecordedStream()` -- **but only if `GetRecordingStreamProperties()`
  leaves `PVR_STREAM_PROPERTY_STREAMURL` unset.** An earlier version of
  this note claimed STREAMURL is *never* consulted for a recording and is
  harmless to populate regardless; that turned out to be wrong -- a real
  failure (see the API-key note below) was root-caused to Kodi's generic
  `CCurlFile` opening a populated STREAMURL directly, bypassing these
  callbacks (and their retry logic) entirely, confirmed via a live
  `kodi.log`. This addon never implemented those callbacks originally, so
  Kodi's default
  `OpenRecordedStream()` (`return false;`) meant every single recording
  playback attempt failed immediately, with no network request even
  attempted. Fixed by implementing real byte-range HTTP reads
  (`DispatcharrClient::OpenRecordingStream()`/`ReadRecordingStream()`/
  `SeekRecordingStream()`/`GetRecordingStreamLength()`) against
  `/api/channels/recordings/{id}/file/`, using the `X-API-Key` header
  (see the permissions note below for why that's required at all).
  Confirmed end-to-end against a real completed recording: real playback
  progress, correct duration, and working seeks (verified via
  `Player.Seek`).
  **In-progress recordings are not supported by this byte-range fix** --
  `/file/` redirects to an HLS playlist (`.../hls/index.m3u8`) while a
  recording is still being written, and each individual `.ts` segment
  inside that playlist independently requires the same `X-API-Key`
  header, which Kodi's own HLS demuxer has no way to know to send for
  segments it discovers by parsing the playlist itself.
  `OpenRecordingStream()` detects this case (by content-type/URL) and
  fails with a clear error instead of trying and silently corrupting
  playback. See the separate `inputstream.ffmpegdirect`-based path below
  for how this is actually solved when opted into.

  **Update (2026-09-26, a 22nd-pass audit): a completed recording's own
  file can still change size out from under an already-open
  `ReadRecordingStream()`, confirmed against Dispatcharr's own real
  current upstream source (cloned into a scratchpad, never committed to
  this repo -- stronger than the API shape alone, not the same standard
  as a live test), not reproduced live.** Dispatcharr's own
  `comskip_process_recording()` runs automatically right after a
  recording finishes (when comskip is enabled), and its own "cut" mode
  replaces the original file with a shorter, commercial-trimmed one
  in-place (`os.replace()`, or a non-atomic `shutil.copy()` fallback) --
  reachable while a client is already mid-playback if it started
  watching the recording right after it finished, before comskip (which
  can take minutes) is done. `ReadRecordingStream()` had no cross-check
  against this at all, unlike the live-timeshift read path's own
  analogous guard -- it kept using the length cached when the stream was
  opened against the new, shorter file's actual bytes at the same
  offsets, silently reading the wrong content with no error anywhere.
  Fixed with new `dispatcharr::HasRecordingFileChanged()`
  (`RecordingHttpUtil.h`), comparing each read's own fresh
  `Content-Range` total (via the same `RecordingHeaderCallback` already
  used by `OpenRecordingStream()`'s own probe) against the cached
  length, failing the read outright on a disagreement rather than
  risking silent corruption.
- Both endpoints above (recording file and the HLS redirect target) also
  confirmed to return a flat **403 for a fully anonymous request**,
  despite their schema listing anonymous access (`{}`) as one of the
  allowed security schemes -- a Bearer token or `X-API-Key` header is
  actually required. A JWT access token expires after 30 minutes
  (confirmed by decoding one -- `exp - iat` -- see the login note above),
  too short for most recordings, so this addon obtains a Dispatcharr
  API key on first use and persists it to its own `api_key` setting --
  reading the account's *existing* key via `GET /api/accounts/api-keys/`
  and only calling `POST /api/accounts/api-keys/generate/` when the
  account has none (it used to generate unconditionally; fixed
  2026-09-30, see `docs/OPEN_ITEMS.md`). Regenerating replaces the
  account's previous key (confirmed: two calls returned two different
  keys) -- **account-wide, not per-installation**, which matters once
  more than one Kodi install (or any other script/tool) shares the same
  Dispatcharr account; see "Known limitations with more than one Kodi
  client" in `docs/TROUBLESHOOTING.md` for the self-heal this addon does
  about a key another client replaced.
- **In-progress recording playback, via `inputstream.ffmpegdirect`
  (`enable_inprogress_playback` setting, off by default, experimental).**
  Confirmed query-param auth is **not** a usable alternative to the
  `X-API-Key` header for the HLS segment endpoints (`?api_key=` and
  `?X-API-Key=` both got a flat 403; only the real header works), so
  fixing this needed something that could attach a header to every
  segment fetch, not just the manifest. `inputstream.ffmpegdirect`'s
  plain pass-through mode does that -- confirmed by reading its actual
  source, not just its docs -- but two details matter, both found by
  reading `FFmpegStream.cpp` directly rather than guessing from the two
  already-reverted attempts elsewhere in this addon (live TV's
  `stream_mode: timeshift` and catch-up's `timeshift`/`catchup`, both
  reverted for *seeking* reasons that don't apply to a forward-only,
  still-growing in-progress recording):
  1. A plain `http(s)://` URL defaults to `inputstream.ffmpegdirect`'s
     `OpenWithCURL()` code path, not `OpenWithFFmpeg()` -- confirmed via
     source that `OpenWithCURL()` sets no header options at all when
     opening the format context, silently reproducing the exact same
     segment-auth failure this was meant to fix. Must explicitly set
     `inputstream.ffmpegdirect.open_mode=ffmpeg` to force the code path
     that actually calls `GetFFMpegOptionsFromInput()`.
  2. Even in FFmpeg-native mode, `GetFFMpegOptionsFromInput()` only maps
     a fixed allowlist of standard HTTP header names to real headers --
     anything else (including `X-API-Key`) is silently dropped unless
     prefixed with a literal `!`, which it strips before using the rest
     as the header name. Confirmed by a real failed attempt logging
     `ignoring header option 'X-API-Key'` with the plain name, and a
     second attempt with `!X-API-Key` succeeding.
  No `stream_mode` is set at all (neither `timeshift` nor `catchup`) --
  the recording's own HLS playlist is already a valid, correctly
  segmented structure; the only actual gap was header propagation to
  segments, not anything either specialized mode addresses.
  `GetRecordingStreamProperties()` checks the recording's live
  `isInProgress` status (via `GetRecordings()`) before taking this path
  at all; a completed recording is unaffected and still goes through
  `OpenRecordingStream()`/etc. as before.
  Verified end-to-end against a real in-progress recording: real video
  rendering (confirmed via screenshot, not just JSON-RPC state), playback
  time advancing in real time, and over a minute of continuous playback
  with no stalls.

  **Follow-up attempt (`is_realtime_stream=false`) turned out to be
  incomplete -- it fixed the advertised seek *capability* but not the
  actual join *position*, and the earlier "verified working" claim below
  was wrong.** Original theory, confirmed by reading `FFmpegStream.cpp`
  directly: `GetCapabilities()` only advertises
  `INPUTSTREAM_SUPPORTS_SEEK`/`PAUSE`/`ITIME` when `is_realtime_stream` is
  false, and Kodi's `CVideoPlayer` only performs its normal "seek to the
  requested start position on open" behaviour when seeking is advertised
  as supported. Fixed by setting both `PVR_STREAM_PROPERTY_ISREALTIMESTREAM`
  and `inputstream.ffmpegdirect.is_realtime_stream` to `false`. This part
  held up: seeking genuinely works once this is set (see below).

  What didn't hold up: the "starts at the true beginning" verification.
  It was checked only via Kodi's own JSON-RPC `Player.GetProperties`
  (`time`/`percentage`), which display position *relative to wherever the
  stream happens to begin*, not relative to the recording's true absolute
  start -- so a demuxer that joins near the live edge still reports
  `time: 0:11` right after open, because Kodi labels wherever playback
  starts as "0". That's not evidence of anything; it's what Kodi always
  shows at the start of any stream. The real join point is only visible in
  ffmpegdirect's raw `av_dump_format` log line
  (`Duration: N/A, start: <seconds>, ...`), which wasn't checked at the
  time.

  Caught when the user reported the bug still happening on a currently-
  recording game, re-tested live, and that dump line read
  `Duration: N/A, start: 9681.617944, bitrate: N/A`. Cross-checked against
  the recording's real `start_time` from Dispatcharr's API
  (`a placeholder timestamp`) versus wall-clock time at the moment of the
  test (the following day): elapsed time since recording start was
  ~2h40m (9600s), matching the logged `start: 9681.6` almost exactly.
  **libavformat's HLS demuxer is joining the still-growing (no-
  `#EXT-X-ENDLIST`) playlist at the current wall-clock live edge,
  independent of `is_realtime_stream`.** That property only ever
  controlled ffmpegdirect's *advertised* seek capability, never
  libavformat's own automatic join-point selection for a no-`ENDLIST`
  playlist (governed by its own `live_start_index` option, confirmed
  earlier -- see the `GetFFMpegOptionsFromInput()` note above -- to have
  no reachable passthrough through any property this addon can set).
  Whichever recording happened to be tested when this was first "verified"
  most likely also joined near its own live edge; it just wasn't caught
  because the only check was Kodi's relative-position display.

  The only known way to stop libavformat from applying live-edge-join
  logic at all is to make the playlist look like a complete, closed VOD
  list -- i.e., inject `#EXT-X-ENDLIST` into a copy of the playlist before
  handing it to ffmpeg. VOD-shaped HLS is always demuxed from segment 0
  with a full seek range, sidestepping `live_start_index` entirely rather
  than trying to override it (there is no property this addon can set
  that reaches it directly, confirmed via `GetFFMpegOptionsFromInput()`'s
  source). Real, accepted trade-off: once marked `ENDLIST`, ffmpeg treats
  the list as complete and stops polling for newly-appended segments, so a
  single playback session no longer tails the recording live -- catching
  up on brand-new content needs a stop/replay to re-fetch a fresh, larger
  snapshot (and per the resume-point finding above, that replay starts
  over from position 0 rather than where the last session left off).

  **First implementation attempt -- a `data:` URI built from a one-time
  fetch-and-rewrite of the playlist -- failed outright, and not for a
  reason specific to this addon or ffmpegdirect.** Implemented
  `GetInProgressRecordingStreamUrl()` to fetch the live playlist itself,
  rewrite every segment reference to an absolute URL (a data: URI has no
  base path for a relative reference to resolve against), append
  `#EXT-X-ENDLIST`, base64-encode the result, and hand ffmpegdirect
  `data:application/vnd.apple.mpegurl;base64,<payload>|!X-API-Key=<key>`
  as STREAMURL. `kodi.log` confirmed the rewrite itself worked correctly
  (the base64 payload decodes to a well-formed playlist with absolute
  `http://` segment URLs and a trailing `#EXT-X-ENDLIST`), but ffmpegdirect
  logged `Error, could not open file data:application/...` immediately.
  Root-caused by reading Kodi's own `CURL::Parse()` (`xbmc/URL.cpp:72`):
  it hard-requires the literal substring `"://"` to recognise a protocol
  at all (`strURL.find("://")`) -- a standard `data:` URI, correctly per
  RFC 2397, has no `"://"` anywhere in it, so Kodi's parser never
  recognises it as a protocol and falls into a `.zip`/`.apk` archive-path
  fallback that just treats the whole string as a literal filename
  instead. This isn't a struct size limit (`INPUTSTREAM_PROPERTY`'s
  `m_strValue`/`m_strURL` are plain `const char*`, not fixed buffers --
  checked and ruled out first) or anything ffmpeg-side -- it's that
  `PVR_STREAM_PROPERTY_STREAMURL`'s pipe-delimited
  `url|option=value` convention is built entirely on top of Kodi's own
  generic `CURL` class, which cannot represent a bare `data:` URI at all.
  **A `data:` URI is therefore not viable through this property, full
  stop -- not just for this addon, for any Kodi PVR/inputstream addon
  using STREAMURL this way.**

  **Implemented and confirmed working: a tiny local HTTP listener inside
  this addon's own process (`LocalPlaylistServer`), serving the rewritten
  playlist at `http://127.0.0.1:<port>/playlist/<id>.m3u8` instead of a
  data: URI.** A real `http://` URL parses through Kodi's `CURL` class
  exactly like the original live one did. Loopback-only, OS-assigned
  ephemeral port (`bind()` to `INADDR_LOOPBACK`, port `0`, then
  `getsockname()` for the actual port); started in `PVRDispatcharr`'s
  constructor only when `enable_inprogress_playback` is on, stopped in the
  destructor -- no listening socket held open for installs that never use
  this feature. Raw sockets, not curl (curl is client-only and can't
  listen): platform-conditional `winsock2.h`/`ws2tcpip.h` vs.
  `sys/socket.h`/`unistd.h`, same pattern as `WebSocketClient.h`, reusing
  the `ws2_32` link already added for that. Single connection at a time,
  no keep-alive -- a fresh connection per request is simpler and
  libavformat's HLS demuxer doesn't need one to reload a playlist
  repeatedly.

  First shipped version served one pre-computed snapshot per recording
  (`SetPlaylist()`, called once from `GetRecordingStreamProperties()`),
  fixing the join-position/seek problem at the cost of a session never
  tailing new segments recorded after it started -- reopening got a
  fresh, larger snapshot, but not the same session continuing to grow.
  **Superseded by a dynamic, per-request design (`SetPlaylistProvider()`)
  that fixes that too, confirmed working.** Read directly from
  libavformat's own `hls.c` (not guessed) to find the mechanism:
  `select_cur_seq_no()`'s live-edge-join computation --
  `FFMAX(pls->n_segments + live_start_index, 0)`, `live_start_index`
  defaulting to `-3` -- only ever runs on the very first segment
  selection, and clamps to the true first segment whenever the playlist
  has 3 or fewer segments listed *at that moment*, regardless of how much
  has actually been recorded. Separately, as long as a playlist never
  claims `#EXT-X-ENDLIST`, the same file's reload logic
  (`!pls->finished` gating a reload-interval check) keeps re-fetching it
  throughout playback on its own -- the actual mechanism newly-recorded
  segments get picked up by, entirely independent of the one-time join
  decision.

  `DispatcharrClient::FetchInProgressPlaylistSnapshot(recordingId,
  truncateForInitialJoin, error)` (renamed from
  `GetInProgressRecordingStreamUrl()`) is now called fresh on every HTTP
  request `LocalPlaylistServer` receives for that recording, not once at
  open: `truncateForInitialJoin` (true only for that recording's actual
  first request, tracked by the server) caps the rewritten playlist to 3
  segment entries to force the clamp above to land on the true first
  segment; every request after that gets the full, untruncated history.
  Whether to finally append `#EXT-X-ENDLIST` is decided fresh on every
  call too, from a live `GetRecordings()` check of the recording's
  current `isInProgress` state -- not fixed at open time -- so a session
  that's still open when the underlying recording actually finishes
  correctly transitions from "keep tailing" to "reach a clean end" on its
  own, without needing to be reopened. `PVRDispatcharr` registers a
  provider lambda wrapping this (still handling the api-key-persist
  dance the old one-shot code did), and still attaches `!X-API-Key` as a
  pipe-option on the outer STREAMURL for ffmpegdirect's own segment
  fetches, exactly as before.

  **The "revealing full history from the second request onward is safe"
  claim above turned out to be wrong, and was shipped on insufficient
  evidence -- corrected here, along with the actual fix.** Original
  verification checked this addon's own `isFirstRequest=1, lines=13,
  hasEndlist=0, containsSeg00000=1` log line (proving what this addon
  *served*) and Kodi's relative `Player.GetProperties` time display, but
  never rechecked the one genuinely conclusive signal used earlier in
  this file -- ffmpegdirect's own `av_dump_format` `start:` value -- for
  this specific design. That gap hid a real bug for weeks of testing on
  short (1-3 minute) recordings, where "true position 0" and "wherever
  the demuxer actually landed" were too close together to visibly
  distinguish. A user testing against an 11+ minute recording caught it
  cleanly: playback consistently showed different content on every
  attempt, all matching whatever was live at that moment. Rechecking the
  raw `start:` line confirmed it precisely --
  `start: 118.931` on a recording that was ~2 minutes old at open,
  `start: 433.875` at ~7.2 minutes, `start: 734.182` at ~12.2 minutes --
  matching the recording's current age each time, not 0, despite this
  addon's own logging correctly showing every single first request as
  truncated-and-starting-from-`seg_00000`.
  Root cause: `select_cur_seq_no()`'s live-edge join computation, while
  documented as a one-time operation on the very first segment selection,
  can in practice get re-applied several times across libavformat's own
  rapid reloads while it's still probing/settling in right after open. A
  first request capped to 3 segments correctly forced the *first*
  application of that computation to land on segment 0 -- but the second
  request revealing the *entire* history at once (jumping from 13 lines
  to sometimes 300+) meant that if the computation got re-applied again
  before probing settled, it would use that much larger count and land
  far from 0 instead.
  Fixed by growing the revealed segment cap gradually instead of jumping
  straight to the full history on request two: `LocalPlaylistServer`
  tracks a small, growing per-recording cap
  (`kInitialMaxSegments`/`kMaxSegmentsGrowthStep`, both 3) instead of a
  one-time `isFirstRequest` boolean, and `FetchInProgressPlaylistSnapshot()`
  takes that cap directly as an `int` rather than a bool. Every reload's
  segment count now stays close to the previous one's, so no matter how
  many times the join computation actually gets re-applied during the
  settling window, it can never land far from wherever it last was.
  A second bug turned up applying this fix, caught before shipping by
  deliberately testing the finish-transition case again: applying the
  still-growing cap *unconditionally* combined badly with appending
  `#EXT-X-ENDLIST` once a recording finishes -- if the cap hadn't yet
  caught up to the true segment count when the recording ended, the
  response would falsely declare an artificially truncated prefix (e.g.
  57 of several hundred true segments) "the complete file," cutting
  playback off at ~2 minutes into what was actually a much longer
  recording. Fixed by only applying the cap while still in progress; once
  finished, the cap is ignored and the full true history is revealed in
  the same response that finally appends `ENDLIST` (safe unconditionally,
  since a finished/`ENDLIST`-terminated playlist takes hls.c's simple
  `return pls->start_seq_no` path and never reaches the live-edge
  computation at all).
  Re-verified end-to-end with the actual conclusive signal this time:
  against a ~10-minute-old recording, `start: 14.013` (not ~600s);
  against a ~19.5-minute-old one, `start: 13.829` (not ~1170s). Confirmed
  continuous, gapless playback throughout via repeated `Player.GetProperties`
  polling against wall-clock elapsed time. Confirmed the finish transition
  separately: stopping the underlying recording mid-session produced a
  response with the full ~170-segment true history (not capped) alongside
  `hasEndlist=1`, and playback continued normally past where the old,
  buggy version would have cut off.

  **Real, accepted trade-off, and different from the one-time-snapshot
  version's trade-off:** a still-growing (no-`ENDLIST`) playlist reports
  an unknown duration to libavformat (`Duration: N/A` in its own
  `av_dump_format` line, versus a real finite value once `ENDLIST`
  finally appears), and per the duration-metadata finding elsewhere in
  this file, Kodi's own PVR layer appears to gate `canseek` on having a
  known total duration independent of what the inputstream addon
  advertises -- confirmed live (`canseek: false` throughout an actively-
  tailing session in this round of testing, where the prior static-
  snapshot version's `canseek: true` came from always presenting a
  finite, `ENDLIST`-terminated duration immediately). Kodi also queries
  `GetCapabilities()` once, at open, and doesn't re-query it mid-session
  -- so even though the *same* session correctly reaches a clean end once
  the recording finishes and `ENDLIST` appears, it doesn't retroactively
  gain seek support for whatever's left of that session; only a fresh
  `Player.Open()` after the recording has actually finished gets normal
  VOD treatment with seek. In short: this version trades seek-while-still-
  recording for not needing to stop and reopen to keep watching new
  content -- the opposite trade-off from the one-time-snapshot version it
  replaced, not a strict improvement on every axis.

  **Revisited once the join-position bug above was fixed, to check whether
  seek could now also be recovered without giving up live-tailing --
  confirmed this is a genuine, inherent architectural trade-off, not
  something left to fix.** Investigation initially chased a promising
  alternative theory: a freshly-created recording's PVR-level `runtime`
  can briefly show a tiny placeholder value (`5` seconds observed) rather
  than its real scheduled duration, self-correcting a while later once
  Dispatcharr's own async EPG-matching settles it (confirmed directly:
  the same recording read `runtime: 5` moments after creation and
  `runtime: 2400` -- matching its real ~40-minute scheduled length -- when
  checked again later). This raised the possibility that every earlier
  `canseek: false` result during live-tailing had been confounded by
  testing against recordings still carrying that placeholder, rather than
  reflecting the live-tailing design itself.
  Ruled out by testing again against a recording confirmed to already
  have its correct, settled PVR-level duration (`runtime: 1252`, sane) at
  the moment of open: `canseek` was still `false`. The placeholder-
  duration behaviour is real (worth fixing or at least being aware of
  separately, since it can misrepresent a recording's length in Kodi's UI
  for a while after creation) but is not what gates seek during live
  playback.
  Traced the real mechanism instead by reading `FFmpegStream`'s handling
  of stream times directly: it only populates start/end time information
  when `!IsRealTimeStream()` (always true here, since `is_realtime_stream`
  is set to `false`), but the end time it reports is
  `m_pFormatContext->duration` -- which stays unknown for as long as
  libavformat's HLS demuxer doesn't know the playlist is finished, i.e.
  for as long as `#EXT-X-ENDLIST` is withheld to keep live-tailing
  working. So `GetCapabilities()` genuinely does advertise
  `INPUTSTREAM_SUPPORTS_SEEK` throughout -- the addon-level capability
  flag was never the blocker -- but Kodi-core, receiving that
  capability alongside an unknown/invalid total duration, correctly
  declines to actually offer seeking: there's no way to seek to a
  percentage or timestamp of a length that isn't known.
  This is a hard architectural conflict, not a bug: an HLS demuxer's
  notion of duration is derived from summing the durations of every
  segment *up to whatever the playlist currently lists as complete*, and
  that concept is fundamentally incompatible with "duration unknown
  because more might still be appended," which is exactly what
  live-tailing depends on. Getting both simultaneously -- seek while a
  recording is still actively being written -- isn't achievable within
  this ffmpeg/libavformat-based approach; the two require contradictory
  answers to "does this stream have a known end."

  **The same root cause also rules out Kodi automatically resuming
  mid-session playback of a still-in-progress recording, confirmed by a
  direct test, not just inferred.** Played an in-progress recording for
  ~25 seconds (of a recording with well over 1000 seconds left on its
  schedule -- nowhere near naturally ending), then stopped it via
  `Player.Stop` -- a genuine mid-playback interruption, not the
  natural-end-of-file case documented earlier in this file.
  `PVR.GetRecordingDetails` afterward showed the exact same outcome as
  that natural-EOF case: `playcount: 1`, `resume: {position: -1.0}` --
  marked fully watched, no bookmark saved at all -- and reopening landed
  back at true position 0, not ~25 seconds in. Kodi's own
  save-a-bookmark-vs-mark-watched decision on stop is a comparison
  against the total duration (how far in, as a fraction of the whole,
  counts as "essentially finished" vs. "still partway through") -- and
  that duration is unknown for exactly the same reason seeking doesn't
  work, so Kodi can't tell a 2%-in stop from a 98%-in one and appears to
  default to treating any stop as complete. Combined with the earlier,
  separately-confirmed finding that there is no Kodi-exposed way to
  write an arbitrary resume point for a `pvr://` path at all
  (`Files.SetFileDetails` fails unconditionally for that scheme), this
  means there is currently no way -- automatic or manual -- to have a
  session resume from where an earlier one left off while the underlying
  recording is still being written. The only two options while still in
  progress are: start over from the true beginning each time (current
  behaviour), or track the position yourself outside Kodi and seek to it
  manually -- which itself doesn't work either, per the seek finding
  above.

  **Since seek and live-tailing are permanently mutually exclusive per
  playback session (not just currently unimplemented together), added an
  explicit choice instead of picking one behaviour for everyone: pressing
  Play on an in-progress recording showed a blocking selection dialog
  ("Play live" vs. "Play from start (seek)") before
  `GetRecordingStreamProperties()` returns, and the answer decided which
  of the two designs above that playback session used.** (Superseded a
  few commits later by the context-menu-based design described further
  down this section, once cancelling this dialog turned out to always
  trigger Kodi's own "Playback failed" report -- kept here as the
  as-shipped history of how the choice was first implemented and verified,
  not the current behaviour.) Confirmed safe
  to call `kodi::gui::dialogs::Select::Show()` -- a synchronous, blocking
  call -- directly from inside that callback: it's invoked directly in
  response to the user pressing Play, the same circumstance Kodi's own
  native resume-point prompt already blocks in.
  "Play live" registers the existing gradual-cap `SetPlaylistProvider()`
  callback unchanged. "Play from start" instead calls a new one-shot
  method, `DispatcharrClient::FetchInProgressRecordingSeekableSnapshot()`
  (shares its actual HTTP fetch-with-401-retry logic with
  `FetchInProgressPlaylistSnapshot()` via a small private helper,
  `FetchRawInProgressPlaylist()`), which calls `RewritePlaylist()` with no
  segment cap and `appendEndlist` forced true unconditionally --
  deliberately skipping the gradual-cap dance the live mode needs
  entirely, since an always-ENDLIST-terminated response is unconditionally
  safe to reveal in one shot (`pls->finished=true` takes hls.c's simple,
  always-start-at-0 path, never reaching the live-edge join computation
  the cap exists to bound) and libavformat never reloads a finished
  playlist anyway, so this mode's provider is in practice only ever
  invoked once per session regardless of how it's implemented. Cancelling
  the dialog (`Select::Show()` returning `-1`) returns `PVR_ERROR_FAILED`
  from `GetRecordingStreamProperties()` -- confirmed live this cleanly
  aborts opening with no player started, rather than falling back to
  either mode silently.
  Verified all three paths live: choosing "Play live" reproduced the
  already-established live-tailing behaviour exactly
  (`maxSegments`/`hasEndlist=0` diagnostic logging, `canseek: false`);
  choosing "Play from start" gave a real known `totaltime` (`2:10`) and
  `canseek: true`, and an actual `Player.Seek` to 1:00 succeeded and
  continued playing correctly afterward; cancelling produced
  `Player.GetActivePlayers: []` -- no player started at all -- confirmed
  via `kodi.log` showing a clean `CVideoPlayer::CloseFile()` rather than
  a hang or crash.
  New localised strings `#30042`-`#30044` (dialog heading, the two option
  labels) in `strings.po`; no new settings.xml entries -- this is a
  per-playback choice, not a persistent preference, and only appears at
  all when `enable_inprogress_playback` is already on.

  **A real user report caught a second, more subtle bug in the gradual-cap
  fix above: "Play live" could still start ~12 seconds into the recording
  instead of at true position 0, specifically on the very first playback
  attempt after the mode-choice dialog was added.** Reproduced precisely
  via the same `av_dump_format` `start:` signal used throughout this
  investigation: `start: 13.427` for "Play live" vs. `start: 1.427` for
  "Play from start" on the same recording, back to back -- an exact
  3-segment (12-second) offset, not a vague "somewhere off." Root cause:
  the gradual-cap fix above grows the cap starting from the very first
  request (`kInitialMaxSegments + kMaxSegmentsGrowthStep` on request two),
  and it turns out even *one* step of growth is sometimes enough for a
  second application of `select_cur_seq_no()`'s live-edge join computation
  -- still occurring within libavformat's settling window, just one reload
  later -- to use the grown 6-segment count instead of the original
  3-segment one, landing `FFMAX(6-3,0)=3` segments (12s) off 0 instead of
  0. Fixed by holding the cap at `kInitialMaxSegments` for several requests
  (`kHoldRequestsAtInitialCap`, 4) before allowing any growth at all --
  `LocalPlaylistServer` now tracks a per-recording request count rather
  than a next-cap value, and a small `ComputeMaxSegments(requestIndex)`
  helper derives the cap from it. Growth only starts once re-application of
  the join computation is no longer occurring in practice, going by the
  margin observed above the single re-application actually caught.
  Re-verified end-to-end after the fix, same methodology: "Play live" and
  "Play from start" opened back to back against the same in-progress
  recording now both report `start: 1.400000` -- identical, not offset --
  confirming the join computation landed on true position 0 for "Play
  live" this time.

  **The mode-choice dialog itself was removed and replaced with a
  context-menu design, which also eliminates the "Playback failed" dialog
  above as a side effect rather than working around it.** Root cause of
  that dialog (confirmed via Kodi-core source, not guessed):
  `CPVRPlaybackState::StartPlayback()` calls `GetRecordingStreamProperties()`
  but never actually checks its `PVR_ERROR` return value -- it only
  inspects whether any stream properties were set. Returning
  `PVR_ERROR_FAILED` on cancel (as the dialog-based design did) was
  therefore indistinguishable, from Kodi-core's point of view, from any
  other kind of failure to produce stream properties: with nothing to
  open, `CVideoPlayer::CloseFile()` sets `m_error = true` (since this
  wasn't a user-initiated stop, `m_bCloseRequest` is false), which fires
  `OnPlayBackError()` and, via `GUI_MSG_PLAYBACK_ERROR`, Kodi's generic
  `HELPERS::ShowOKDialogText` "Playback failed" dialog (strings
  #16026/#16027). There is no cancel-safe value in Kodi's `PVR_ERROR`
  enum, and no separate "user cancelled, don't report an error" signal
  available to a PVR client addon at this call site -- an architectural
  gap in Kodi-core's PVR playback path that can't be worked around from
  inside `GetRecordingStreamProperties()` alone. Fixed at the design level
  instead, per explicit user direction, once presented with the trade-off:
  plain Play on an in-progress recording no longer prompts at all -- it
  goes straight to "Play from start" (the seekable one-shot snapshot,
  matching what the earlier dialog's default-highlighted option already
  was) -- and "Play live" moved to a `PVR_MENUHOOK_RECORDING` context-menu
  entry (`CallRecordingMenuHook()`) instead of a second dialog option.
  With no dialog on the Play path at all, there's nothing left to cancel
  and no way to trigger the "Playback failed" report.

  A binary PVR addon has no API to start playback itself, though (no
  `PlayMedia`/`ExecuteBuiltin`-equivalent exposed to `kodi::addon::CInstancePVRClient`
  -- confirmed by reading through `kodi-dev-kit`'s `AddonToKodiFuncTable_kodi`
  general-purpose function table, which has nothing playback-related), so
  the menu hook can't just open the item live directly the way selecting
  the old dialog's option did. It arms a single pending-recording-id flag
  (`m_pendingLiveModeRecordingId`) instead and shows a
  `kodi::QueueNotification` telling the user to press Play now;
  `GetRecordingStreamProperties()` consumes it (one-shot, whether or not
  it actually matches the id being opened) the next time it's called, and
  falls back to "Play from start" otherwise. Also confirmed via source
  (`PVRContextMenus.cpp`'s `PVRClientMenuHook::IsVisible()`) that
  `PVR_MENUHOOK_RECORDING` has no per-item visibility hook back to the
  addon -- it shows on every recording's context menu indiscriminately,
  completed ones included -- so `CallRecordingMenuHook()` re-checks
  `isInProgress` itself and shows a different, explanatory notification
  (without arming anything) when invoked on a recording that isn't
  actually in progress.

  Verified live end-to-end, including the specific failure this replaced
  a first, broken attempt at: plain `Player.Open` on an in-progress
  recording went straight to `Fullscreen video` with zero dialogs,
  `canseek: true` and a real `totaltime` (confirming "Play from start" by
  default, matching the design). Selecting "Play live" from the context
  menu on that same recording, confirmed present in the menu via GUI
  navigation, then pressing Play again produced `canseek: false`, no
  `totaltime`, and the gradual-cap `hasEndlist=0` diagnostic log line seen
  earlier in this file -- confirming the arm/consume flag actually
  switched modes correctly. Selecting "Play live" on a *completed*
  recording instead queued the explanatory notification and armed nothing,
  confirmed by tracing `id`/`inProgress` through a temporary diagnostic
  log line before removing it. One real bug caught and fixed mid-verification:
  an initial live A/B run through this exact same sequence appeared to show
  the arm silently failing (a second `Player.Open` also came back
  "Play from start"-shaped) -- diagnostic logging on both
  `CallRecordingMenuHook()` and the consuming side in
  `GetRecordingStreamProperties()` showed the ids actually matching
  correctly once added, so the first run's failure is attributed to GUI
  focus landing on a different one of the two identically-titled test
  recordings than intended, not a real logic bug -- flagged here rather
  than asserted with full confidence, since the diagnostic run that would
  have proven that explanation conclusively wasn't repeated.

  Two secondary things noticed along the way, neither investigated
  further this session: the placeholder-duration behaviour described
  above (Dispatcharr-side, not confirmed against its own source, but
  consistent with the API_NOTES entry on this addon's own periodic-
  refresh design existing partly to smooth over exactly this kind of
  post-creation correction); and the real-time-updates WebSocket
  appearing not to reconnect after a Dispatcharr outage-and-recovery
  during this investigation (only one "connected" log line the whole
  session, from well before the outage) -- if confirmed as a real gap in
  the reconnect-on-drop logic (as opposed to, say, the connection
  surviving the outage fine and just not having anything new to report),
  it would mean an addon install stays silently on the periodic-refresh-
  only fallback until restarted, worth a dedicated look later.

  **Update (2026-09-26): the dedicated look happened, via a project-wide
  review, not a repeat of this exact live session.** This was a real gap,
  not the connection surviving fine -- see `docs/API_NOTES.md`'s own
  "OS sleep/wake and the real-time-updates WebSocket" section for the
  full mechanism (a half-open connection that never receives a FIN/RST
  is indistinguishable, from this thread's own perspective, from a
  healthy one with nothing new to report) and the fix (TCP keepalive).
  Not yet re-confirmed against a fresh live outage-and-recovery test.

  **The macOS-vs-Windows seek discrepancy investigated earlier is probably
  not a real platform difference -- more likely the same class of
  duration-metadata issue described next, not yet re-tested under that
  hypothesis.** `canseek` on Windows tracked whether Kodi had a sane,
  already-known recording duration: `true` against a long-established
  recording with a normal scheduled runtime, `false` against a recording
  whose duration Kodi's PVR data showed as an obviously-wrong ~6 seconds
  (see below) even though Dispatcharr's real `end_time` gave a normal
  ~3h duration -- suggesting Kodi-core gates seek permission on having a
  known total duration, independent of whatever the inputstream addon
  advertises. The macOS clean-room test that ruled out caching used a
  *brand-new* recording created via the API specifically for that test --
  exactly the kind of recording most likely to still be carrying a
  placeholder duration at open time (see below). Version mismatch and
  property-plumbing were still correctly ruled out as explanations, but
  "genuine macOS platform gap" was likely the wrong conclusion; worth
  re-testing on macOS against a long-established recording with a
  known-correct duration before trusting that conclusion further.

  **Confirmed via live testing: a natural end-of-file during in-progress
  playback gets marked "watched" with no resume bookmark at all, and
  there is no way to manually correct this through Kodi's exposed API.**
  Reproduced live: started a fresh recording, waited 5 minutes (confirming
  the live-edge-join bug above also applies to a very short recording --
  the join point, `start: 349.86`, landed almost exactly at the 5-minute
  mark, since with only ~5 minutes of content total there's barely any
  "behind the live edge" room to join into), then stopped the recording
  early to let it finalize and let playback run to a genuine end.
  `kodi.log` showed a clean `CVideoPlayer::Process - eof reading from
  demuxer` / `OnPlayBackEnded` (not an error, not a user-initiated stop),
  followed immediately by `CSaveFileState::DoWork - Marking video item
  ... as watched`. `PVR.GetRecordingDetails` afterward confirmed
  `playcount: 1`, `resume: {position: -1.0, total: 0.0}` -- fully watched,
  no bookmark, even though only a few minutes of real content ever
  existed. Reaching a clean EOF, as opposed to a user-initiated
  `Player.Stop`, is what triggers this "fully watched" classification.

  Tried the obvious fix -- `Files.SetFileDetails` to write an explicit
  `resume: {position, total}` directly -- and it fails unconditionally for
  any `pvr://` path, confirmed architecturally, not just by trial and
  error: `FileOperations.cpp`'s `SetFileDetails()` gates on
  `CFileUtils::Exists(file)` before doing anything else, which calls
  through to `CFile::Exists()` -- and `xbmc/filesystem/FileFactory.cpp`
  explicitly returns `nullptr` for the `pvr://` protocol
  (`else if (url.IsProtocol("pvr")) return nullptr;`), meaning Kodi's
  generic VFS layer has no file handler for PVR paths at all. Verified
  this is the actual cause (not a malformed request) by testing
  progressively simpler calls -- even `{file, media}` alone, and even
  against a deliberately fake path, produced the identical
  `-32602 Invalid params` -- and by checking Kodi's own JSON-RPC error
  codes confirm permission failures (`BadPermission`) are a distinct code
  from this, ruling out a permission-tier explanation instead.

  Net conclusion: there is currently no Kodi-exposed way to directly edit
  a PVR recording's resume point to an arbitrary value. The only way to
  get an accurate bookmark is to stop playback yourself (via
  `Player.Stop`, or the normal "stop" remote/GUI action) *before* it
  reaches a genuine end-of-file -- Kodi's ordinary mid-playback stop
  bookmark-save behaviour does still work for PVR paths (that's the same
  mechanism the normal "resume from where you left off" prompt already
  relies on); it's only the JSON-RPC *write* path that's blocked for
  `pvr://`. This has a real, if awkward, workaround for the in-progress-
  playback UX problem described above: whatever eventually opens/manages
  this playback should stop the player a little before it would naturally
  hit the end of the current snapshot, rather than letting it run out on
  its own.

  **Separately-noticed: a recording's Kodi-visible duration can be stuck
  far too low (6 seconds observed against a real ~3-hour scheduled game)
  even though Dispatcharr's own `start_time`/`end_time` for that same
  recording are correct.** `TimeFromIso()` parsing was checked directly
  against the real API response and is correct (handles both the `Z` and
  `+00:00` suffix styles fine). The theory originally floated here --
  "Dispatcharr sets a short placeholder `end_time` at creation and extends
  it shortly after via EPG matching, and this addon's refresh thread
  hasn't caught the correction yet" -- **is refuted, confirmed by directly
  reading Dispatcharr's own source, not just re-guessed.** `Recording.
  end_time` (`apps/channels/models.py`) is a required, non-nullable field
  with no default; every one of the (exactly two) server-side creation
  paths sets a real value up front, and the plain manual/one-off path
  (the generic `RecordingViewSet.create()`) requires the *client* to
  supply both `start_time` and `end_time` -- there is no "record now,
  fill in the real end time later" mechanism anywhere in the source.
  `end_time` only ever changes afterward via the explicit, user-triggered
  `POST .../extend/` action, or an offset-reschedule task that only
  touches recordings already anchored to an EPG programme and only while
  still in the future -- neither is "a short-lived placeholder silently
  self-correcting soon after creation," and EPG-matching itself
  (`_match_epg_program_by_timeslot`) only ever updates
  `custom_properties.program`'s title/description fields, never
  `start_time`/`end_time`.
  **Much better fit, given the refuted theory pointed at exactly this
  symptom shape (correct backend duration, a small stuck Kodi-side
  value): this is very likely the same `m_streamDetails`/stream-details-
  caching bug already documented in `docs/TROUBLESHOOTING.md`'s "Known
  Kodi-core quirks" section**, which produces precisely this signature --
  Kodi's own probed-duration cache winning over the correct value this
  addon reports on every call -- and was root-caused there by reading
  Kodi's own source (`CVideoInfoTag::GetDuration()` preferring
  `m_streamDetails.GetVideoDuration()` unless it's under 60% of the
  addon-supplied duration), not by a Dispatcharr-side data problem at all.
  That entry has since been re-verified live as no longer reproducing
  under the current native-demuxer in-progress-recording mechanism, which
  narrows this passage's original "not chased further" status considerably
  even without a fresh dedicated repro of this exact 6-second case.
  Gap found by a companion session's real multi-install testing: unlike
  `OpenRecordingStream()`/`ReadRecordingStream()`, there's no way to
  self-heal a stale API key *after the fact* here -- the URL (with the
  key baked in) is handed to a separate addon process once, with no
  401-retry hook the way this addon's own HTTP client has. Fixed with a
  proactive check instead of a reactive one:
  `GetInProgressRecordingStreamUrl()` does a cheap live probe (a tiny
  ranged GET, mirroring `OpenRecordingStream()`'s own probe) against the
  exact URL it's about to build, and regenerates the key first if that
  comes back 401, before ever baking it into the URL ffmpegdirect will
  use standalone. `PVRDispatcharr` persists the regenerated key the same
  way it does for the other two paths. Adds one extra request before
  in-progress playback starts; accepted as a fair trade for closing a gap
  that's already been hit repeatedly in real testing across two
  installs sharing one account.
- **A recording/timer deleted (or otherwise changed) with no local Kodi
  action to react to it kept showing in Kodi indefinitely -- a "phantom"
  recording only a full Kodi restart would clear.** Root cause: every
  `TriggerRecordingUpdate()`/`TriggerTimerUpdate()` call in this addon is
  reactive, firing only right after this addon's own `AddTimer()`/
  `DeleteTimer()`/etc. -- there was no periodic check independent of local
  activity, unlike the lazy staleness check channels/EPG already have.
  Anything that changed the recordings list another way (a different Kodi
  install sharing the account, a direct Dispatcharr API call, a recording
  finishing on its own) had nothing to prompt Kodi to notice. Fixed with a
  background thread (started in the constructor, cleanly joined in the
  destructor) that calls both triggers every `recording_refresh_minutes`
  (default 5, configurable) regardless of local activity. Verified
  end-to-end: created a recording directly via Dispatcharr's API (not
  through this addon, matching how the phantom was actually produced
  during testing), confirmed it appeared in Kodi, deleted it directly via
  the API again, confirmed it was still showing immediately afterward
  (reproducing the bug), then confirmed it disappeared on its own after
  the refresh interval elapsed with no Kodi restart.
- **Real-time recording/timer updates (`enable_realtime_updates` setting,
  off by default, experimental) -- no Dispatcharr plugin needed at all.**
  The periodic refresh above is still a poll; asked to look at genuine
  push instead, confirmed by reading Dispatcharr's own source
  (`dispatcharr/consumers.py`, `dispatcharr/asgi.py`,
  `dispatcharr/jwt_ws_auth.py`, `core/utils.py`) that its backend already
  runs a real Django Channels WebSocket server at `ws(s)://host:port/ws/`,
  authenticated by the *exact same JWT access token* this addon already
  obtains via `/api/accounts/token/` (passed as a `?token=` query
  parameter -- confirmed the token is only checked once, at connect time,
  by `JWTAuthMiddleware`, so an already-open connection keeps working past
  the token's own 30-minute expiry). This is the same channel Dispatcharr's
  own frontend uses, not something added for this addon -- no plugin, no
  server-side change, nothing to install or enable on the Dispatcharr side.
  Confirmed (by reading `apps/channels/tasks.py`/`api_views.py`) that every
  recording lifecycle event this addon cares about is already broadcast on
  it, wrapped as `{"type": "update", "data": {..., "type": "<event>",
  ...}}`: `recording_started`, `recording_ended`, `recording_stopped`,
  `recording_extended`, `recording_updated`, `recording_cancelled`,
  `recordings_refreshed`.
  Implemented as a hand-rolled minimal RFC 6455 client
  (`src/WebSocketClient.h`/`.cpp`) rather than using libcurl's own native
  WebSocket support (`CURLOPT_WS_OPTIONS`/`curl_ws_recv()`), which needs
  curl >= 7.86 (added October 2022) -- the prebuilt Windows curl this addon
  links against (see `docs/BUILDING.md`) is 7.67.0, and Linux/macOS builds
  link whatever system libcurl happens to be installed, not guaranteed to
  have it either. Built instead on `CURLOPT_CONNECT_ONLY`, a much older,
  stable curl feature that hands over a connected (and, for `wss://`,
  already TLS-terminated) socket and lets the caller speak whatever
  protocol it wants over `curl_easy_send()`/`curl_easy_recv()` -- works on
  any curl new enough to build this addon at all. Implements just enough
  of RFC 6455 to open a connection, receive text frames (with simple
  fragmented-message reassembly), and answer ping frames; no
  permessage-deflate, no client-initiated fragmentation, since Dispatcharr
  needs neither for these small JSON payloads. On Windows this needed an
  explicit `ws2_32` link (`CMakeLists.txt`) -- curl handles its own Winsock
  linkage internally but doesn't propagate it to a consumer that also
  calls raw Winsock functions (`select()`) itself.
  Runs alongside, not instead of, the periodic-poll thread above: if the
  WebSocket can't connect or a connection drops and stays down (reconnects
  with exponential backoff, capped at 60s), the poll still gets there
  eventually. Verified end-to-end against the live server: created a
  recording directly via Dispatcharr's API (bypassing this addon
  entirely) and saw the `recording_updated` push arrive **less than one
  second** later, with the recording already visible in Kodi by the next
  check; deleted it the same way and saw `recording_cancelled` arrive
  **1 millisecond** after the delete call. The connection also correctly
  reacted to unrelated events from other concurrent activity on the same
  shared Dispatcharr account during testing (a `recording_started`/
  `recording_ended` pair neither created nor expected), confirming it
  reflects real account-wide activity, not just this install's own
  actions -- exactly the cross-install gap the periodic refresh above was
  built to narrow, now closed to sub-second latency when this is enabled.
  Re-confirmed the same way via `tools/kodi_smoke_test.py`'s own
  `check_realtime_update_push` on a real macOS install, a real
  CoreELEC/ODROID N2+ install (both 2026-09-15), a real native Windows
  install, and both real Android devices this project tests against, one
  32-bit ARM and one 64-bit ARM (2026-09-16) -- same
  create-directly-via-Dispatcharr's-API-then-poll-`PVR.GetTimers`
  approach, same clean pass on every platform, completing this feature's
  live confirmation across every target platform this project supports,
  alongside the original Linux one.
  One real snag on the Windows pass, worth remembering: the specific
  channel `tools/kodi_smoke_test.py`'s own channel-auto-discovery picked
  (the account's own first channel, already heavily exercised by this
  project's own testing all session) didn't show the newly-created
  recording as a timer within the poll window, even though `kodi.log`
  confirmed the realtime push itself arrived and the addon's own
  recordings cache count incremented within about a second of creation
  -- the push mechanism plainly worked. Likely cause: that specific
  channel already has its own active series/recurring rule tracking the
  same programme from this project's own earlier testing, and the new
  one-time recording got matched into that rule's own tracking
  (`SeriesRuleMatching`) rather than surfacing as an independent timer.
  Retried against a different, unrelated channel and it passed cleanly
  on the first attempt -- not a bug in the realtime-update feature
  itself, just a test-channel-selection collision with this project's
  own prior testing on the same account.

  **Update (2026-09-26, a 24th-pass audit): "every recording lifecycle
  event this addon cares about is already broadcast" above overclaims
  for a plain *edit*, confirmed against Dispatcharr's own real current
  upstream source (cloned into a scratchpad, never committed to this
  repo -- stronger than the API shape alone, not the same standard as a
  live test).** `Recording`'s own `post_save` signal
  (`schedule_task_on_save`, `apps/channels/signals.py`) schedules
  `prefetch_recording_artwork` after any save, but that task only sends
  `recording_updated` when it actually changes one of a specific set of
  enrichment fields (poster/rating/season/episode/onscreen episode) --
  gated on `updated` being true. There's no separate, unconditional push
  anywhere for a plain schedule edit: `RecordingViewSet` has no
  `perform_update()`/`perform_create()` override of its own that pushes
  one either. So editing an already-enriched recording's start/end time
  (this addon's own `UpdateOneTimeRecording()` PATCH, seen from a
  *second* Kodi install, or the identical edit made via Dispatcharr's
  own web UI) usually changes none of those enrichment fields and
  sends nothing -- confirmed indirectly by Dispatcharr's own frontend,
  whose `api.js` `updateRecording()` refetches manually right after its
  own PATCH rather than relying on a push. Recurring rules have an
  analogous gap: `purge_recurring_rule_impl()`/`sync_recurring_rule_impl()`
  only notify `if removed or total_created`, so deleting or disabling a
  rule with no future occurrences left to purge sends nothing either.
  Impact in both cases is bounded to the existing periodic-poll
  fallback's own worst case (`recording_refresh_minutes`, default 5) --
  not a correctness bug, just this doc's own wording (and `README.md`'s
  "show up immediately" feature-list line, corrected the same day)
  claiming more coverage than actually exists.
- **`ReadRecordingStream()` used to open a brand-new libcurl easy handle
  (fresh TCP connection, fresh TLS handshake if HTTPS) for every single
  demuxer read**, rather than reusing one across the life of an open
  recording. Negligible on a low-latency LAN/Ethernet link, but confirmed
  (via real measurements on a higher-latency link) to starve playback on
  a higher-latency link even with plenty of raw bandwidth for the
  recording's bitrate: a single bulk range request over one connection
  measured dramatically higher throughput than many small sequential
  reads with a fresh connection each (matching the old per-read pattern)
  -- barely above what a real recording needed, and reproduced live as
  `CVideoPlayerAudio::Process - stream stalled` a few seconds into
  playback. Fixed by keeping one persistent `CURL*` in
  `RecordingStreamState`, reused across reads so libcurl's own connection
  cache lets keep-alive apply, and only torn down on a transport-level
  error (in case a long-idle keep-alive connection went stale) or on
  `CloseRecordingStream()`. Verified on a wired link by
  watching `netstat` during live playback: one connection stayed
  `ESTABLISHED` for the full duration of an 8-second sampling window
  instead of new ports cycling through `ESTABLISHED`/`TIME_WAIT` on every
  read.
- The same per-call fresh-connection cost also applied to `Request()`, the
  helper behind essentially every other API call (login, `GetChannels()`,
  `GetRecordings()`, `AddTimer()`'s `CreateOneTimeRecording()`, etc.) --
  not as hot a path as recording reads, but a companion session found that
  a single "Record" press fires several of these in a row, and on WiFi
  each one is independently exposed to a connection-setup latency spike:
  measured a low latency under calm conditions but 1.8-10s before Kodi's own
  "recording started" notification appeared under worse ones, on
  identical code across repeated runs -- pointing at intermittent
  connection setup, not a deterministic slow path. Unlike
  `ReadRecordingStream()`, `Request()` can't just reuse one `CURL*`: Kodi's
  PVR API calls into this client from multiple threads (see the class
  comment in `DispatcharrClient.h`), and a single easy handle isn't safe
  for concurrent use. Fixed with a `CURLSH` share object (connection/DNS/
  TLS-session cache) applied to every easy handle this client creates,
  with mutex-backed lock/unlock callbacks -- libcurl doesn't lock a share
  object internally, that's on the application. Verified no regressions
  across channels/recordings/timers/`AddTimer()`/playback after the
  rebuild.
  This closed most, but confirmed not all, of the gap: the companion
  session's post-fix WiFi timing was 2/3 runs at ~0.1s (matching the raw
  api latency floor) but one run at 8.4s, still in the original
  complaint's range. They ruled out the network path for that outlier --
  a 30-second/60-packet ping to the Dispatcharr host in a calm window
  right after showed 0% loss, consistently low latency throughout, no anomaly -- and floated
  (not confirmed at the time, no lower-level instrumentation attempted)
  macOS WiFi radio power-save/idle-wake behavior: if the radio dozes
  during a quiet spell between guide navigation and the record press, the
  next transmission can eat a real multi-second wake latency no HTTP-layer
  fix touches, and a sustained ping (which itself keeps the radio busy)
  wouldn't reproduce it.

  **Later investigated properly on a real Mac, with better evidence
  either way -- still not a clean confirm or refute, but no longer just a
  guess.** Ran 7 real `PVR.AddTimer` calls through Kodi's own JSON-RPC
  (not a synthetic HTTP probe) against distinct future EPG broadcasts,
  each preceded by a genuine idle gap (1s, 1s, 15s, 20s, 25s, 30s, 35s --
  no JSON-RPC/script traffic during the gap), cleaning up each timer
  immediately after. Two real bugs in the test harness itself were caught
  and fixed *before* trusting any result from it, not after: a cleanup
  pass that omitted `istimerrule` from its `PVR.GetTimers` properties
  request caused a "delete anything non-rule" filter to misfire against
  the pre-existing recurring-rule timer (a real recurring daily show,
  see `docs/RECURRING_RULES.md`) -- confirmed no actual harm (a full Kodi
  restart forced a clean resync from Dispatcharr and the rule was still
  there, untouched), fixed to match by title against the test's own
  broadcasts before deleting anything, and re-verified the real rule was
  untouched before proceeding; separately, an early netstat-based
  packet-counter correlation attempt was reading the wrong column
  (`Ibytes` as `Opkts`) and was fixed before drawing any conclusion from
  it. With the harness actually correct: **all 7 trials landed in a tight
  218-300ms band, zero spikes, no reproduction of the slow case.**

  That non-reproduction doesn't cleanly settle it, though -- this specific test machine turned out to have continuous outbound WiFi traffic
  at all times (a steady packet rate sampled over 30s), confirmed unrelated to
  Kodi/this addon by repeating the same measurement with Kodi fully
  killed (`pkill -9`) and getting the identical rate. A radio that never
  goes quiet can't enter 802.11 power-save doze in the first place, so
  this machine's own environment can't actually distinguish "the theory
  is true" from "the theory is false" -- absence of reproduction here is
  confounded, not a refutation. Digging into *why* the radio stays busy
  (`nettop -l 1 -x` during a quiet window) surfaced a more parsimonious
  candidate for the original single 8.4s spike than radio wake-up: an
  established, already-present `kernel_task` connection to a **different,
  unrelated** host (an SMB/file-sharing IP, not the Dispatcharr host)
  showing a very high rate of retransmits and out-of-order packets in one
  one-second sample -- a lossy background connection (likely a mounted
  network share) that could plausibly cause a coincidental
  jitter spike at the moment of an `AddTimer` call, independent of
  anything Dispatcharr- or radio-specific. Not proven either (no slow
  `AddTimer` was actually caught in the act to check against it directly)
  -- a better-evidenced hypothesis, not a confirmed alternate cause.

  Net effect on how this should be read: **downgrade from "likely
  environmental, would fix with keep-alive traffic" to "unconfirmed, with
  a plausible unrelated alternate explanation and no clean way to test
  the original theory on typical hardware that has any other steady
  background network activity at all."** The periodic keep-alive-traffic
  mitigation floated originally was already speculative and this pass
  didn't strengthen the case for it -- not implemented, and not
  recommended unless the radio-doze theory gets real, direct confirmation
  (e.g. a slow `AddTimer` actually caught alongside a genuine power-save
  wake event, on a machine quiet enough for that state to occur at all).
- Unrelated discovery while testing the fix above: Kodi can reject
  `PVR.AddTimer` outright with "The PVR backend does not allow to record
  this event" for some EPG broadcasts and not others, with **zero** log
  output from this addon (confirmed: no `AddOnLog: pvr.dispatcharr-unofficial`
  line at all) -- meaning the rejection happens entirely in Kodi core,
  before ever reaching `AddTimer()`. Not investigated further (out of
  scope, and the exact same broadcastid succeeded cleanly and instantly
  moments later), but worth knowing so a rejected recording isn't
  mistaken for an addon bug: check for a scheduling conflict on that
  channel first (a channel already mid-recording will reject an
  overlapping one, which explains at least one case seen).
- A freshly-created recording can briefly show as `"Recording <id>"`
  instead of its real title, until Dispatcharr's own async enrichment
  (`custom_properties.program.title`, see above) catches up and a later
  refresh picks it up. Kodi already has the correct title *before* this
  addon is ever called, though: `CPVRTimerInfoTag::CreateFromEpg()`
  populates it from the EPG tag the user pressed "Record" on, and
  `AddTimer()` was just discarding it (`CreateOneTimeRecording()`'s
  `title` parameter went unused, deliberately, to avoid the
  custom_properties-replace-not-merge trap noted above). Fixed by caching
  that title client-side (`DispatcharrClient`'s `PendingTitle`, matched by
  channel, not also start time -- Dispatcharr silently clamps a recording's
  stored `start_time` to the moment it actually began for an
  already-airing EPG event, confirmed against a real one, so exact-time
  matching missed the single most common case: "Record" on something
  currently on) and using it in `GetRecordings()` in place of the
  `"Recording <id>"` fallback. Live-tested against several real EPG
  broadcasts (including an already-airing one) and confirmed the correct
  title end to end with no regressions -- but this server's own
  enrichment turned out to be fast enough in testing (both for
  already-airing and future-scheduled recordings) that the exact race
  this fixes couldn't be reliably reproduced live; the fix is
  correct-by-construction (a pure fallback, only consulted when the
  server-provided title is still empty) rather than confirmed against a
  reproduced failure the way most fixes in this file are.
- **The entire `inputstream.ffmpegdirect`-based in-progress recording
  mechanism documented at length above -- `LocalPlaylistServer`, the
  gradual-cap join-position workaround, the "Play live"/"Play from start"
  context-menu split, and the permanent seek-vs-live-follow trade-off that
  drove all of it -- has been replaced outright, not just patched
  further.** That whole design existed because ffmpeg/libavformat's HLS
  demuxer ties seekability to a *known, finite* duration, which is
  fundamentally incompatible with a playlist that's still being appended
  to; the only way around it within that architecture was picking one of
  the two per session. Server-side live timeshift (`docs/TIMESHIFT.md`)
  had already solved the equivalent problem for live channels by dropping
  `inputstream.ffmpegdirect` entirely and demuxing a growing buffer
  through this addon's own `OpenLiveStream`/`ReadLiveStream`/
  `SeekLiveStream`, letting Kodi's *native* demuxer -- which has no such
  finite-duration requirement, since `GetStreamTimes()` supplies a
  self-reported, freely-growing `ptsEnd` instead -- handle it directly.
  The same mechanism applies just as well to an in-progress recording:
  `CInputStreamPVRRecording` extends the same `CInputStreamPVRBase` as
  `CInputStreamPVRChannel` (confirmed in Kodi-core source), so the
  identical `GetStreamTimes()`/`CanPauseStream()`/`CanSeekStream()`/
  `IsRealTimeStream()` callbacks that make live-timeshift's real
  pause/rewind/live-follow work apply unchanged to a recording once the
  same growing-buffer approach is used for it.
  Implemented as `DispatcharrClient::OpenInProgressRecordingStream()`/
  `ReadInProgressRecordingStream()`/`SeekInProgressRecordingStream()`/
  `GetInProgressRecordingStreamDurationMs()` -- an append-only variant of
  the live-timeshift buffer (no rolling-window eviction needed, since a
  recording's own segments are never recycled the way a live buffer's
  are): `RefreshInProgressRecordingManifest()` parses the recording's HLS
  playlist directly (no plugin, no rewriting, no local HTTP server -- the
  same `X-API-Key`-authenticated direct reads `OpenRecordingStream()`
  already uses for a completed recording, just against the in-progress
  `.../hls/index.m3u8` instead of the post-completion `/file/` endpoint),
  merging any segments past the count already known into a fixed-origin
  byte address space exactly like `RefreshLiveManifest()` does.
  `GetRecordingStreamProperties()` is now drastically simpler as a result
  -- it only ever sets `ISREALTIMESTREAM`, `STREAMURL` is never populated
  for either recording flavour -- and `LocalPlaylistServer.cpp`/`.h`, the
  mode-choice context-menu hook, and the two now-dead
  `PendingLiveMode`-style settings/strings were all deleted rather than
  kept alongside the new path.
  `OpenRecordedStream()` checks the recording's current `isInProgress`
  (same live `GetRecordings()` check `GetRecordingStreamProperties()` used
  to do the mode-choice with) to decide which of the two implementations
  to open; `ReadRecordedStream()`/`SeekRecordedStream()`/
  `LengthRecordedStream()`/`CloseRecordedStream()`/`GetStreamTimes()`/
  `CanPauseStream()`/`CanSeekStream()`/`IsRealTimeStream()` all branch the
  same way, via `DispatcharrClient::IsInProgressRecordingStreamOpen()`
  (only one of the two recording-stream flavours, or a live-timeshift
  stream, is ever open at once). A completed recording is entirely
  unaffected, still going through the original `OpenRecordingStream()`/
  etc. byte-range path.
  Two real bugs found and fixed during live verification, neither
  specific to the design above -- both pre-existing gaps this addon's own
  code had to close, not anything wrong with Dispatcharr:
  1. **Segment-size probing silently downloaded entire multi-MB segments
     instead of a few bytes, and got worse the longer a recording ran.**
     `ProbeSegmentByteSize()` (needed once per newly-discovered segment,
     mirroring `RefreshLiveManifest()`'s own per-segment probe) originally
     issued a `Range: 0-0` GET and read the total size back from a
     `Content-Range` response header, exactly like the completed-recording
     path's own probe does. Confirmed live via a direct `curl -r 0-0`
     against a real in-progress segment that Dispatcharr's in-progress-
     recording HLS endpoint (unlike the completed-recording one)
     **ignores the `Range` header entirely** and returns a plain `200`
     with the full body and no `Content-Range` header at all -- so every
     probe both downloaded the entire segment over the network (several
     MB each) *and* came back with no usable size, meaning no segment
     ever got added to the known set. Because segments-known never grew,
     every subsequent manifest refresh re-probed *every* segment in the
     playlist from scratch, not just the new ones -- an unbounded,
     ever-growing cost per refresh as the recording (and its segment
     count) grew, which is what made opening a recording that had already
     been running a while for several minutes appear to hang indefinitely
     rather than just be slow. Fixed by switching the probe to a `HEAD`
     request (`CURLOPT_NOBODY`) reading a plain `Content-Length` header
     instead (confirmed via `curl -I` against the same segment: `HEAD`
     returns the correct length with no body transferred at all) -- a new
     `ContentLengthHeaderCallback`, separate from the existing
     `RecordingHeaderCallback` (which stays as-is for the completed-
     recording path's genuine ranged-GET use, where `Content-Range`'s
     semantics -- slice size vs. total -- actually differ from a plain
     `Content-Length`). Confirmed live: cold-open against a ~70-second-old
     recording found all its already-written segments on the very first
     attempt, no retry loop needed.
  2. **`GetStreamTimes()`/`CanPauseStream()`/`CanSeekStream()`/
     `IsRealTimeStream()` checked whether server-side live-timeshift mode
     was *enabled in settings*, not whether a live-timeshift stream was
     *actually open*.** `m_liveTimeshiftMode` is read once from the
     `live_timeshift_mode` setting at construction and never changes at
     runtime, so with server-side timeshift enabled, `m_liveTimeshiftMode
     == kLiveTimeshiftServer` was true unconditionally -- including while
     an in-progress *recording*, not a live channel, was what was actually
     open. In `GetStreamTimes()` this meant the live-timeshift branch
     always won, permanently shadowing the in-progress-recording branch
     below it and reporting `GetLiveTimeshiftStreamDurationMs()`'s `0` (no
     live stream open) as `ptsEnd` instead of the recording's real,
     growing duration. Confirmed live: `canseek: false` and an empty
     `Player.Duration` throughout, even after fix #1 above was confirmed
     working and `GetInProgressRecordingStreamDurationMs()` was already
     correctly returning a growing, non-zero value on every call --
     diagnostic logging on both branches' actual entry conditions made the
     shadowing directly visible in `kodi.log`. Fixed by adding a genuine
     `DispatcharrClient::IsLiveTimeshiftStreamOpen()` accessor (mirroring
     the existing `IsInProgressRecordingStreamOpen()`) backed by the
     live-timeshift stream state's own `open` flag, and checking that --
     not the setting -- in all four callbacks. Confirmed live
     end-to-end after both fixes: `canseek: true`, `totaltime` correctly
     showing and growing with the recording (`10:56` and climbing), an
     actual `Player.Seek` landing near its target (confirmed via
     `CDVDDemuxFFmpeg::SeekTime` in `kodi.log`, not just JSON-RPC's own
     EPG-relative `time` display -- see `docs/TIMESHIFT.md`'s note on why
     that display can't be trusted directly), working pause/resume, and
     the reported duration growing by ~27s over a 30-second wait with
     playback continuing uninterrupted throughout -- real live-follow.
     Regression-tested a completed recording immediately after and
     confirmed it still takes the original, unaffected code path
     (`inProgress=0` in the log) with its own correct fixed duration.
  3. **A third, more serious bug shipped alongside the two above and
     wasn't caught by that verification pass: `ReadInProgressRecordingStream()`
     silently corrupted playback from the second read of every segment
     onward, on every platform, not just the one it was first noticed on.**
     Caught by a companion session doing real macOS verification who
     checked `kodi.log` for actual decode errors rather than only
     `Player.GetProperties` state -- continuous `ffmpeg[h264]: No frame
     decoded?`/`hardware accelerator failed to decode picture` from open
     through 70+ seconds of playback, `ActiveAE - large audio sync error`
     climbing past -15000ms, and `time` barely advancing (18s to 28s over
     70+ real seconds) despite `speed: 1` -- while `canseek`/`totaltime`
     looked completely correct throughout, which is exactly why the
     original verification pass above missed it: it never looked past
     JSON-RPC player state to the actual decode log or watched real
     playback quality. Cleanly isolated by playing the *completed* version
     of the same freshly-recorded content through the unaffected
     `OpenRecordingStream()` path immediately after: zero decode errors,
     exact real-time progression. Checked this addon's own Windows
     `kodi.log` from the verification pass above and found the identical
     1400 decode-error lines already present there too, missed for the
     same reason -- confirmed not platform-specific.
     Root cause: `ReadInProgressRecordingStream()` issued a ranged GET
     (`CURLOPT_RANGE`) per demuxer read, mirroring the completed-recording
     path's own per-read ranged reads against `/file/` -- but unlike that
     endpoint, Dispatcharr's in-progress-recording HLS segment endpoint
     ignores `Range` entirely and always returns the *full* segment body
     from its own byte 0 (the same finding fix #1 above already made
     against a `HEAD`/ranged-GET size probe, just not yet applied to the
     actual data-reading path when that fix shipped). Every read therefore
     silently received that segment's own leading bytes, correct only for
     the very first read of each segment and wrong -- not an error, just
     quietly incorrect data handed to the demuxer -- for every read after
     that, corrupting the reconstructed stream from partway through the
     first segment onward. This also explains the near-stalled real-time
     progression: since the server always sends the complete segment body
     regardless of the requested range, and the old write callback
     (`FixedBufferWriteCallback`) let curl keep streaming the full response
     while only copying the first `wantSize` bytes into the caller's
     buffer, *every single small demuxer read re-downloaded the entire
     multi-MB segment over the network*, not just the requested slice.
     Fixed by adding a whole-segment cache to `InProgressRecordingStreamState`
     (`cachedSegmentBytes`/`cachedSegmentByteOffset`): the first read
     landing in a given segment fetches that segment's full body exactly
     once (a plain GET, no `Range`, into the cache), and every read against
     that segment -- however many the demuxer issues -- is served directly
     from memory afterward, correctly sliced client-side by
     `offsetInSegment` instead of trusting the server to honor a `Range`
     header it ignores. The cache holds only the one segment current reads
     are landing in (replaced, not accumulated, the moment `position`
     moves into a different one), so memory use stays bounded to
     (transiently, up to twice -- noted 2026-09-27, a 57th-pass audit,
     since a 56th-pass audit's own fix now briefly holds the outgoing and
     incoming segment's bytes at once while a fetch is in flight, to
     avoid a cache-coherency bug a failed fetch used to risk -- see
     `ReadInProgressRecordingStream()`'s own comment) a single segment's
     size regardless of recording length; a seek into an
     already-cached segment is free, a seek into a new one costs one fresh
     full-segment fetch, matching the seek-cost tradeoff already accepted
     elsewhere in this addon. Re-verified live end-to-end after the fix:
     zero decode errors across a 74-second continuous playback session (vs.
     1400 before), real-time progression throughout (`time` advancing ~66s
     over a 65-second wall-clock window), and a `Player.Seek` to 1:00
     landing at 58.99s (`CDVDDemuxFFmpeg::SeekTime`) with zero decode
     errors afterward either, confirming a seek into a freshly-cached
     segment works correctly too, not just sequential reads within one
     already cached.
     The companion session that originally caught this (real macOS
     hardware-decoder testing) re-verified the fix independently right
     after, this time covering everything the corrupted build had blocked
     testing: forward seek (0:20, landed exactly on target, a brief
     transient decode-error burst right at the seek transition matching
     the same normal-decoder-resync pattern already seen on the completed-
     recording path, then flat for the next 24s), pause (position held
     frozen exactly across 8s, zero new errors), resume (continued from
     the exact paused position, zero new errors), and backward seek (0:05,
     landed exactly on target, a smaller resync blip, then clean) -- all
     with `totaltime` continuing to grow the entire time regardless of
     pausing or seeking around within the buffer, confirming real seek in
     both directions, pause/resume, and continued live-follow all work
     correctly together in one session, corruption-free, cross-platform.
- **The `enable_inprogress_playback` opt-in setting itself was later
  removed, once cross-platform verification above confirmed the feature
  stable -- in-progress recording playback is now unconditional, the same
  way playing a completed recording always has been.** `settings.xml`'s
  toggle and its two strings (`#30036`/`#30037`) are gone;
  `OpenRecordedStream()`/`GetRecordingStreamProperties()` check a
  recording's live `isInProgress` status unconditionally now rather than
  gating that check behind the old `m_enableInProgressPlayback` flag.
  Confirmed live after the change: a fresh in-progress recording opened
  and played correctly (`canseek: true`, growing `totaltime`, zero decode
  errors) with no setting enabled at all -- there's nothing left to enable.
- **Recordings are grouped into per-show folders in Kodi's own recordings
  UI (`PVRRecording::SetDirectory()`), using `rec.title` -- confirmed
  against Dispatcharr's own source that this is genuinely the show name,
  not an episode-specific one, and that it's the exact same value
  Dispatcharr itself uses as the on-disk folder segment
  (`apps/channels/tasks.py`'s `_build_output_paths`: `show` and `title`
  are both `program.get('title')`, read once and used for both).** Using
  this instead of parsing `file_path` directly means no dependency on
  knowing Dispatcharr's currently-configured `tv_template`/
  `tv_fallback_template` strings to correctly strip the show segment back
  out -- the two are provably identical at the source, so the cheaper one
  wins. Never empty: `rec.title` already falls back to "Recording <id>"
  server-side when nothing else is available (see the first entry in this
  file), so an unmatched/manual recording gets its own single-item folder
  rather than an empty `Directory`, the same grouping behavior a real
  named show gets. Confirmed live: a real EPG-matched test recording came back from `PVR.GetRecordingDetails`
  with its `directory` matching its `title` exactly.

  **Update (2026-09-26, a 21st-pass audit): "provably identical at the
  source" above was only true for a title with none of a small set of
  characters, confirmed against Dispatcharr's own real current upstream
  source (cloned into a scratchpad, never committed to this repo --
  stronger than the API shape alone, not the same standard as a live
  test).** `_build_output_paths` doesn't use `program.get('title')`
  completely raw -- it passes both `show` and `title` through its own
  `_safe_name()` first, which strips `[\/:*?"<>|]` and trims. A title
  containing a forward slash (e.g. "Face/Off", "20/20") passed straight
  to `SetDirectory()` turned into a *nested* Kodi folder instead of
  Dispatcharr's own single flat one for that show -- confirmed against
  Kodi's own real source, `CPVRRecordingsPath` treats a raw `/` in
  `Directory` as a path separator, and only the title itself gets
  `CURL::Encode()`'d, not the directory. Fixed with new
  `dispatcharr::SanitizeRecordingDirectory()` (`RecordingDirectory.h`),
  mirroring `_safe_name()`'s own character set and falling back to
  `"Recording"` for the rare case sanitizing consumes the whole title.
- **Recording pre/post padding is now surfaced as two addon settings
  (`recording_pre_offset_minutes`/`recording_post_offset_minutes`) that
  mirror Dispatcharr's own global padding setting directly, rather than
  Kodi's per-timer margin fields
  (`PVRTimer::SetMarginStart()`/`SetMarginEnd()`).** That per-timer route
  was the original plan, but confirmed against Dispatcharr's own source
  (an exhaustive search of `Recording`/`RecurringRecordingRule`/
  `SeriesRuleRequest` for any offset/padding/margin-shaped field, not
  just absence in one file) that Dispatcharr has no per-item override at
  all -- padding is one value pair, global, period. Exposing it as a
  per-timer Kodi field would have implied a per-timer effect that doesn't
  exist server-side; a plain settings-screen value that reads/writes
  Dispatcharr's real global setting is the honest fit instead. Also
  confirmed a real, non-obvious gap on Dispatcharr's own side while
  researching this: the offset only ever gets applied to *EPG-based*
  scheduling (series rules, an EPG-matched one-time recording) --
  `sync_recurring_rule_impl` (the day-of-week recurring-rule scheduler,
  see `docs/RECURRING_RULES.md`) builds its recordings straight from the
  rule's own `start_time`/`end_time` with no offset applied at all.
  Recurring-rule recordings are simply unaffected by this setting no
  matter what it's set to -- not something this addon can fix, just
  documented rather than silently wrong.

  **Update (2026-09-26, a 20th-pass audit): this is only true of a
  recurring occurrence's own *initial* scheduling -- *changing* the
  padding setting afterward can still reach (and, per Dispatcharr's own
  logic, duplicate) an already-materialized recurring occurrence,
  confirmed against Dispatcharr's own real current upstream source
  (cloned into a scratchpad, never committed to this repo -- stronger
  than the API shape alone, not the same standard as a live test).**
  `CoreSettingsViewSet.update()` runs a `reschedule_upcoming_recordings_
  for_offset_change` task whenever pre/post actually changes, and that
  task re-pads every future `Recording` whose own
  `custom_properties.program` carries a `start_time`/`end_time` -- which
  a recurring occurrence's own row does, set by `sync_recurring_rule_impl`
  itself, the same function this entry's own paragraph above already
  cites for never applying the offset *at creation*. The hourly
  `maintain_recurring_recordings` beat task (`sync(drop_existing=False)`)
  then checks for an already-existing occurrence at the rule's own
  *unpadded* `start_time`, doesn't find one (it's now padded), and
  creates a second, unpadded, overlapping recording for that same
  occurrence -- two recordings, two provider streams, from one setting
  change. This self-heals within about `kRecurringRuleWindowDays`/2 days
  of the change, once this addon's own periodic renewal PATCH purges and
  regenerates every future occurrence unpadded again (`sync(drop_existing=
  True)`) -- but not before. This is an upstream Dispatcharr behavior
  this addon's own settings screen can trigger, not something addressed
  here; see `docs/OPEN_ITEMS.md`.

  **Update (2026-09-26, a 25th-pass audit): "an EPG-matched one-time
  recording" above only ever describes a recording created through
  Dispatcharr's *own* UI, not one created by this addon itself,
  confirmed against Dispatcharr's own real current upstream source
  (cloned into a scratchpad, never committed to this repo -- stronger
  than the API shape alone, not the same standard as a live test).**
  `RecordingSerializer.validate()` only applies the pre/post offset when
  `isinstance(custom_properties.get("program"), dict)` -- but
  `DispatcharrClient::CreateOneTimeRecording()` deliberately sends no
  `custom_properties` at all (see its own comment: an explicit value on
  create would *replace* Dispatcharr's own auto-enrichment rather than
  merge with it), so a recording created by pressing "Record" in Kodi's
  own EPG guide gets exactly the raw EPG start/end times, no padding
  applied, regardless of what this addon's own `recording_pre/post_
  offset_minutes` settings are set to -- contradicting this addon's own
  user-facing text (`strings.po` #30055/#30057's "every EPG-based
  recording", `README.md`'s "synced with Dispatcharr's own global
  setting"). Corrected the same day, not yet a code fix: sending
  `custom_properties: {"program": {"start_time", "end_time"}}` only for
  an EPG-based timer (`timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID`), with
  no title/id so Dispatcharr's own enrichment still runs afterward,
  looks viable but needs a live test -- specifically to confirm this
  doesn't suppress that enrichment the way an existing comment elsewhere
  in this codebase already documents a *different*, full-custom_properties
  create call doing. A later padding-setting change would then also
  reach these recordings via `reschedule_upcoming_recordings_for_offset_
  change` (see the paragraph just above) once they carry a real
  `program.start_time`/`end_time` -- the same behavior Dispatcharr's own
  UI-created recordings already have, just new for this addon's own
  creates specifically.

  **Fixed and confirmed live, 2026-09-29 (see `docs/OPEN_ITEMS.md`'s own
  entry for the full test account).** The enrichment-suppression risk
  above was confirmed safe first, live: two real recordings, one with
  this exact `custom_properties` shape and one with none, both came
  back fully enriched within seconds of Dispatcharr's own
  `prefetch_recording_artwork` task, with the shaped one's own
  `start_time`/`end_time` merging in alongside the new fields rather
  than being replaced. `BuildOneTimeRecordingCreateBody()`
  (`TimerRequestBuilder.{h,cpp}`) gained an `includeEpgProgramWindow`
  parameter implementing exactly this; `CreateOneTimeRecording()`/
  `AddTimer()` (`DispatcharrClient.cpp`/`PVRDispatcharr.cpp`) pass
  `timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID` for it, so a manual
  (non-EPG) recording stays unaffected. Re-verified live end-to-end
  against a real Kodi client afterward: pressing "Record" on a real EPG
  guide entry produced a recording padded by exactly the instance's own
  configured offsets. While re-verifying, found (and logged separately,
  `docs/OPEN_ITEMS.md`) a real, unrelated Dispatcharr-side gap this fix
  doesn't touch: its own auto-enrichment queries a channel's *raw*
  `epg_data`, not the effective/override one this addon's own guide
  display already correctly uses -- a channel with an EPG override can
  have its recordings silently never enrich for a slot outside the raw
  source's own coverage, independent of anything this addon sends.

  Dispatcharr stores this pair (`pre_offset_minutes`/`post_offset_minutes`,
  minutes, default 0 for both) inside a single shared `CoreSettings` row
  (`key: "dvr_settings"`) alongside several unrelated settings -- comskip
  mode/hw-accel/custom-path, and the recording path templates
  (`tv_template`, `movie_template`, `tv_fallback_dir`,
  `tv_fallback_template`, `movie_fallback_template`). Confirmed live
  against a real instance's actual current row (not an assumed empty
  one) before writing `SetDvrOffsetMinutes()`: it's read-modify-write,
  fetching the full blob first and only changing the two offset keys
  within it -- a naive whole-field overwrite would have silently wiped
  every other key sharing that row. Verified directly (bypassing Kodi
  entirely, since this is server-side HTTP mechanics, not addon logic):
  PATCHed the row with the offsets changed (1/2 -> 3/4 minutes) and
  confirmed every other key -- `tv_template`, `comskip_mode`,
  `series_rules`, all the rest -- came back byte-for-byte unchanged, then
  restored the real values afterward.

  Synced *from* Dispatcharr into Kodi's own settings on every addon
  startup (only actually rewriting Kodi's persisted value when it's
  genuinely different, so a normal restart doesn't churn
  `OnAddonSettingChanged()` for no reason) -- confirmed live against this
  same real instance, which already had non-default padding configured
  (not 0/0): a fresh Kodi start picked up exactly those two values into
  `recording_pre_offset_minutes`/`recording_post_offset_minutes`
  with no user action, rather than showing a misleading `0` default that
  didn't match reality. Pushing the other direction (Kodi setting changed
  -> Dispatcharr updated) runs on a detached background thread from
  `OnAddonSettingChanged()`, unlike every other setting handled there --
  this is the first one requiring a real network round-trip rather than
  an in-memory write, and there's no reason to block whatever thread
  Kodi delivers the settings-changed callback on for it.

  **The two directions turn out to need different permissions, confirmed
  against Dispatcharr's current source, not assumed.** `CoreSettingsViewSet`
  (`core/api_views.py`) falls back to the same global
  `permission_classes_by_action` table used elsewhere in Dispatcharr
  (`apps/accounts/permissions.py`): `"retrieve"`/`"list"` map to
  `IsStandardUser`, so the GET this addon does at every startup works for
  any standard account. `"partial_update"` (what a PATCH resolves to,
  which is what `SetDvrOffsetMinutes()` sends) maps to `IsAdmin` --
  `user_level >= 10`, the same full-admin bar as the companion plugins'
  `run/` API, not the lighter `dvr_access` tier that gates recording
  management elsewhere in this addon. A non-admin account can freely read
  Dispatcharr's padding into Kodi's settings, but a push back fails with a
  403 -- and right now that failure is silent to the user: the error is
  logged at `ADDON_LOG_ERROR` in `OnAddonSettingChanged()`'s detached
  thread, with nothing surfaced through Kodi's UI, so the value stays
  showing as "changed" in Kodi's settings screen while Dispatcharr's real
  global setting silently didn't move. Not yet fixed -- flagged here so a
  future pass doesn't have to re-derive it from scratch.
- **`UpdateTimer()` is now implemented, closing a long-standing gap --
  every timer type edits without a delete+recreate, dispatched by the
  same `ClientIndex` namespace-bit scheme `DeleteTimer()` already uses.**
  Three genuinely different update mechanisms underneath, one per type,
  each confirmed live directly against a real instance before trusting
  it (Kodi's own JSON-RPC has no generic full-field `UpdateTimer`
  equivalent to drive this end-to-end through Kodi itself, the same
  limitation `AddTimer()`'s own recurring-rule testing hit earlier --
  see `docs/RECURRING_RULES.md` -- so this was verified the same way:
  exercise the exact request each C++ path sends, directly against the
  API):
  - **Recurring rules**: a plain `PATCH` -- `RecurringRecordingRuleSerializer`
    was written partial-update-safe (falls back to the existing
    instance's value for any field the payload omits), confirmed live by
    creating a rule with a real far-future `end_date`, PATCHing it with
    every field *except* `end_date` and `enabled: false`, and getting
    back `enabled: false` with the original `end_date` completely
    untouched. This is also how `PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE`
    reaches this type at all -- Kodi's own "enable/disable" timer action
    calls `UpdateTimer()` with the rest of the timer unchanged and just
    `GetState()` flipped, not a separate dedicated call.
  - **Series rules**: no PATCH exists (still no path-addressable id --
    see the series-rule entries earlier in this file), so this reuses
    `CreateSeriesRule()`, the same call `AddTimer()` uses to create one.
    Confirmed live this is a real upsert, not just believed from reading
    the source: created a rule, re-`POST`ed the identical title+channel
    with a different `mode`, and confirmed via a full re-`GET` of the
    rules list that exactly one rule existed afterward with the new
    mode -- not two. The real limitation this implies (not worked around,
    just documented): Dispatcharr's identity key for a series rule is
    `title`+`tvg_id`+`epg_source_id`; if those genuinely change, the
    "edit" creates a second rule under the new identity and leaves the
    original behind rather than renaming it. Not chased further -- Kodi's
    own series-timer dialog doesn't really support "rename this rule" as
    a normal workflow to begin with.
  - **One-time recordings**: `PATCH` with only `start_time`/`end_time`,
    never `custom_properties`/title -- two real risks confirmed live
    before settling on this shape, not just inferred from reading
    Dispatcharr's `RecordingSerializer.validate()`: (1) a `PATCH` that
    omits both times crashes with an uncaught server-side 500
    (`end_time < now` runs against a `None` on a bare partial update --
    a genuine Dispatcharr bug, not preventable except by never sending
    that request shape), confirmed by deliberately sending a
    `custom_properties`-only `PATCH` against a real recording and getting
    a raw 500 back; (2) resending a real EPG-matched recording's
    *unchanged* `start_time`/`end_time` was suspected (from reading
    `validate()`'s own source, which re-derives offset-adjusted times
    whenever `custom_properties.program` is a dict and both times are in
    the payload) to risk silently re-applying the global pre/post padding
    a second time on every edit -- **confirmed live this does NOT
    happen**: PATCHed a real EPG-matched recording (a non-default padding
    configured) with its own current, unchanged times and got back the
    exact same `start_time`/`end_time`, no drift. Scoped to times only
    (not title) as a result of being unable to fully rule out the
    metadata-editing path the same way Dispatcharr's own UI avoids it
    (a dedicated `update-metadata` action instead of the generic PATCH)
    -- mirrors `CreateOneTimeRecording()`'s own existing choice not to
    send `custom_properties` at all, for the same underlying reason
    (avoid stomping Dispatcharr's own auto-enrichment).
- **Seeking to the live edge of an in-progress recording took ~10s,
  root-caused and fixed to land at ~2-3s, which turned out to be the
  real floor.** Reported live: recorded a channel for 2 minutes, started
  watching it, stepped forward to the live edge, and playback took
  ~10s to resume. `SeekInProgressRecordingStream()` had no equivalent of
  `SeekLiveTimeshiftStream()`'s existing live-edge backoff -- it clamped
  a forward seek straight to the current `totalBytes` (the exact tip),
  leaving zero read-ahead margin, so the very next read landed
  immediately back at the tail and had to wait out another whole
  segment-production cycle right after what looked like a completed
  seek. Ported the identical fix: back off by one segment's worth of
  bytes so there's always something already available to play the
  instant the seek reports success.

  Confirmed live via targeted timing instrumentation added to
  `SeekInProgressRecordingStream()`, `ReadInProgressRecordingStream()`'s
  catch-up loop, `RefreshInProgressRecordingManifest()` (playlist fetch /
  per-segment probe / `GetRecordings()` timing breakdown), and the
  previously entirely unlogged per-segment body fetch inside
  `ReadInProgressRecordingStream()` -- added because the first two
  retest attempts showed *zero* addon-level log activity during the
  entire multi-second delay window, which turned out to be because a
  stale, disconnected build (a leftover local Windows dev copy the
  addon-defs tooling was still building from, not this repository) was
  being tested both times; the reported "10s -> 5-6s" improvement
  between those two attempts was pure test-to-test variance on the
  *original*, unfixed binary, not a real signal. Once the build was
  pointed at the actual source and verified via `strings`/`grep -a` on
  the compiled binary before redeploying, a real retest showed the seek
  itself completing in ~116ms (three internal FFmpeg seek probes --
  start of stream, near the end, and twice slightly past the known end
  -- all safely clamped to the backed-off tail instead of the raw edge,
  so none of them blocked), with the remaining ~2.2s entirely accounted
  for by one clean catch-up-loop cycle (`9/16 attempts`) waiting on
  Dispatcharr's own DVR recording ffmpeg to actually produce its next
  segment (`-hls_time 4`, hardcoded in Dispatcharr's `tasks.py`, not a
  setting this addon or either companion plugin controls). That wait is
  the genuine floor for this operation -- you cannot play a segment the
  recorder hasn't written yet -- so ~2-3s for a live-edge seek on an
  in-progress recording is expected, not a bug, and there's nothing left
  to trim on this addon's side without Dispatcharr itself segmenting DVR
  recordings faster.
- **Opening a recording immediately after stopping it could error outright,
  or play without the ability to seek, depending on exactly when you
  tried -- fixed by adding a second signal alongside `isInProgress`.**
  Reported live: stopped a recording, tried playing it right away -- first
  attempt errored, second attempt played but wouldn't seek, third attempt
  (a little later) worked normally. Root cause: Dispatcharr's stop endpoint
  flips `custom_properties.status` away from `"recording"` synchronously,
  the instant the user stops it -- confirmed in `tasks.py`'s own comment,
  "'stopped' is set by the stop endpoint before stream teardown" -- well
  before the HLS-to-MKV concat that happens afterward, in the background
  recording task, actually produces a complete, stable file. This addon's
  `OpenRecordedStream()` was deciding "completed vs. still-recording" purely
  off that `status`-derived `isInProgress` flag, so during that gap it
  guessed "completed" and opened whatever partial state happened to exist
  on disk: nothing yet (error), or a real file still being actively written
  by the concat (played, but an unstable Content-Length the completed-
  recording path was never built to seek against safely).

  Fixed with a second, independent signal: `custom_properties._hls_dir`,
  which (confirmed in `tasks.py`) is only ever popped from custom_properties
  *after* the concat and the post-recording active-viewer-wait grace period
  both finish -- i.e. it stays present for the entire window `isInProgress`
  alone can't see. `OpenRecordedStream()` now keeps routing through the
  growing-buffer HLS reader for as long as `_hls_dir` is still there,
  regardless of what `status` already says. That reader already tolerates a
  frozen (finished, no-longer-growing) manifest correctly on its own --
  `RefreshInProgressRecordingManifest()` ties its own `finished` flag to
  `isInProgress`, not to whether Dispatcharr's ffmpeg process happens to
  still be running -- so once ffmpeg has actually exited, reads correctly
  stop waiting and just play through to genuine EOF like any static file.
  Deliberately NOT folded into `isInProgress` itself, which also drives
  Kodi's timer-state UI (`PVR_TIMER_STATE_RECORDING`) and must keep
  reflecting Dispatcharr's real status, not this file-readiness detail.

  Confirmed live after the fix: stopped a ~30-minute recording and opened
  it immediately -- played correctly on the first attempt (a few seconds'
  wait, matching the still-open HLS reader's own cold-start behavior), with
  the final MKV visibly still growing on disk throughout playback. A batch
  of `HEAD .../hls/segNNNNN.ts` "Broken pipe" errors turned up in
  Dispatcharr's own log around the same recording -- traced and ruled out
  as this addon's problem: every `RefreshInProgressRecordingManifest()`
  call in the corresponding `kodi.log` window reported `[0 failed]`
  probes, and the final segment count/duration matched the recording's
  full length with no gap. `ProbeSegmentByteSize()` sends exactly this kind
  of HEAD request (`CURLOPT_NOBODY`), which by HTTP definition carries no
  response body -- curl only needs the headers (Content-Length, status) to
  succeed, so a server that still attempts an unnecessary `sendfile()` for
  that nonexistent body and hits a broken pipe once the client (correctly)
  isn't reading one logs a scary-looking error even though the request
  itself, from curl's side, already succeeded. Dispatcharr-side log noise,
  not a real failure -- confirmed by hard evidence rather than assumed.
- **Pausing an in-progress recording's playback past the recording's own
  end killed playback on resume -- fixed with a periodic keep-alive that
  only runs while the viewer still has unread segments (2026-09-30).**
  Flagged by a 28th-pass source audit (2026-09-26), then reproduced live:
  a real 4-minute recording, played while still recording and paused
  ~48s in, ended at +240s; by +301s Dispatcharr had removed the HLS
  directory (the playlist URL answered 302 to the completed file), and on
  resume playback ran ~12s from Kodi's own cache, then 63 consecutive
  segment fetches came back 404 over ~8s (~7.8/s) and the player ended by
  itself 17s after resume.

  Root cause: once a recording finishes, Dispatcharr's own recording task
  waits for the `dvr:hls_viewer:{id}` Redis key to lapse before it removes
  the HLS directory (`apps/channels/tasks.py`), and the HLS view sets that
  key (20s TTL) for a `.ts` request and nothing else
  (`apps/channels/api_views.py`). A paused viewer's own background
  traffic -- the `GetStreamTimes()` polling that keeps running through a
  pause -- is a playlist fetch and a status lookup, neither of which
  touches the key (`ProbeSegmentByteSize()` does, but only for segments
  that newly appeared, and none do once the recording is over). So the key
  lapsed ~20s after the last real segment request and the directory went
  with it, out from under a viewer still holding unread segments.

  Fix: `MaybeSendInProgressHlsKeepAlive()` (`DispatcharrClient.cpp`, called
  at the top of `RefreshInProgressRecordingManifest()`, which
  `GetStreamTimes()`'s polling keeps reaching through a pause) sends a
  HEAD on the newest known segment every 10s (`kHlsKeepAliveInterval`),
  retrying a transient failure after 2s. DRF maps HEAD to the same view
  function a GET runs and `ProbeSegmentByteSize()` already HEADs `.ts`
  URLs, so it refreshes the key without downloading anything. Every request
  this addon makes that itself lands on a `.ts` URL (a segment body fetch,
  a newly discovered segment's size probe) pushes the schedule out the
  same way, so a stream that is actually playing never sends one. The
  decision logic is `dispatcharr::ShouldSendHlsViewerKeepAlive()`
  (`HlsViewerKeepAlive.h`, unit-tested, including a simulated 5-minute
  pause checked against the server's TTL).

  **One deliberately non-obvious rule: it only runs while the reader still
  has unread bytes (`position < totalBytes`).** Dispatcharr only finalizes
  a finished recording -- removes the HLS directory, *then* flips its
  status to `completed` -- after the viewer key lapses, and this addon only
  learns a recording is finished (and reports EOF) from exactly that
  finalization. A reader waiting at the tail that also kept the key alive
  would hold the recording open forever, waiting for an EOF its own
  keep-alive prevents. The keep-alive as first sketched in the audit entry
  ("while the recording is still open and unfinished") would have had that
  deadlock.

  Side effects worth knowing. For as long as a viewer sits paused with
  unread segments, Dispatcharr keeps the recording in its `recording`
  status and defers finalizing it -- the same "deferring HLS directory
  cleanup until client disconnects" behavior its own web UI's HLS viewer
  gets, capped at 4 hours by its own safety timeout -- so Kodi keeps
  showing it as recording, and the completed-file URL isn't available,
  until the viewer resumes and plays through to the end or stops. Reaching
  the end of the recording costs a ~20s stall at the tail while the key
  lapses (the same wait a viewer following the live edge already had).
  And Dispatcharr's own log gets one more `HEAD ... Broken pipe` line per
  keep-alive, the same harmless noise the entry above explains.

  Confirmed live after the fix (Linux test client, a real 3-minute
  recording, paused at position 18s and left paused ~110s past the
  recording's end): 12 keep-alives, every ~10.1s, all answered 200; the
  HLS playlist still answered 200 (not 302) and the recording still read
  `recording` at +300s. On resume, playback ran through to the recording's
  real end with zero segment 404s, stalled ~22s at the tail, ended by
  itself, and the server then reported `completed` with the playlist
  answering 302 -- the tail-stall/finalization sequence above, working as
  designed. The same scenario on the Windows test client (a 2-minute
  recording, paused ~60s past its end) behaved identically. `GetStreamTimes()`'s
  polling turned out to be roughly twice a second throughout the pause (392
  real manifest refreshes in ~245s), far more often than the 10s interval
  needs. Not covered: a directory that is
  already gone by the time a paused viewer resumes (a pause past Dispatcharr's
  4-hour cap, the device asleep past the TTL, a network outage longer than
  the TTL) still hits the old unbounded segment-404 retry -- see
  `docs/OPEN_ITEMS.md`'s entry on this.
- **Opening an in-progress recording within its first ~3 seconds failed
  outright instead of waiting for it to start (2026-09-30).** With
  real-time updates on, Kodi lists a newly started recording about a second
  after it is created, and opening it then (or up to about +2.5s) failed
  every time -- 6 of 6 in one live run -- with "Dispatcharr returned HTTP
  404 fetching in-progress playlist" after a single attempt. Opens at +3s
  and later worked. Cause: `RecordingViewSet.hls()` answers 404 both when
  the recording has no HLS directory yet and when the directory exists but
  has no `index.m3u8` yet, and `OpenInProgressRecordingStream()`'s
  cold-start loop -- whose own 45s budget was already confirmed live as
  necessary for a playlist that exists but has no segment in it yet --
  returned on the first *failed* refresh, so that budget only ever covered
  a fetch that succeeded. Real exposure was small (a person can't reach a
  recording that fast; a script or a very quick "record, then play" can),
  but the behavior was inconsistent with the wait right next to it.

  Fixed: a failed first refresh is now waited out, on the same budget, when
  it is a plain 404 *and* the server says the recording is in progress
  right now (`dispatcharr::ShouldRetryInProgressColdStart()`,
  `RecordingVisibility.h`). Anything else still fails fast -- a redirect
  (the HLS directory is already gone and the file is complete), a transport
  failure, a 401 or a 5xx say nothing about the recording being young, and a
  recording that finished, failed or was deleted must not burn the whole
  budget. Confirmed live: opens made 0.9-1.0s after creation now retry the
  missing playlist 2-3 times and reach advancing playback in 4.1-5.8s (4 of
  4), where the same window failed every time before. One thing the live
  runs showed about the wait itself: how long Dispatcharr takes to produce
  the playlist varies from a couple of seconds to more than 9 (it depends on
  how fast the channel connects), so an open right at the start can
  legitimately take that long -- the retry budget covers it, but a caller
  that gives up on playback sooner will still see a failure.

- **Watching an in-progress recording that got deleted (or finished with
  its HLS directory removed) kept the addon polling a server that could only
  answer 404, for as long as Kodi kept the stream open (2026-09-30).** With
  real-time updates off -- the default, and the only case where Kodi can't
  find out another way -- deleting a recording on the server while a viewer
  sat at its tail made the addon alternate a recording lookup and an HLS
  playlist fetch roughly every 0.3s, every one a 404: measured through a
  counting proxy, 766 requests over 98s (~7.8/s), ending only when Kodi's own
  no-data timeout closed the stream (101s and 109s in two runs). The lookup
  404 was already recognizable, but `ResolveInProgressFinished()`'s "a failed
  lookup is unknown, don't assume finished" rule -- right for a network blip
  -- swallowed it, and the refresh throttle only ever arms on a successful
  refresh, so nothing slowed the loop down.

  Fixed with `dispatcharr::IsInProgressContentGone()` (`RecordingVisibility.h`)
  and a sticky `contentGone` flag on the stream. A refresh that finds the
  playlist 404 (or a redirect) for a stream that already had segments, with
  either a lookup that answered 404 (deleted) or a lookup that says the
  recording is no longer in progress (finished, directory removed), marks the
  content gone: `finished` goes true, every later refresh returns immediately
  without a request, and a read that needs a segment it doesn't already hold
  returns EOF rather than `-1` (which Kodi retries near-immediately). A
  segment 404 in the read path runs one throttled refresh so a viewer
  mid-buffer finds out as well. A lookup that failed some other way (5xx,
  timeout) stays "unknown", and one that says the recording is *still* in
  progress never counts -- a naturally completing recording's status is only
  written after its directory is removed, and that window resolves itself on
  the next refresh.

  Confirmed live on the same harness: after the delete the addon made exactly
  two requests (lookup and playlist, both 404, 0.6s in), logged that the
  recording is gone, and stayed silent; the player ended ~10s later, which is
  Kodi playing out what it had already buffered. The paused-viewer and
  playing-mid-recording variants behaved the same, and a viewer whose addon
  couldn't reach the server while the recording finalized and lost its
  directory ended cleanly (one lookup, one playlist request) once it could --
  the situation that produced 63 consecutive segment 404s before the
  keep-alive existed and would still have, had the server stayed unreachable
  past the viewer-key TTL.

- **Opening an in-progress recording got slower the longer it had already
  been running, and re-paid that cost on every open, not just the first --
  fixed by parallelizing the segment probe and caching results across
  opens.** Reported live (post-1.0.0): opening a ~2h-in recording on macOS
  took 29.4s, with `kodi.log`'s own timing breakdown pinning it exactly on
  `RefreshInProgressRecordingManifest`: 1,800 new-segment probes, 29.417s,
  serialized one at a time (each individual probe was already fast, at a low latency
  average -- `ProbeSegmentByteSize()` already used the shared `CURLSH`
  connection pool, so this wasn't a fresh-connection-per-request problem).
  Root cause: HLS playlists carry each segment's `#EXTINF` duration but
  never its byte size, and Dispatcharr's in-progress-recording HLS segment
  endpoint doesn't support `Range` (see this file's `Broken pipe` entry
  above and `ProbeSegmentByteSize()`'s own comment) -- so this addon has to
  discover each segment's byte size itself via a HEAD probe to build the
  byte-offset index Kodi's `IStream` API needs, and on a cold open, every
  segment the recording has produced so far counts as "new."

  Two independent fixes, addressing the two compounding problems
  separately:
  1. **Parallelized the probing.** `RefreshInProgressRecordingManifest()`
     now parses the playlist into a pending list first, then probes up to
     16 segments concurrently (bounded fan-out via `std::thread`, batched),
     merging results back in playlist order afterward so byte/time offsets
     stay correctly cumulative regardless of which probe actually finished
     first. Assumed safe at the time because this addon's `CURLSH` share
     was already built for concurrent access (see its own comment: "Kodi's
     PVR API can call into this client from multiple threads at once,"
     with real `CURLSHOPT_LOCKFUNC`/`UNLOCKFUNC` callbacks backing it) --
     reasoned that this just exercised that existing thread-safety more
     heavily, without adding a new *kind* of risk. **That assumption was
     wrong** -- see the entry directly below for what a real macOS crash
     report revealed about the difference between occasional cross-thread
     access and a tight same-host concurrent burst.
  2. **Cached probed segments across opens**, keyed by recording id
     (`m_inProgressSegmentCache` in `DispatcharrClient`). Safe because a
     recording's HLS output is genuinely append-only -- a segment probed on
     one open is still at the same byte offset on the next, so re-probing
     it on every reopen (channel switch and back, resuming after pausing in
     the Kodi UI) was pure waste. `OpenInProgressRecordingStream()` seeds
     from the cache before its own cold-start wait loop; the cache entry
     itself is dropped once `finished` goes true, since a finished
     recording plays back through the completed-recording path instead.

  Confirmed live on Windows against a real, currently-recording game (same
  matchup as the original report, coincidentally): a cold open (fresh addon
  instance, no cache) of a recording ~2h15m in, with 2,000 elapsed segments,
  completed in 4.65s total (`4.596s` of probing) -- down from the 29.4s/
  1,800-segments baseline despite having *more* segments to probe this
  time. A same-process reopen right after (`Player.Stop` then `Player.Open`
  again, no addon/DLL reload in between) completed in **0.038s total**,
  probing only the 1 segment that had newly appeared since the close --
  confirming the cache is what closed the gap between "fast on this open"
  and "fast on every open." (A DLL-reload/PVR-client-recreation event
  between two of the manual open attempts during this same test correctly
  reset the in-memory cache and forced a fresh 4.457s cold probe again --
  expected, not a bug: the cache only ever claimed to survive re-opens
  within the same running addon instance.)
- **1.0.1's concurrent segment-probing (the fix directly above) crashed
  outright on macOS -- fixed by giving the concurrent probe burst its own
  `CURLSH` that never shares the connection cache.** Reported live: a current
  macOS, addon 1.0.1, opening the same long-running in-progress recording
  crashed Kodi (SIGSEGV/EXC_BAD_ACCESS, crash report
  `Kodi-2026-01-01-000000.ips`), faulting inside `/usr/lib/libcurl.4.dylib`
  -- macOS's own system libcurl, not a bundled/vendored one -- with a
  backtrace bottoming out in `ProbeSegmentByteSize()` via
  `curl_easy_perform` → `curl_multi_perform` → `multi_runsingle` →
  `multi_done` → `Curl_conncache_return_conn` → `Curl_disconnect` →
  `Curl_conn_close`.

  Root cause: this addon's shared `CURLSH` (`m_curlShareState`) had always
  allowed concurrent access from different threads -- Kodi's PVR API can
  call into this client from more than one thread (background EPG/
  recording-refresh threads alongside active playback) -- and had run
  crash-free through many hours of real testing across all four platforms
  this project supports, including on macOS earlier the same session. What
  changed in 1.0.1 wasn't "concurrent access" in the abstract, but a much
  more intense *pattern* of it: up to 16 threads at once, all hammering
  `curl_easy_cleanup()` within milliseconds of each other, all against the
  identical host (every segment of one recording's HLS output lives under
  the same base URL). That specific stress pattern -- tight, bursty,
  same-host, high-count -- triggered a real concurrency bug in macOS's
  system libcurl's own connection-cache return/close path. The addon's own
  `CURLSHOPT_LOCKFUNC`/`UNLOCKFUNC` callbacks were confirmed correctly
  implemented (bounds-checked mutex array covering every `curl_lock_data`
  type in use) -- this wasn't a locking gap on this addon's side, it's
  libcurl's own share-connection-cache code not holding up under this much
  concurrent churn on this specific build. Per curl's own project history,
  connection-cache sharing (`CURL_LOCK_DATA_CONNECT`) is a substantially
  newer, less battle-tested part of the share interface than DNS or
  TLS-session sharing.

  Fixed by giving `ProbeSegmentByteSize()` -- the *only* call site that's
  ever invoked from a concurrent fan-out, confirmed via `grep` before
  changing anything -- a second, separate `CURLSH`
  (`m_probeCurlShareState`) that shares `CURL_LOCK_DATA_DNS` and
  `CURL_LOCK_DATA_SSL_SESSION` but deliberately never
  `CURL_LOCK_DATA_CONNECT`. This sidesteps the crash mechanism entirely --
  no concurrent thread ever touches a shared connection cache during the
  probe burst -- rather than working around one specific libcurl
  version/platform (e.g. capping probe concurrency to 1 on macOS only,
  considered and rejected: it would regress the very fix this was added
  for, only on the one platform that happened to expose the bug, while
  leaving the same latent risk in place for whatever other platform's
  system libcurl hits it next). Every other call site keeps using the
  original, connection-sharing `CURLSH` exactly as before, unchanged --
  the same combination already proven crash-free through this project's
  extensive prior live testing.

  **Update (2026-10-05, the thirteenth hardening sweep): the "other call
  sites keep connection sharing, proven crash-free" conclusion above did
  not hold, and the main share no longer shares connections either.** The
  crash is not specific to macOS's libcurl: the real `DispatcharrClient`
  linked against stub Kodi functions and driven by 16 threads
  (`GetChannels`/`GetRecordings`/`InvalidateAccessToken`) against a local
  fake server segfaulted inside `curl_easy_perform` on the stock Ubuntu
  24.04 libcurl 8.5.0 in 4 of 5 runs, and 0 of 6 with connection sharing off;
  a 30-line plain-C program with the same lock callbacks crashed 5 of 5 at 8
  threads and 3 of 3 at 16, and never with sharing off (3, 4 and 6 threads did
  not crash in 5 runs each). The addon can have five to seven transfers in
  flight at once, just under that measured threshold, so the earlier
  crash-free experience was luck of low concurrency, not a safe combination.
  `m_curlShareState` now shares DNS and TLS sessions only; the long-lived
  read handles keep their own keep-alive connections, and what is lost is
  reusing one short request's connection for the next. Which libcurl version
  fixed it is not established.

  Confirmed live on Windows after the fix: the same in-progress recording
  opened cleanly with no crash, still fast (2,600 elapsed segments probed
  in 5.878s, consistent with the original fix's numbers) -- the DNS/TLS-
  only probe share preserves the concurrency win, it just no longer shares
  connections while doing it. The actual crash itself could only be
  confirmed fixed on macOS, where it was reported; this addon has no way
  to reproduce macOS's system libcurl locally.
- **A self-heal API-key regeneration during an in-progress-recording open
  could immediately kill the playback that had just started -- fixed by
  telling `OnAddonSettingChanged()` apart a self-persisted key from a
  user-edited one.** Surfaced during the crash-fix verification above (on
  the same live macOS session): right after a recording opened
  successfully, Kodi showed a "PVR clients: Dispatcharr PVR Client --
  Needs to restart" dialog; dismissing it triggered a real
  `UpdateClients: Recreating PVR client` and stopped the playback that had
  just started. Initially suspected as a `TransferSettings`/thread-timing
  interaction from the concurrent probe burst -- reading the actual code
  instead pointed at something more concrete and unrelated to either of
  today's fixes.

  Root cause: `OpenRecordedStream()` (and `ReadRecordedStream()`, same
  pattern) compares the API key before and after opening/reading, and if
  `RefreshInProgressRecordingManifest()`'s proactive self-heal check (see
  its own comment) silently regenerated it during that call, persists the
  new one via `kodi::addon::SetSettingString("api_key", ...)` -- purely so
  a *future* restart doesn't lose it, since `GenerateApiKey()` already
  applied the new key live, in `DispatcharrClient`'s own `m_config.apiKey`,
  the moment it returned. That `SetSettingString()` call is exactly what
  Kodi delivers back to `OnAddonSettingChanged("api_key", ...)`, and its
  existing `m_lastAppliedConfig` guard (added specifically to catch a
  *different*, spurious-renotification quirk -- see that struct's own
  comment) correctly saw a genuine string change and returned
  `ADDON_STATUS_NEED_RESTART`, exactly as it's supposed to for every other
  connection setting. The guard logic itself wasn't wrong; it just had no
  way to know this particular "change" was the addon's own self-heal
  persistence rather than something that actually needs the running
  instance rebuilt.

  This bug predates both of today's other fixes -- it's not a regression
  from 1.0.1's concurrent probing or the macOS crash fix above, just
  surfaced by this session's unusually thorough live testing of a
  long-running in-progress recording (giving the API key's own natural
  expiry window more time to land mid-test). Fixed by updating
  `m_lastAppliedConfig.apiKey` at the same two self-heal sites, before
  their own `SetSettingString()` call, so the ensuing notification
  correctly sees no genuine change and returns `ADDON_STATUS_OK` instead.
  Guarded by a new dedicated mutex (`m_lastAppliedApiKeyMutex`) since
  `apiKey` is now the one `m_lastAppliedConfig` field with two possible
  writers on two different threads (Kodi's own `SetSetting()` dispatch,
  and whichever thread calls `OpenRecordedStream()`/`ReadRecordedStream()`)
  -- every other field still has exactly one.

**A malformed `#EXTINF:` duration in an in-progress recording's HLS
playlist was undefined behavior, not a clean failure.** Found via a
project-wide code review, not a live incident. `RefreshInProgressRecordingManifest()`
parses each segment's `#EXTINF:` duration with `std::stod()`, guarded by
a `try`/`catch (const std::exception&)` that defaults to `0.0` on a parse
failure -- but `std::stod()` accepts `"inf"`/`"nan"` (with an optional
sign) as valid input per the C++ standard, so it does *not* throw for
either. That parsed value later feeds a `static_cast<int64_t>(durationSec
* 1000 + 0.5)` a few lines down, and casting an infinite or NaN `double`
to an integer type is undefined behavior in C++ -- not a catchable
exception the way the analogous gap in this project's two companion
Python plugins was (see `docs/TIMESHIFT.md`'s "A malformed `#EXTINF:`
duration could fail the whole manifest fetch" and
`docs/RECORDING_EDL.md`'s "A malformed `.edl` line..." sections for
those -- this C++ instance was found by deliberately re-checking the
native side for the same failure shape after fixing both Python ones).

Dispatcharr's own DVR ffmpeg is the only realistic writer of this
playlist and isn't expected to ever emit either value -- a defensive
gap, not a reproduced live failure, same as its Python-side siblings.
Fixed by validating the parsed duration with `std::isfinite()` right
after the `std::stod()` call and defaulting to `0.0` if it isn't,
before the value is ever used in the later cast. Confirmed live
(Windows): compiles cleanly and the addon reloads normally. This was
the only `std::stod()`/`std::stof()` call anywhere in the addon's C++
source (checked directly, not assumed), so this closes the entire class
of this specific bug on the native side, not just this one call site.

**A series rule created with "Record all episodes" or "Record only new
episodes" appeared correctly in Kodi and in Dispatcharr, but the actual
upcoming episode never got marked to record -- reported live (2026-09-10)
for a channel with a channel-level EPG-data override.** Both
`POST .../series-rules/` and the immediate `POST .../evaluate/` reported
success throughout, and the EPG data itself was fine (confirmed the exact
programme, byte-identical title, was in the live `/output/epg` export for
that channel) -- so the create/evaluate round trip and the guide data were
both innocent. Root cause traced to Dispatcharr's own channel data:
`Channel.tvg_id` and `Channel.effective_epg_data_id` can point at two
*different* EPGData rows. For the reported channel, an EPG-data override
(set by an auto-channel-merge process) had repointed `effective_epg_data_id`
at a different EPG source's row without updating `tvg_id` to match --
confirmed directly against the API: the channel's own `tvg_id` field
resolved to one EPGData row (a different, unrelated EPG source), while
`effective_epg_data_id` -- the one actually driving the channel's displayed
guide -- pointed at a completely different row with a completely different
`tvg_id`. `CreateSeriesRule()`/`UpdateTimer()`/`DeleteTimer()` all read
`Channel.tvgId` (this addon's cached copy of the channel's own field) when
building a series-rule request, so the rule was created against the
*wrong* EPGData row every time -- one Dispatcharr's own `evaluate/` could
successfully resolve (hence "success" with nothing scheduled), but which
had no matching programme data. This matches a real, general Dispatcharr
bug class: v0.30.0's own changelog describes "series rules resolving the
wrong EPG copy when the same tvg_id exists on multiple sources, and rules
that silently scheduled nothing when the channel used an override EPG,"
fixed there by letting new rules pin a specific `epg_source_id` -- but that
fix only helps a rule that already carries the *correct* tvg_id to begin
with; it doesn't correct a channel whose own `tvg_id` field has drifted
from what its `effective_epg_data_id` actually points to, which is what
was reproduced here. Fixed addon-side with `DispatcharrClient::
ResolveSeriesRuleTvgId()`: before building a series-rule request, look up
the tvg_id that `Channel.epgDataId` (the effective id) actually resolves
to via `GET /api/epg/epgdata/{id}/`, and use that instead of the channel's
own (possibly stale) `tvgId` -- falling back to it if there's no override
or the lookup fails, so a channel without this kind of drift (the common
case) is unaffected. Confirmed end-to-end against the live instance:
deleted the stale rule, recreated it through Kodi with the fix deployed,
and Dispatcharr immediately scheduled a real recording for the specific
upcoming episode that had never been matched before.

**A series rule's own row in Kodi's Timer rules list showed `12/31/1969`
as its start and end time -- reported live (2026-09-10), right after the
fix above.** A series rule is an EPG-title match, not a fixed schedule, so
it has no time of its own -- but its `PVR_TIMER` object never called
`SetStartTime()`/`SetEndTime()` at all, leaving Kodi's zero-initialized
default (rendered in local time as the Unix epoch). Unlike a recurring
rule, whose own row already gets a real time window from its own fields,
and whose materialized children already link back to it via
`recurringRuleId`/`SetParentClientIndex()`, a series rule's children were
never linked back to it either. Fixed by matching each series rule to its
earliest known upcoming/in-progress `Recording` (by channel + title --
Dispatcharr's own rule identity, title+tvg_id+epg_source_id, already rules
out two rules sharing a title on one channel, so this is unambiguous) and
using that occurrence's real times on the rule's own row, plus wiring up
`SetParentClientIndex()` the same way recurring rules already do so the
matching recording nests under the rule in Kodi's UI too. Confirmed live:
after the fix, the rule's own row showed the same real start/end time as
its matched child recording instead of the epoch.

**Update (2026-09-26): the epoch bug above could still come back
through a title case mismatch, found via a project-wide review by
downloading and reading Dispatcharr's own current upstream source
(`apps/channels/tasks.py`, `main` branch as of this writing) into a
scratchpad -- not written into this repo, and not the same standard of
proof as a live test against a real running instance, but stronger than
guessing from the API shape alone.** `evaluate_series_rules_impl()`
matches "exact" `title_mode` case-insensitively
(`programs_qs.filter(title__iexact=series_title)`), so a rule whose
title differs only in case from the EPG programme's own title still
records correctly server-side -- but `MatchRecordingsToSeriesRules()`'s
own title comparison was case-sensitive, purely for this addon's own
client-side re-linking (parent-index/display-time only; the recording
itself was never at risk). A title typed in a different case than the
EPG's own (more likely now that "Search guide for" is the field that
actually reaches Dispatcharr -- see the entry just below) recorded fine
but never linked back to its own rule row, reintroducing the epoch
display and leaving the recording appearing as an unparented one-time
timer. Fixed by comparing case-insensitively
(`dispatcharr::ToLower()`, `StringUtil.h`).

**Update (2026-09-27, a 68th-pass audit): the same epoch/unparented
symptom was still reachable through leading/trailing whitespace on the
rule's own title, confirmed against Dispatcharr's own real current
upstream source (the same scratchpad clone), not itself independently
reproduced.** `evaluate_series_rules_impl()` matches on
`(rule.get("title") or "").strip()`, but `SeriesRulesAPIView.post()`
(`apps/channels/api_views.py`) stores `title` exactly as sent, and
Kodi's own timer dialog doesn't trim "Search guide for" either
(`GUIDialogPVRTimerSettings.cpp`) -- so a stray space typed there
reached Dispatcharr verbatim, recorded fine, and never linked back.
`MatchRecordingsToSeriesRules()` now strips the rule's title (ASCII
whitespace only at that point -- see the update just below for the
Unicode half) before comparing; the recording's own title is
deliberately left unstripped, since the server compares against each
programme's raw title too.

**Update (2026-09-30): the same epoch/unparented symptom was still
reachable through a rule whose `title_mode` isn't "exact", through
non-ASCII whitespace and letter case, and through a description filter --
all confirmed live against the real instance and fixed.** Dispatcharr
materializes a recording from a series rule with nothing but a `program`
snapshot (title, description, tvg_id, epg_source_id) in
`custom_properties` -- never a reference back to the rule -- so this addon
can only link a recording to its rule by re-running the rule's own
filters against that snapshot, and had only ever re-implemented the
"exact" one, and only for ASCII. Reading the rest of the server's side
(`_evaluate_series_rules_locked()`, `apps/epg/query_utils.py`, Dispatcharr
0.31.0) showed what those filters really are: `"exact"` is
`title__iexact`; every other title mode (including `"contains"`, and any
mode name the server doesn't know) goes through `parse_text_query()` -- an
`icontains` with an AND/OR/double-quote/parenthesis grammar applied
strictly left to right, `"search"` anchoring each term on a regex word
boundary, `"regex"` passing the whole value to PostgreSQL as a regular
expression; a description is always run through that same parser; and a
rule's title and description filters are ANDed. Both the title and the
description are `.strip()`ped (Python's, so the no-break space and the
rest of Unicode whitespace too) while the stored rule stays unstripped.

Confirmed live before changing anything: a `contains` rule "ZZZ_TEST Alpha"
beside a recording "ZZZ_TEST Alpha Report" (an exact-mode control linked, the
`contains` rule read "Any day at any time"), a rule with a trailing no-break
space, and a rule "ZZZ_TEST ÜNDER TEST" against a recording "ZZZ_TEST ünder
test" all stayed unlinked. And a probe through the channel list's `icontains`
filter (`UPPER(name) LIKE UPPER(...)` -- the same function `title__iexact`
uses) found a channel named "ÜNDER" by searching "ünder", so the real
instance's database does fold non-ASCII case, as the stock Debian-based
postgres image's libc collation does.

Fixed by `SeriesRuleTextMatch` (exact, contains and search modes, a
faithful port of `parse_text_query()` -- see its header for the quirks it
reproduces on purpose) and `UnicodeText` (Python-identical whitespace
stripping, and simple-uppercase case folding from a table generated from
glibc's `towupper()`, the function PostgreSQL's `UPPER()` calls on such a
database -- not from Python's `str.upper()`, whose full mapping differs in
exactly the places that matter here). A regex filter is deliberately left
unevaluated: PostgreSQL's regular-expression syntax isn't ECMAScript's, so
a regex rule stays unlinked rather than being guessed at. Verified against
the real upstream parser (loaded unmodified, with Django's `Q` replaced by
a small fake that keeps its empty-operand rule) on ~108,000 random
query/text pairs with zero disagreements, every Unicode code point against
glibc and Python exhaustively, and live: eleven rules covering each mode
beside eight recordings, every one linked or left unlinked exactly as
predicted.

**Series timers sent Kodi's cosmetic "Name" field to Dispatcharr as the
actual EPG-title match pattern instead of "Search guide for", found via
a project-wide review (2026-09-26), not itself independently
reproduced.** `GetTimerTypes()` declares
`PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH` for the series type, which
makes Kodi's own timer-settings dialog show "Search guide for"
(`timer.GetEPGSearchString()`) as a separate field from "Name"
(`timer.GetTitle()`) -- confirmed against Kodi's own source
(`xbmc/pvr/dialogs/GUIDialogPVRTimerSettings.cpp`): the two fields are
edited and saved back completely independently, with no relationship
Kodi itself enforces between them. But `AddTimer()`/`UpdateTimer()`'s
own series branches sent `GetTitle()` to `CreateSeriesRule()` as the
actual match pattern and never read `GetEPGSearchString()` at all, and
`GetTimers()` never called `SetEPGSearchString()` either. Two real
consequences: editing "Search guide for" alone was silently dropped
(the addon never read it), and editing "Name" alone -- which Kodi's own
dialog presents as a cosmetic label -- silently changed Dispatcharr's
own match rule instead, creating a *second*, separate rule under the
new identity per the upsert limitation documented above rather than
renaming the original. The ordinary "Record" button from an EPG guide
entry happened to work correctly only by coincidence: Kodi's own
`CPVRTimerInfoTag::CreateFromEpg()` sets both fields to the same EPG
title for a brand-new timer, so nothing ever revealed the addon was
reading the wrong one. Fixed by routing both create/update
(`dispatcharr::ResolveSeriesRuleMatchTitle()`, `TimerIdentity.h` --
prefers `GetEPGSearchString()`, falling back to `GetTitle()` only when
the search string is empty, matching Kodi's own dialog convention for a
brand-new timer) and display (`GetTimers()` now also calls
`SetEPGSearchString(rule.title)`, since Dispatcharr's own single
`title` field serves as both the cosmetic name and the match pattern --
there's no separate field server-side) through the actual match
pattern instead.

**Update (2026-09-26, a 17th-pass audit): the fix above means the
existing "editing a series rule's identity creates a duplicate instead
of renaming" limitation (documented in `UpdateTimer()`'s own comment,
"Not worked around here") is now reached through the *correct* field
("Search guide for") instead of the previously-read, cosmetic one
("Name").** This isn't a new bug from the fix above -- that exact
create-a-duplicate-on-identity-change behavior already existed and was
already documented before this fix, and editing "Name" pre-fix
triggered the identical duplicate-creation path, just via the wrong
field. What changes is that a genuine "widen my search pattern" edit
from Kodi -- the scenario this field exists for -- now reaches that
same limitation directly, rather than being silently dropped as it was
before. Confirmed against Dispatcharr's own current upstream source
(`SeriesRulesAPIView.post()`, downloaded to a scratchpad for review,
not committed to this repo): the upsert genuinely appends a new rule
whenever `(tvg_id, title, epg_source_id)` doesn't match an existing one,
leaving the old rule (and its own future recordings) fully in place --
not just a guess from the API shape. A more complete fix (detect the
identity change against the cached rule and delete the old one first)
is possible but has a real data-loss-ordering subtlety of its own
(delete-then-create risks losing the rule entirely if create then
fails; create-then-delete risks the old rule's own delete wiping
recordings the new rule's server-side evaluation had already
materialized) -- logged to `docs/OPEN_ITEMS.md` rather than implemented
blind, since it needs a live test either way.

**Editing a channel-less series rule (one with no pinned channel) from
Kodi always failed with a 400, confirmed and fixed by an 18th-pass audit
(2026-09-26) that cloned Dispatcharr's own real current upstream source
into a scratchpad, never committed to this repo.** Such a rule is real
and reachable in practice, not hypothetical -- Dispatcharr's own
"Record series" Guide button creates one with no pinned channel, sent
into Kodi as `PVR_CHANNEL_INVALID_UID` (-1) since this addon's own
convention for "no channel" is `channelId <= 0`.
`DispatcharrClient::CreateSeriesRule()` unconditionally included
`channel_id` in its POST body, unlike `tvg_id` (already conditionally
omitted when empty) -- so an edit sent `channel_id: -1` literally,
which Dispatcharr's own validation rejects outright ("channel_id does
not exist"), since Dispatcharr's own schema documents `channel_id` as
genuinely optional too ("Optional channel to pin recordings to"). Fixed
by omitting `channel_id` from the body whenever `channelId <= 0`,
mirroring the existing `tvg_id` pattern. Still open: such a rule's own
row in Kodi's Timers list still shows the Unix-epoch placeholder and
its recordings still appear unparented, since
`MatchRecordingsToSeriesRules()` requires a matching `channelId` --
logged to `docs/OPEN_ITEMS.md` rather than fixed this same pass, since a
proper fix needs `RecordingParser` to also read
`custom_properties.program.tvg_id`, a larger change.

**Update (2026-09-26, a 30th-pass audit, confirmed against Kodi's own
real current SDK source, not itself independently reproduced): the
above never actually let a real Kodi-driven edit of a channel-less rule
reach this addon at all.** Kodi's own `GUIDialogPVRTimerSettings` only
auto-selects a channel for an *existing* timer whose channel uid is
`PVR_CHANNEL_INVALID_UID` when the timer type declares
`PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL` (never declared here) -- without
it, editing such a rule from Kodi's own dialog failed with "Could not
update the timer" before this addon's `UpdateTimer()` was ever called,
for the whole edit, not just the channel. Fixed this pass: the series
timer type now declares that flag, and `UpdateTimer()`'s own cache-match
comparison (which a plain `channelId == clientChannelUid` check would
still miss, since one side's "no channel" sentinel is `0` and the
other's is `-1`) now goes through `dispatcharr::IsSameSeriesRuleChannel()`
(`TimerIdentity.h`) instead. See `docs/OPEN_ITEMS.md`'s own entry for
the fuller account, including what still needs a live dialog check.

**A `<date>` value can be a series-level placeholder, not a real
per-episode original air date -- reported live (2026-09-10) as a
recording showing an implausibly old date despite being a genuinely new,
same-day episode, not a rerun.** Initially assumed to be legitimate data
(the affected show is long-running, so an old air date isn't
implausible on its face) until the user pointed out these specific
recordings weren't reruns. Checked the raw Dispatcharr data for five
upcoming instances of the same daily show, airing on five different
calendar dates: all five carried the *identical*
`custom_properties.program.original_air_date`, and none
had any `season`/`episode`/`onscreen_episode` identifier at all --
unlike an actual episodic programme with real, distinct per-episode
identifiers. A single fixed date
across every distinct airing of a still-running daily show is not a real
fact about any of those specific episodes; it's almost certainly a
series-level value (possibly from Dispatcharr's TVMaze poster/metadata
cross-reference, given the poster URL's domain, rather than the raw
upstream guide feed itself) stamped onto every instance because
the guide source has no true per-episode date for a long-running daily show.
`GetEPGForChannel()` was mapping XMLTV's `<date>` element straight to
Kodi's `FirstAired` unconditionally (see `docs/EPG.md`), so this
misleading value surfaced anywhere Kodi shows `FirstAired` for a timer or
recording tied to that EPG entry. Fixed by only setting `FirstAired` when
the entry also has a real season or episode number (`entry.seasonNumber
> 0 || entry.episodeNumber > 0`, the same signal already used for
`EPG_TAG_FLAG_IS_SERIES`) -- a programme with real episode identity keeps
getting its `FirstAired` exactly as before.

**Update: the same `<date>` value also leaked through via `Year`,
independently of the `FirstAired` fix above -- caught live (2026-09-10)
immediately after deploying it, when a fixed recording's home-screen date
badge changed from the full misleading date to a bare misleading year
(a placeholder year) instead of disappearing.** Root cause: `Year` (`SetYear()`,
`<date>`'s leading 4 digits) was left unconditional in the same pass that
guarded `FirstAired`, even though it derives from the exact same
unreliable value for the exact same episode-less programmes -- Kodi's
widget fell back to it once `FirstAired` was empty rather than showing
nothing. Fixed by applying the identical `entry.seasonNumber > 0 ||
entry.episodeNumber > 0` guard to `Year` too. Confirmed live: of the five
affected daily-show broadcasts checked via `PVR.GetBroadcasts`,
four now return both `firstaired: ""` and `year: 0`; the fifth (already
actively recording at deploy time) still returns the old cached
placeholder date/year for both fields, from Kodi's own separate EPG cache
-- the same pre-existing gotcha noted in the entry above, not a gap in
this fix.

**`RefreshInProgressRecordingManifest()` fetched every recording just to
check one's `isInProgress` flag -- found via an efficiency review
(2026-09-10), fixed and live-verified the same day.** This runs on every
throttled manifest refresh during in-progress-recording playback (up to
~2/sec), and was calling `GetRecordings()` -- a full `GET
/api/channels/recordings/` plus a JSON parse of every recording returned
-- purely to find the one matching `recordingId` and read its
`isInProgress` flag. The original note flagging this left `GET
/api/channels/recordings/{id}/` as "likely supports... but unconfirmed
against real source" -- confirmed directly against a live instance before
writing any code: real, `HTTP 200`, identical shape to a list item, a
standard DRF `retrieve` route matching the already-used
`{id}/stop/`/`{id}/extend/` siblings. New
`DispatcharrClient::GetRecordingById()` uses it; `GetRecordings()`'s own
per-item field-mapping was pulled out into `ParseRecordingJson()` so both
paths share identical parsing rather than duplicating it. Live-verified
against a real in-progress recording during actual playback: `kodi.log`'s
existing timing breakdown showed `GetRecordingById 0.006-0.009s` on every
refresh cycle (down from fetching and parsing the full recordings list --
a real, non-trivial number of recordings on the live instance this was tested against), with
`finished=0` correctly reflected throughout, exercising the exact same
code path a real playback session already relies on. At the time this
was written, deliberately left `PVRDispatcharr::FindRecordingById()`
alone -- a different, pre-existing helper that scanned an *already-fetched*
`std::vector<Recording>` a caller passed in, not a new REST call itself.
**Update: simplified anyway once `GetRecordingById()` existed -- see the
recordings/timers caching entry below.**

**`GetRecordingsAmount()`/`GetRecordings()` and `GetTimersAmount()`/
`GetTimers()` each independently re-fetched on every Kodi refresh --
the other two findings from the same efficiency review as the entry
above, fixed and live-verified the same day (2026-09-10).** Kodi calls
the `Amount()` half of each pair and then the `List()` half essentially
back-to-back on every refresh; both halves called `m_client.GetRecordings()`/
`GetTimerRules()`/`GetRecurringRules()` completely independently, so a
single logical refresh cost 2-3x the real REST round trips it needed.
Fixed with `EnsureRecordingsLoaded()`/`EnsureTimerRulesLoaded()`, the
same staleness-cache shape as the pre-existing `EnsureChannelsLoaded()`/
`EnsureEpgLoaded()` pair, but with a 2-second TTL
(`kRecordingsAndTimersCacheTtlSeconds`) instead of
`channel_refresh_hours`/`epg_refresh_hours` -- recordings/timers can
change the instant the user acts, unlike channels/EPG, so an hours-scale
cache would risk showing stale state right after the user's own change.
That TTL alone isn't enough on its own, though: `TriggerRecordingUpdate()`/
`TriggerTimerUpdate()` are Kodi SDK base-class methods (not something this
addon defines, so their own implementation can't be edited to add
invalidation), so two new wrapper methods,
`InvalidateAndTriggerRecordingUpdate()`/`InvalidateAndTriggerTimerUpdate()`,
reset the relevant cache timestamp before delegating to the real
trigger -- applied mechanically across every one of the ~10 existing call
sites (confirmed via `grep` that none were missed), so a change from
`AddTimer()`/`UpdateTimer()`/`DeleteTimer()`/the realtime-update
WebSocket handler/the recording-refresh background thread is never
delayed by the TTL, only genuinely idle refreshes are. Also simplified
`PVRDispatcharr::FindRecordingById()` (see the entry above) to call
`DispatcharrClient::GetRecordingById()` directly instead of its own
separate full-list fetch+scan, now that that REST call exists -- a small
additional win beyond the original three findings, since its callers
(`GetRecordingStreamProperties()`/`OpenRecordedStream()`/`UpdateTimer()`'s
extend-recording branch) want this one recording's truly current state
right before acting on it, so it deliberately stays a fresh single-item
lookup rather than reading the (up to 2s stale) cache.
Live-verified against the real instance: 8 rapid-fire `PVR.GetTimers`/
`PVR.GetRecordings` calls, matching Kodi's own Amount()+List() pattern,
produced exactly 2 real cache-refresh log lines (not 8), correctly
straddling the 2-second TTL across the two request batches. Separately
confirmed invalidation actually works, not just the TTL: added a real
one-time timer via `PVR.AddTimer` and watched `kodi.log` -- the very
next `GetRecordings()` call (triggered by the addon's own
`InvalidateAndTriggerRecordingUpdate()`) refetched immediately and
picked up the new recording well within the 2-second window
that would otherwise still have been "fresh." A momentary miss in the
timer *count* right at the exact instant of creation was traced
separately to a pre-existing sub-second race in the `isInProgress`/
`isUpcoming` time-window check (the new recording's `start_time` landed
at essentially the same wall-clock instant as the query itself) --
resolved on its own within ~3 seconds and confirmed unrelated to this
cache, not a regression it introduced.

**The `EnsureRecordingsLoaded()` debug log line ("recordings cache
refreshed (N recording(s))") reports the raw fetch count, not
`GetRecordings()`'s own filtered output -- investigated as a suspected
data-loss bug (2026-09-15) on a real account with many cached items but
only a fraction actually reaching Kodi's own `PVR.GetRecordings`, and turned out
to be entirely expected behavior, not a bug anywhere.** Dispatcharr's
`/api/channels/recordings/` returns past/completed and future/scheduled
items together in one list -- confirmed live by adding temporary
per-item diagnostic logging to `GetRecordings()`'s transfer loop: all
"missing" items had a genuinely positive `start_time` (ranging from
minutes to about a week out), `recurringRuleId == 0` (ruling out a
suspected leftover-occurrence artifact from unrelated same-day
recurring-rule testing), and were the account's own ordinary
daily/weekly recurring shows -- exactly what `GetTimers()` already
separately reports as scheduled. `GetRecordings()`'s `isUpcoming` filter
(see its own comment) is deliberately excluding them, correctly: an
item that hasn't started yet belongs in Timers, not Recordings. No
duplicate `recordingid` and no exception/early-exit in the transfer loop
were found either (both checked directly, since they were the other two
leading theories before the `isUpcoming` breakdown above settled it).
Worth remembering next time this cache log's count doesn't match what
`PVR.GetRecordings` shows: that's normal whenever the account has any
upcoming scheduled recordings at all, not a sign of a broken transfer.

**`PVR.AddTimer` against a broadcast that has already started airing
(rather than a genuinely future one) created two separate timers/
recordings server-side from a single JSON-RPC call -- confirmed live
(2026-09-15, CoreELEC/ODROID N2+) while exercising
`check_in_progress_recording_playback`/`seek` against a real account for
the first time.** This addon's own `AddTimer()` (`src/PVRDispatcharr.cpp`)
makes exactly one `CreateOneTimeRecording()` call regardless -- confirmed
by reading it -- so the duplication happens on Dispatcharr's own side,
not in this addon. `tools/kodi_smoke_test.py`'s own timer-creation helper
(`_add_and_verify_timer`) had never exercised this specific case before:
it deliberately only ever picks a *future* broadcast (see its own
docstring on the blocking-dialog hang an already-finished one causes),
so recording an *already-airing* broadcast by `broadcastid` was a
genuinely new code path for this project's own testing, only reached
because `check_in_progress_recording_playback`/`seek` need a real
recording already in progress right now rather than one scheduled ahead
of time. Both resulting timers deleted cleanly via `PVR.DeleteTimer`
with no further side effects observed; one of the two recordings kept
its originally-scheduled end time after being stopped early, matching
the already-documented `custom_properties.status`-vs-`end_time` display
quirk above rather than anything new. Not investigated further on
Dispatcharr's own side (its own server-side recording-creation logic is
outside this repo) -- worth knowing about if scheduling a recording
against an already-airing broadcast ever needs revisiting.

## Recording-management feature gaps vs. TVHeadend, checked against Dispatcharr's real API (2026-09-08)

*The whole-addon comparison with the Tvheadend addon's source, done on
2026-10-10, is `docs/PVR_HTS_COMPARISON.md`; this section is the
earlier, recording-management-only pass it builds on.*

Prompted by a "what does TVHeadend have that this addon doesn't"
question. Comparing this addon's declared Kodi `PVRCapabilities`
(`GetCapabilities()` in `src/PVRDispatcharr.cpp`) against everything
the Kodi PVR API exposes, filtering out the DVB/tuner-specific flags
that don't map to an IPTV backend anyway (channel scan, channel
settings, descramble info), left five real gaps, all recording
management: rename, undelete, per-recording retention/lifetime,
recording file size, and resume position/play count. Checked each
against Dispatcharr's actual behavior rather than guessing -- first its
live OpenAPI schema (`GET /api/schema/` against a real instance), then,
since DRF-spectacular's auto-generated `requestBody` for a custom
`@action` frequently just reuses the ViewSet's default serializer
schema regardless of what the view actually reads from `request.data`
(confirmed exactly this for the endpoint below), Dispatcharr's real
source (`apps/channels/api_views.py`, fetched via `gh api
repos/Dispatcharr/Dispatcharr/contents/...` since GitHub's code-search
API refuses unauthenticated requests).

**Rename/description edit is real and already writes into the exact
field this addon already reads.** `POST
/api/channels/recordings/{id}/update-metadata/` takes a plain
`{"title": ..., "description": ...}` body (confirmed from the view's
own source, not the schema, which -- as above -- just shows the whole
`Recording` serializer as the body and doesn't mention title/description
at all despite the endpoint's own docstring explicitly saying "Update
user-editable recording metadata (title, description)"). It writes
straight into `custom_properties.program.title`/`description` and sets
`custom_properties.program.user_edited = true` to stop the EPG
auto-enrichment task from overwriting it on a later run -- the exact
`custom_properties.program.*` path `GetRecordings()` already reads on
the way in (see this file's own note above on that nesting). Genuinely
implementable: add `SetSupportsRecordingsRename(true)` to
`GetCapabilities()` and a `RenameRecording()` callback that POSTs here.

**Update: implemented and confirmed live end-to-end (2026-09-09).**
Exactly that -- `SetSupportsRecordingsRename(true)` plus
`PVRDispatcharr::RenameRecording()` calling the new
`DispatcharrClient::RenameRecording(recordingId, newTitle, error)`,
which POSTs `{"title": newTitle}` (description deliberately omitted
entirely, not sent as an empty string, so the server-side "None means
no change" logic in `update_metadata` leaves the existing description
alone). Kodi has no JSON-RPC method for triggering a rename at all --
confirmed by checking its full method list -- so this had to be tested
through the actual GUI: navigated Kodi's own recordings list, opened a
real recording's context menu (which only shows an "Edit" entry once
`SetSupportsRecordingsRename` is true -- itself a first confirmation
the capability wired up correctly), and used its rename dialog to
change a real in-progress-turned-stopped recording's title from its
real EPG-sourced show name to "RENAMETEST". Confirmed two ways: Kodi's own
`PVR.GetRecordings` reported the new title back immediately, and a
direct check against Dispatcharr's REST API showed exactly the
expected write -- `custom_properties.program.title` updated,
`custom_properties.program.user_edited: true` set, `description`
completely untouched (still the original EPG-sourced text) confirming
the addon's own "omit the field entirely" choice does what it's
supposed to.

**Recording file size is available, just not as a JSON field.** The
`Recording` model itself really does have no size field (confirmed
against the live schema: `id`/`start_time`/`end_time`/`task_id`/
`custom_properties`/`channel`, nothing else, matching this file's
existing note on the model's minimalism) -- but `/api/channels/
recordings/{id}/file/`'s own handler computes it server-side
(`os.path.getsize(file_path)`) and reports it as a normal HTTP
`Content-Length` header. A `HEAD` request against that endpoint gets
the size without downloading anything. Implementable:
`SetSupportsRecordingSize(true)` plus a `HEAD` call in whatever
populates `PVRRecording`'s size field.

**Update: implemented and confirmed live end-to-end (2026-09-09) --
simpler than the `HEAD`-request plan above, once actually checked
against Dispatcharr's finalization code.** The `HEAD`-against-`/file/`
approach was never built: `custom_properties.bytes_written` turned out
to already be present in the exact same `GetRecordings()` response this
addon already fetches, no second HTTP call needed per recording.
Confirmed against `apps/channels/tasks.py`'s real source: it's a sum of
the recording's HLS segment (`seg_*.ts`) file sizes, written into
`custom_properties["bytes_written"]` only once the recording task
reaches its post-processing/finalization step -- not updated live while
still recording, which is why `Recording::bytesWritten` (new field in
`DispatcharrClient.h`) defaults to 0 whenever the key is absent from
`custom_properties`, rather than treating absence as an error.
`SetSupportsRecordingSize(true)` plus `PVRRecording::SetSizeInBytes()`
in `GetRecordings()`'s population loop surface it. Tested live on
Windows with a real instant recording (`PVR.Record`) across its full
lifecycle, confirmed via temporary debug logging of the parsed
`custom_properties` state at each stage: in-progress
(`custom_properties` genuinely has no `bytes_written` key yet), stopped
but not yet finalized (still absent -- `_hls_dir` still present,
matching this file's own note above on that window), and finalized
(key present, real value: its true byte count). Kodi's
own GUI confirmed the same number two independent ways -- the
recordings list's per-folder "Total: <its size>", and a dedicated
"Size: <its size>" line in the recording's own info panel -- both
matching the raw byte count exactly. Also confirmed the boundary case
live: a recording that never captured real stream data
(`custom_properties.status == "interrupted"`, from a channel with no
real backing live stream) correctly parsed `bytesWritten=0`, and Kodi's
info panel omitted the Size line entirely rather than showing a
misleading "Size: 0 B".

**Extending an in-progress recording is real, dedicated, and currently
unused by this addon at all.** `POST /api/channels/recordings/{id}/
extend/` moves a still-recording's `end_time` forward without
interrupting the stream (the running Celery task re-reads `end_time`
every ~2s and adjusts its own deadline live, confirmed from the source).
Not one of the original TVHeadend-comparison items, but a genuine find
while checking this: grepping this addon's own source for `extend`/
`ExtendRecording` turns up nothing -- there's currently no way to do
Kodi's usual "record for longer" from this addon at all, despite
Dispatcharr already supporting it cleanly server-side.

**Update: implemented and confirmed live (2026-09-09) -- and a real
near-miss caught by reading the endpoint's own source before wiring it
up.** `GetTimers()` already surfaces every in-progress recording as a
`PVR_TIMER_STATE_RECORDING` timer (that's how Kodi's "record for
longer" reaches an addon at all -- editing that timer's end time in
Kodi's own Timers window, no separate UI concept exists). The obvious
implementation would have been routing that edit through
`UpdateTimer()`'s existing one-time-recording branch,
`UpdateOneTimeRecording()`'s plain `PATCH .../{id}/`, exactly like a
not-yet-started recording's reschedule already does. Checked against
the real `extend` action's source first (`apps/channels/api_views.py`)
and found why that would have been wrong: its own docstring explains
the endpoint deliberately uses `queryset.update()` specifically to
*bypass* the model's `pre_save` signal, because that signal revokes the
scheduled/running Celery recording task -- i.e. a generic PATCH against
an already-recording item would have gone through the normal `.save()`
path and stopped the recording being "extended," the opposite of the
intent. New `DispatcharrClient::ExtendRecording(id, extraMinutes,
error)` calls the dedicated endpoint instead; `UpdateTimer()` now
branches on `timer.GetState() == PVR_TIMER_STATE_RECORDING` and takes
this path only for an already-recording timer (a not-yet-started
one-time recording's reschedule is untouched, still the original PATCH
-- correct there, since there's no running task yet to protect).
Dispatcharr's endpoint takes a relative `extra_minutes`, not the
absolute end time Kodi hands back from its edit dialog, so the current
end time is fetched fresh via `GetRecordings()` right before computing
the delta (Kodi doesn't send the pre-edit value, and this addon's own
last-polled copy could be stale); a delta `<= 0` (an unchanged or
earlier end time -- Kodi's dialog has no separate "shorten" action) is
rejected client-side as `PVR_ERROR_INVALID_PARAMETERS` before any
request is sent, matching the server's own validation. Tested live on
Windows against a real instant recording: extended an in-progress timer's end
time by 15 minutes via Kodi's actual Timers-window edit dialog (its
"Numeric pad" time-entry sub-dialog needed real numeric-pad key actions
-- `Input.SendText` fed it a garbled value, `Input.ExecuteAction` with
`number1`..`number9` worked correctly and matched what the on-screen
digits showed at each step). Confirmed via `PVR.GetTimers` immediately
after: `endtime` moved from `17:00:00` to `17:15:01` UTC and `state`
stayed `"recording"` throughout -- direct proof the running task kept
going rather than being revoked, the exact risk the dedicated endpoint
exists to avoid. (The extra 1 second beyond a clean 15:00 delta is
integer-minute rounding against Dispatcharr's own `extra_minutes` API,
not a bug on this addon's side.)

**Undelete, retention/lifetime, and resume-position/play-count are
confirmed *not* implementable -- Dispatcharr genuinely has no backend
for any of them, not just an unexposed one.** `RecordingViewSet.destroy()`
(the real `DELETE` handler, read directly, not inferred) deletes the DB
row first, then tears down any live DVR client and removes the file(s)
from disk in a background thread -- immediately destructive by design,
no soft-delete/trash table anywhere for an "undelete" to restore from.
Grepping `api_views.py` for `retention`/`lifetime`/`resume`/
`play_count`/`last_played` turns up nothing at all -- no per-recording
or global auto-delete policy, and no resume-position or play-count
concept anywhere server-side. Resume position could theoretically be
faked as a purely addon-local value (Kodi's own storage, or an
unofficial key stashed in `custom_properties` this addon invents
itself), but that wouldn't survive a reinstall or follow the recording
across devices the way `update-metadata`'s title/description do, so
it's a materially weaker win than the two implementable items above --
not pursued further without the user actually wanting the tradeoff.

Originally a findings/feasibility pass, not a change -- all three
implementable items (rename, file size, extending an in-progress
recording) have since been implemented and confirmed live (see the
"Update" paragraphs above). See `docs/OPEN_ITEMS.md` for the tracked
history.

**Update (2026-09-26, a 19th-pass audit): `DeleteTimer()`'s series-rule
branch could send the wrong title on its rare cache-miss fallback,
found via a project-wide review, not itself independently reproduced.**
`DeleteTimer()` prefers the rule's own originally-stored identity from
`m_cachedTimerRules` (see the `FindSeriesRuleIndexByClientIndex()` fix
in `TimerIdentity.h`), falling back to a freshly re-derived title/tvg_id
only when the rule isn't found in the cache at all. That fallback took
`title = timer.GetTitle()` ("Name", a cosmetic label) directly, rather
than `dispatcharr::ResolveSeriesRuleMatchTitle(timer.GetEPGSearchString(),
timer.GetTitle())` -- the same "Search guide for" preference
`AddTimer()`/`UpdateTimer()` already apply when building the identical
kind of request. A series rule whose match pattern differs from its
displayed name (edited "Search guide for" separately from "Name",
confirmed as two genuinely independent fields against Kodi's own
source -- see `ResolveSeriesRuleMatchTitle()`'s own comment) hitting
this fallback path sent a title Dispatcharr's own title+tvg_id delete
identity never matched, silently no-opping the delete -- the same
failure mode the tvg_id-preference fix above exists to avoid, just
through the title half of that same identity instead. Fixed by using
`ResolveSeriesRuleMatchTitle()` here too.

**Update (2026-09-27, a 41st-pass audit): a real, confirmed latch bug in
Kodi's own `CPVRRecording::IsInProgress()` could permanently mark a
genuinely-in-progress recording as finished, found via a project-wide
review and confirmed against Kodi's own real current SDK source, not
itself independently reproduced.** `IsInProgress()` (`PVRRecording.cpp`)
only re-checks `GetRecordingTimer()` (whether a matching RECORDING-state
timer still exists) while its own cached `m_bInProgress` is still
`true` -- the moment that check ever comes back `false`, it latches
permanently, never re-checking again regardless of what changes
afterward. A brand-new `CPVRRecording` starts with `m_bInProgress = true`
(`Reset()`), so a just-started recording whose corresponding timer
hasn't reached Kodi's own Timers list *yet* gets latched to "not in
progress" the instant anything calls `IsInProgress()` on it in that
gap. Something does, ambiently, roughly once a second regardless of any
user action: `CPVRManager::Process()`'s own main loop (confirmed:
`CThread::Sleep(1000ms)` between iterations) calls
`TriggerRecordingsSizeInProgressUpdate()` (which calls `IsInProgress()`
on every recording) on every iteration, once any client declares
`SetSupportsRecordingSize(true)` -- which this addon does. The addon's
own `InvalidateAndTriggerRecordingUpdate()`/`InvalidateAndTriggerTimerUpdate()`
pair (`PVRDispatcharr.cpp`) each schedule an independent, asynchronous
Kodi job backed by its own separate HTTP round-trip to Dispatcharr; two
of that pair's three call sites (the periodic recording-refresh
thread's own loop, and `HandleRealtimeUpdateMessage()`) fired the
recordings trigger *before* the timers trigger, leaving exactly the gap
described above open between the two round-trips completing --
`AddTimer()`'s own already-established order (timers, then recordings,
with its own comment on why) never had this problem. Once latched, the
symptom is a real, live-confirmed-mechanism (not just cosmetic)
regression: Kodi's own `CPVRContextMenus`-driven "Stop recording" option
disappears in favor of "Delete" (this addon's own `DeleteRecording()`
sends an unconditional `DELETE`, ending the still-genuinely-running
recording and removing its partial file), the recording can get marked
watched at the live edge, and `PVR.IsPlayingActiveRecording` reads
false. Fixed by swapping the order at both remaining call sites to match
`AddTimer()`'s: timers before recordings.

**Update (2026-09-27, a 42nd-pass audit): four more sites left the same
window open, found via a project-wide review, confirmed against Kodi's
own real current SDK source, not itself independently reproduced --
worse than the two the update above fixed, since none of these four
queued a timer refresh at all, not even one running slightly behind.**
`DeleteRecording()` and `RenameRecording()` each fired only
`InvalidateAndTriggerRecordingUpdate()`, `AddTimer()`'s own delayed
5-second re-enrichment thread (see its own comment on why that delay
exists) did the same, and `OnSystemWake()` fired no recordings/timers
trigger of any kind. The risk isn't specific to whichever recording a
given call is actually acting on -- a recordings-only refresh re-fetches
*every* recording, so it's equally capable of exposing a *different*,
freshly-started recording (one Dispatcharr began recording since this
addon's last successful `GetRecordings()`) to Kodi's own ambient,
roughly-once-a-second `IsInProgress()` check (see the update above) if
its own corresponding timer update hasn't independently caught up yet.
`OnSystemWake()` matters more than it looks: `CPVRManager::OnWake()`
(`PVRManager.cpp`) calls `CPVRClients::OnSystemWake()` -- which is what
invokes this addon's own `OnSystemWake()` callback, synchronously, for
every client -- *before* `OnWake()` itself goes on to call
`TriggerRecordingsUpdate()` ahead of `TriggerTimersUpdate()` a few lines
later; Kodi's own job queue (`CPVRManagerJobQueue::AppendJob()`,
confirmed FIFO, deduping only against another already-pending job of
the exact same type) means a timer-update job queued from inside this
addon's own `OnSystemWake()` is guaranteed to run before whatever
`TriggerRecordingsUpdate()` queues moments later in that same call --
this is the one place in this entire ordering problem where the addon
can actually get *ahead* of Kodi's own sequencing (a device resuming
from suspend), not just avoid making an existing race worse. Fixed by
adding a timer trigger at all four sites, ordered before the recordings
trigger wherever both are present.


**Update (2026-09-27, a 44th-pass audit): a permanently empty API key
never self-healed, silently failing every recording (completed and
in-progress) for the whole addon session, found via a project-wide
review, confirmed against Dispatcharr's own real current upstream
source, not itself independently reproduced.** The constructor only
generates an API key once, and only inside an `else if` gated on that
same construction's own initial `EnsureAuthenticated()` call having
already succeeded -- if that first login attempt fails (Dispatcharr not
yet reachable at Kodi startup, the same startup timing this project has
now found real bugs around three passes running), the key generation
branch is skipped entirely, and nothing else in this addon ever
revisits "no key yet" afterward: every other `GenerateApiKey()` call
site only fires reactively, on a `401` response. Confirmed against
Dispatcharr's own real current upstream source that an *empty* key
never reaches that self-heal at all: `RecordingViewSet`'s `file`/`hls`
actions use `AllowAny` at the DRF permission-class level specifically
so `_user_can_play_recording()` (its own actual authorization gate) gets
to run for an unauthenticated request too -- and that function returns
**403**, not 401, for a request with no credentials (`user.is_authenticated`
false). Only a *bad*, non-empty key gets a 401
(`ApiKeyAuthentication.authenticate()`'s own `AuthenticationFailed` for
an unrecognized key, which DRF's exception handling maps to 401 since
that authenticator implements `authenticate_header()`). Fixed by
generating a key proactively, right at the top of `OpenRecordedStream()`,
whenever one isn't already present -- reusing the same
`keyBefore`/`PersistApiKeyIfChanged()` wrapper this function already
applies for the reactive-regeneration case, so a freshly generated key
here gets persisted the same way.

**Update (2026-09-27, a 44th-pass audit): a stale API key survived a
Dispatcharr host/username change untouched, letting recording playback
keep authorizing as the *previous* account after switching to a new
one -- found via a project-wide review, confirmed against Dispatcharr's
own real current upstream source, not itself independently
reproduced.** A `host`/`username` settings change already returns
`ADDON_STATUS_NEED_RESTART` (this addon's connection settings aren't
something to change on a live instance), but the stored `api_key`
setting itself was never cleared -- so the fresh instance created by
that restart still saw a (still server-side-valid, if the old account
still exists) key and skipped generating a new one. Since Dispatcharr's
own `ApiKeyAuthentication` resolves a request's user purely from the
key itself (`User.objects.get(api_key=raw_key)`), independent of
whichever account this addon's own JWT login now separately
authenticates as for every other API call, recording playback kept
authorizing as the *old* account -- switching from an admin/manage
account to a lower-privileged view-only one could still let recording
playback reach content outside the new account's own channel scope
(`recordings_queryset_for_user()`, `apps/channels/dvr_access.py`).
Fixed by clearing the stored `api_key` setting whenever `host` or
`username` actually changes (not `port`/`use_https`/`verify_ssl`/
`timeout`/`password` -- none of those changes *who* the key
authenticates as), so the fresh instance the resulting restart creates
generates one scoped to whichever account is now configured.

**Update (2026-09-27, a 45th-pass audit): the fix above caused a real,
confirmed GUI-thread deadlock, caught and corrected one pass later --
confirmed against Kodi's own real current source, not itself
independently reproduced.** Calling `kodi::addon::SetSettingString(
"api_key", "")` directly inside `OnAddonSettingChanged()`'s own
`host`/`username` branch, as the fix above did, is unsafe:
`CAddonDispatcharr::SetSetting()` (`addon.cpp`) holds `m_instancesMutex`
for its entire call into `OnAddonSettingChanged()`, and
`SetSettingString()` unconditionally calls `CAddonDll::SaveSettings()`,
which -- once the settings dialog that triggered the change has already
closed (a modal `dialog->Open()` that returns only after `Close()`,
`CGUIDialogAddonSettings::ShowForSingleInstance()`) -- re-enters
`TransferSettings()` and re-delivers *every* setting again, including
`host`/`username` themselves, back into `CAddonDispatcharr::SetSetting()`
on the very same call stack. That tries to re-lock the still-held
`m_instancesMutex` on the same thread: undefined behavior for a plain
`std::mutex`, and a real, permanent hang in practice on every mainstream
implementation (glibc's default `PTHREAD_MUTEX_NORMAL` explicitly
documents self-relock as deadlocking). Kodi's GUI thread hung permanently
on any real `host`/`username` change made through the settings dialog --
the user had to kill Kodi, though the new host and cleared key were
already written to disk by that point (`CAddon::SaveSettings()` writes
before `TransferSettings()` runs), so the *next* start worked, masking
how the previous one had actually ended.

Fixed by never calling any `SetSetting*()` variant from inside
`OnAddonSettingChanged()` at all. Two new hidden settings,
`api_key_host`/`api_key_username` (`resources/settings.xml`, a real
`<visible>false</visible>` child element -- see this update's own
follow-up note below), record which account the stored `api_key`
actually belongs to -- written by `PersistApiKeyIfChanged()` alongside
`api_key` itself, from its own already-safe (non-reentrant) call sites.
The constructor -- an ordinary, non-reentrant call path -- compares them
against the current `host`/`username` and treats a mismatch the same as
"no key yet," achieving the same result the reactive clear was trying
to, safely: the fresh instance a `host`/`username` change's own
`ADDON_STATUS_NEED_RESTART` already creates generates a new key scoped
to whichever account is now configured, the first time it constructs.

**Update (2026-09-27, a 46th-pass audit): the two new settings above
weren't actually hidden, a real, confirmed mistake in this same fix,
found via a project-wide review and confirmed against Kodi's own real
current source.** The original version used `visible="false"` as an
XML *attribute* on `<setting>` itself. `resources/settings.xml`'s own
`<settings version="1">` root routes through
`CAddonSettings::InitializeDefinitionsFromXml()` (`AddonSettings.cpp`),
whose `ISetting::Deserialize()` (`settings/lib/ISetting.cpp`) reads
visibility only via `XMLUtils::GetBoolean(node, "visible", ...)` --
`FirstChild("visible")`, a child *element*, never an attribute; the
attribute form is only ever read by the legacy `version="0"` definitions
parser this file doesn't use. Both settings rendered as two extra,
fully visible, editable "API key" rows in the Advanced category (reusing
label 30033, the real `api_key` field's own label), showing the stored
host/username in plain text -- the opposite of the intent. Fixed by
using a real `<visible>false</visible>` child element instead.

**Update (2026-09-27, a 46th-pass audit): two more real, confirmed gaps
in the same 45th-pass fix, found via a project-wide review, confirmed
against Kodi's own real current source and Dispatcharr's own real
current upstream source, not itself independently reproduced.**

First, the account-mismatch comparison only ever runs inside
`EnsureAuthenticated()`'s success branch in the constructor, and only
actually resolves a detected mismatch if the follow-up
`GenerateApiKey()` call also succeeds right then. If the initial login
fails (the same startup-timing class of gap the 43rd/44th-pass fixes
already addressed for channels/EPG and the plain-empty-key case) or that
regeneration attempt itself fails, the mismatch is silently never
resolved, and -- unlike an empty key -- nothing else in this addon ever
revisits a merely *wrong-account* (non-empty) key for the rest of the
session; `OpenRecordedStream()`'s own pass-44 self-heal only fires on
`keyBefore.empty()`. Fixed with a new `m_apiKeyOwnershipVerified` flag,
checked (and retried) alongside the existing empty-key case in
`OpenRecordedStream()` on every open until it actually succeeds.

Second, `PersistApiKeyIfChanged()` stamped `api_key_host`/
`api_key_username` from `m_lastAppliedConfig.host`/`.username` --
written with no lock by `OnAddonSettingChanged()` on the settings/GUI
thread, a genuine data race against this read from whatever thread
calls `OpenRecordedStream()`/`ReadRecordedStream()`/etc. Worse than the
race itself: `m_lastAppliedConfig.host`/`.username` reflect the *latest
delivered* setting, not necessarily what `m_client` is still actually
using -- `CAddonStatusHandler::Process()` (`AddonStatusHandler.cpp`)
shows a *blocking* OK dialog before actually restarting the addon on
`ADDON_STATUS_NEED_RESTART`, and this old, not-yet-destroyed instance
keeps running (and can still self-heal/regenerate the key mid-playback)
for however long the user takes to dismiss it. Stamping the *new*,
not-yet-applied host/username against a key regeneration that actually
happened against the *old* connection made the next instance's own
owner-check wrongly treat a foreign-account key as already matching,
keeping it permanently. Fixed with two new `const` members,
`m_apiKeyOwnerHost`/`m_apiKeyOwnerUsername`, snapshotting what `m_client`
was actually constructed with once, at construction -- safe to read from
any thread with no lock at all, and immune to the NEED_RESTART-dialog
window since they never change for this instance's whole lifetime.

A minor, related gap fixed the same pass: `OpenRecordedStream()`'s own
proactive key generation ran before the actual stream-open attempt, but
a freshly generated key was only persisted if that open then succeeded
-- if it failed for an unrelated reason (the recording genuinely
doesn't exist, a transient network issue), the new key stayed live
server-side but unpersisted, so the next restart saw the *old*,
already-invalidated key and needlessly regenerated again. Fixed by
persisting on that early-failure path too.

**Update (2026-09-27, a 47th-pass audit): the `m_apiKeyOwnershipVerified`
flag the 46th-pass fix above added had its own regression, caught the
very next pass, confirmed by code trace, not itself independently
reproduced.** The comparison against `m_apiKeyOwnerHost`/
`m_apiKeyOwnerUsername` (which only reads local settings, no network
call) was computed and stored inside the constructor's
`EnsureAuthenticated()` success branch -- so if the *initial* login
attempt failed (the same startup-timing class of gap already fixed
elsewhere: channels/EPG, the empty-API-key case), `m_apiKeyOwnershipVerified`
stayed at its default `false` even when the stored key genuinely already
belonged to this account, since the comparison that would have proven
that was never even run. The very first `OpenRecordedStream()` call
then saw `!m_apiKeyOwnershipVerified` and rotated a perfectly valid key
-- silently invalidating it for every other install/tool/script already
using that same account's key, exactly the disruption this whole
owner-tracking mechanism exists to avoid causing unnecessarily. Fixed
by moving the comparison (and the flag it sets) to run unconditionally,
before the `EnsureAuthenticated()` check, keeping only the actual
regeneration attempt itself gated on login having succeeded.

See `docs/OPEN_ITEMS.md`'s own entry on a related, bigger, deliberately
deferred API-key issue: two installs sharing one Dispatcharr account
regenerating each other's key several times a second during
*simultaneous active playback*, not just once per restart.

**Update (2026-09-27, a 47th-pass audit): the realtime-update WebSocket
thread had the same "live settings vs. construction-time snapshot"
mistake as the API-key owner tracking above, found via a project-wide
review, confirmed against Kodi's own real current source, not itself
independently reproduced.** `StartRealtimeUpdateThread()`'s own
reconnect loop called `PVRDispatcharr::LoadConfigFromSettings()` fresh
on every iteration -- a *live* settings read -- for the host/port/
`use_https`/`verify_ssl`/`timeoutSeconds` it connects with, while using
`m_client`'s own (construction-time) JWT for the connection itself.
During the same NEED_RESTART-dialog window described above (this old,
not-yet-destroyed instance keeps running for however long the user
takes to dismiss Kodi's blocking OK dialog), a reconnect landing in that
window sent the *previous* server's own JWT to whatever *new* host/port
had just been typed in -- a real credential-disclosure risk if that new
host happens to accept it (e.g. a cloned/staging instance sharing the
same Django `SECRET_KEY`), and at best a pointless connection attempt
otherwise. Fixed with a new `DispatcharrClient::GetConnectionSettings()`
accessor exposing `m_client`'s own construction-time connection fields
(safe to read from any thread with no locking, the same reasoning as
`m_apiKeyOwnerHost`/`m_apiKeyOwnerUsername`, since none of them ever
change after construction -- a change to any of them always returns
`ADDON_STATUS_NEED_RESTART` instead) -- the reconnect loop now reads
from that instead of taking a fresh, live settings snapshot.

**Update (2026-09-27, a 48th-pass audit): a failed rename/delete could
leave Kodi's own UI wrong or show a spurious error, found via a
project-wide review, confirmed against Kodi's own real current SDK
source, not itself independently reproduced.**

`CPVRRecording::Rename()` (`PVRRecording.cpp`) sets its own `m_strTitle`
to the new name *before* ever calling into this addon, unconditionally
-- so a failed rename here still left Kodi displaying the new,
never-actually-applied title, with nothing correcting it:
`AsyncRecordingAction::Run()` (`PVRGUIActionsRecordings.cpp`), the
caller, only triggers a recordings refresh on *success*. Fixed by also
calling `InvalidateAndTriggerRecordingUpdate()` on `RenameRecording()`'s
own failure path, so Kodi's wrongly-optimistic in-memory title gets
corrected back to the real server-side value promptly instead of
staying wrong until some unrelated refresh happens to occur.

Separately, `DeleteRecording()` treated a 404 the same as any other
failure -- but a 404 here means the recording is already gone, exactly
the end state this call was trying to reach (whether it was already
deleted by another Kodi install sharing this account, or Dispatcharr's
own automatic cleanup). Confirmed against Kodi's own real current SDK
source that `CPVRGUIActionsRecordings::DeleteRecording()` surfaces any
non-`PVR_ERROR_NO_ERROR` return here as a "PVR backend error" dialog to
the user, for something that's already true. Fixed by treating a 404
specifically as success.

**Update (2026-09-27, a 48th-pass audit): the API-key owner stamp
ignored port and scheme, found via a project-wide review, confirmed
against Kodi's own real current source, not itself independently
reproduced.** The account-mismatch comparison added a couple of passes
ago compared only host and username -- so switching to a *different*
Dispatcharr instance reachable on the same host but a different port or
scheme (two containers on `127.0.0.1` at different ports, a staging
instance) with the same username kept treating the old instance's key
as already belonging to the new one. Worse than the plain host/username
case this whole mechanism already guards against: an API key never
expires on its own, so `OpenRecordedStream()` skipped regeneration
entirely (ownership already marked verified) and silently sent the
*previous* server's key to the *new* one in `X-API-Key`, only
self-healing once that server's own 401 triggered a regeneration. Fixed
by folding port and scheme into the same stored identity
(`ComputeApiKeyOwnerServer()`, `PVRDispatcharr.cpp`'s anonymous
namespace) -- the setting id `api_key_host` is unchanged, only what it
stores changed, so no new hidden setting was needed. This is still the
same minimal, stamp-based approach, not the bigger "read the server's
own current key via `GET /api/accounts/api-keys/`" redesign the related
multi-install entry above already defers -- that redesign would also
close this gap more completely (an authoritative check rather than a
locally-stored guess), but wasn't attempted blind this pass either.

**Update (2026-09-27, a 49th-pass audit): the whole owner-tracking
mechanism above (`api_key_host`/`api_key_username`, added the two
passes prior) had no migration path for an install already running a
released version that predates it, found via a project-wide review,
confirmed against `master`'s own current `addon.xml.in` (`0.11.0`), not
itself independently reproduced.** Neither setting existed before this
same, 45th, pass, so an install upgrading from `0.11.0` or earlier
already has a perfectly valid `api_key` but both settings read back as
their empty `GetSettingString()` default -- indistinguishable from a
genuine host/username mismatch by the plain comparison this mechanism
uses. Left as-is, every existing user's key would have been silently
rotated once on the very first post-upgrade start, invalidating it for
any other install/tool/script already using that same account's key --
exactly the disruption this whole mechanism exists to avoid causing
unnecessarily. Fixed by treating "both stamps empty and `HasApiKey()`"
as a one-time migration case in the constructor: trust the existing key
as already belonging to the current account, and stamp the real
host/username values right then (safe from the constructor, the same
reasoning `PersistApiKeyIfChanged()`'s own calls already rely on) so a
*later* genuine host/username change is still caught as a real mismatch
by the next instance's own constructor, rather than this bypass
silently applying forever. The one gap this leaves: a user who already
changed host/username on the pre-migration version before upgrading
won't get the wrong-account protection for that specific change -- but
that protection didn't exist there either, so nothing regresses.

**Update (2026-09-27, a 49th-pass audit): `StopRecording()`/
`DeleteRecurringRule()` had the same 404-tolerance gap `DeleteRecording()`
was fixed for above, found via a project-wide review, not itself
independently reproduced.** Same reasoning: a 404 from either endpoint
means the target is already gone (already stopped/deleted by another
install sharing this account, or Dispatcharr's own automatic cleanup),
exactly the end state the call was trying to reach, not a real failure.
Fixed the same way, treating a 404 specifically as success in both.
`DeleteSeriesRule()` was independently checked and does NOT need this
fix -- confirmed against Dispatcharr's own real current upstream source
that `SeriesRulesAPIView.delete()` always returns HTTP 200 regardless of
whether anything actually matched.

**Update (2026-09-27, a 49th-pass audit): `UpdateTimer()`/`DeleteTimer()`
only triggered a Kodi timer/recording refresh on their own success path,
found via a project-wide review, confirmed against Kodi's own real
current SDK source, not itself independently reproduced.** A failed
update/delete/stop can still have partially changed server-side state
before the failure occurred (or reflect state that changed for an
unrelated reason, e.g. another install), but with no trigger on the
failure path, Kodi's own cached copy of that timer/recording stayed
wrong until some unrelated refresh happened to occur -- the same class
of gap already fixed for `RenameRecording()`'s own failure path above,
just for `UpdateTimer()`/`DeleteTimer()` instead. Fixed by moving both
trigger calls to run unconditionally, before each function's own
`if (!ok)` failure check, rather than only inside a success branch.

## A user Stop stranded an in-progress recording's final segment

*2026-09-30. Closes the open item "User Stop plus status-before-playlist reorder can
strand a recording's final segment".*

`RefreshInProgressRecordingManifest()` decides a recording is finished from its status
(`ResolveInProgressFinished()`), checked before the playlist is fetched. The 18th-pass
source reading found that `RecordingViewSet.stop()` writes `status = "stopped"`
synchronously and tears the stream down in a background thread afterwards, so the
playlist could still be one segment short when the status had already flipped. **Measured
live** against the real instance (record, then Stop while polling the playlist every
~30ms): after the status changed, the playlist stayed at 9 segments with no
`#EXT-X-ENDLIST` for about three seconds, then showed 10 segments and the tag in the same
read, and about half a second later the `hls` endpoint started answering 302.

The addon side reproduced it: a viewer at the live edge saw `finished=1` at its final byte count and, 2.6 seconds later, a new segment probed -- after the reader had
already been given EOF, so it never played. (How much is lost depends on how much of the
last segment ffmpeg had written; this was a partial one.)

`GateFinishedOnEndList()` (`RecordingVisibility.h`) now sits between the status decision
and the stored `finished`: while the status says finished but the playlist
(`M3u8HasEndList()`) doesn't yet, `finished` stays false and the wait is timed; it turns true
when the tag appears, or after a 15-second grace so a recording that died without ever writing
it still ends. The path where Dispatcharr has already removed the HLS directory (a redirect or
404 on the playlist) is untouched: the tag is gone with the directory and that path ends the
stream by its own rule. Re-run with the same scenario: `finished=0` on the cycle that saw the
status flip, then `finished=1` on the cycle where the final segment (a small one this time) and the
tag arrived together -- the segment is now merged before EOF.

## The in-progress manifest refresh no longer re-checks the API key

*2026-09-30.* Every refresh used to GET the playlist, then GET it again (`Range: 0-0`) to
check the API key, then look the recording up. `FetchRawInProgressPlaylist()` already sends
the same key to the same URL and regenerates it on a 401, so a successful fetch had already
proven the key, and the second GET was pure overhead. Removed (with its `IsApiKeyValidFor()`
helper). Measured through a counting proxy with a viewer at the live edge of a recording:
130 playlist requests in 40 seconds before, 65 after, the recording lookups unchanged at 65.

## Two live checks of the in-progress read path (2026-10-01)

*Both open items were `Needs a live check`; neither is fixed here, see `docs/OPEN_ITEMS.md` for the proposals.*

**How the probes were put behind a proxy.** `index.m3u8` for an in-progress recording lists each segment as an absolute URL at the
server's own host and port, so a forwarder in front of the addon's configured host sees the playlist and API calls but never the
segment HEAD/GET requests, which go straight to the server. An HTTP-aware proxy that rewrites those URLs in the playlist back
through itself sees all of it and can break one segment (`Content-Length: 0` on its HEAD, or a 404) while passing the rest.

**An unsizeable segment stalls the stream, floods the server and pins the recording open.** With segment 20 of a six-minute recording
answering its HEAD with a zero length: playback advanced to 1:16 (the first 20 segments) and stopped for good. The addon then sent about
14 HEADs a second -- the unmerged tail is probed again on every refresh because `CountLeadingProbedSegments()` throws away the
successful sizes behind the first failure -- each refresh took 3-5 s, and every blocking `Read()` ran its full catch-up budget
(49 attempts, 200-255 s). Kodi's Stop only completed when the read it was waiting on returned (it took 4 min 24 s).
Meanwhile the recording's playlist had its `#EXT-X-ENDLIST` and 92 segments, yet its status stayed `recording` until 25 s after
the stream closed: Dispatcharr finalizes a finished recording only once its viewer key (refreshed by any `.ts` request,
including these HEADs) lapses, so the stuck probing held it open indefinitely. Kodi's displayed position also jumped from 1:16 to 4:36
when the first blocked read returned empty, which is Kodi's own bookkeeping and no data.

**A forward seek at the tail can land a little behind the reader.** Sitting at the tail of a five-minute in-progress recording,
Kodi's forward steps produced `SeekInProgressRecordingStream()` calls that clamped to `tailTarget` below the current position by 262,144 bytes
(four times), 524,288 and 1,310,720 -- under a quarter of a multi-MB segment each, the structural ceiling given the one-segment
margin. Kodi's displayed time never went backward; a +30 s step with less than 30 s left reads as "nothing happened" because Kodi asks for
the end of the stream, the clamp puts the reader a quarter-megabyte behind where it was and Kodi's follow-up end probing settles on the
same time. Reaching the tail needs only the reader to wait there, not an accelerated catch-up.

**Both fixed and re-verified live (2026-10-01).**

`UnprobeableSegment.h` (`ShouldProbeOnlyLeadingSegment()`, `UpdateUnprobeableSegmentTracker()`, `CountMergeableSegments()`): once the leading unmerged
segment has failed on a refresh it is probed alone, since nothing behind it can be merged and the old per-refresh probing of the whole tail only
fed the flood; after five consecutive failed refreshes spanning 30 seconds (a segment briefly absent while the server writes it never gets
there, and a down server never reaches the probe at all) it is merged as a zero-byte placeholder with one logged warning. A placeholder
keeps every later byte and time offset exact, and no stream position can fall inside it, so it cannot reintroduce the duplicated-chunk bug the
leading-merge rule exists for; the cost is a few seconds of missing picture where the segment was. Re-running the same fault (segment 20 of a
four-minute recording answering its HEAD with a zero length): 47 probes of that one segment in the 30 s (against ~14 a second over the
whole tail), the warning at 30 s, playback through to the end of the recording, and `completed` status seconds after the player closed. With the
break placed ahead of a viewer waiting at the live edge: a 28 s stall, then playback resumed.

`ClampSeekToTail()` (`LiveEdgeMargin.h`) replaces the plain `newPos > tailTarget` clamp in both seek paths: a target past the tail target lands on it, or on the
current position if the reader is already past it, or stays on the requested position if that is behind the reader. At the tail of a young
in-progress recording the same Kodi forward steps now leave every clamped seek with delta 0 (ten of ten; before, 262,144 to
1,310,720 bytes backward), the seek-to-live from far behind still lands on the tail target, and Kodi's own short backward seeks inside the margin land
where asked. Live timeshift's three-segment margin shares the helper and was not exercised.

## Series-rule edits, delete safety, guide links and segment URLs (2026-10-02)

*Each of these closes an entry in `docs/OPEN_ITEMS.md`; what was and was not exercised live is stated there.*

**Editing a series rule's match pattern replaces the rule.** A changed title or channel-derived `tvg_id` is a new identity to Dispatcharr's upsert and used to leave the old rule and its future recordings in place. `UpdateTimer()` now creates the new rule first and deletes the old by `(title, tvg_id, source)` only after that succeeded (`ShouldReplaceSeriesRuleOnEdit()`, `TimerIdentity.h`). A source-only difference is deliberately not a replacement: the delete omits the source when the rule is unpinned and would then match every source's rule with that title and `tvg_id`, including one the same save upgraded in place. Dispatcharr does not evaluate a rule at create time, so the delete cannot take recordings the new rule has already materialized.

**A failed lookup no longer decides a delete.** `DeleteTimer()` on a one-time recording used to fall through to the destructive delete when the server lookup failed and Kodi's stale `forceDelete` was false. Both guesses are wrong in a real way -- Delete destroys a running recording's file, and Stop (answered 200 for everything but completed/interrupted/failed) leaves a terminal-status row with no file for one that has not started -- so `DecideDeleteTimerAction()` (`RecordingVisibility.h`) treats a 404 as already gone, keeps Stop when Kodi itself says it is recording, and otherwise refuses and tells the user to try again.

**A recording's guide link outlives the guide.** `RecordingEpgLinks.h` remembers, per recording id, the start time of the EPG programme it was matched to (the broadcast id is `ComputeBroadcastId(channel, programme start)`, so that is all that is needed), because the cached guide drops an ended programme at its next refresh. `ResolveRecordingBroadcastId()` falls back to it, and it is persisted as `recording_epg_links.json` in the addon's user directory. It is validated against the recording's channel and start time, pruned against a successfully loaded recording list, capped, and an unreadable file is ignored and rebuilt.

**Segment URLs follow the configured address.** An in-progress recording's playlist lists absolute segment URLs built from the request Dispatcharr received; behind a proxy that forwards neither the port nor `X-Forwarded-*` that is the internal port. `RebaseRecordingSegmentUrl()` (`M3u8SegmentParser.h`) points the recording's own `/api/channels/recordings/<id>/hls/` URLs at the configured address and leaves others alone, so a playlist that names an address of the server's own ends up at the configured one. A segment URL naming a *different* host is left alone and, since 2026-10-04, is fetched without the API key -- see `docs/CLOSED_ITEMS.md`'s "The API key was attached to a segment URL on any host". Confirmed live with a proxy that leaves the playlist naming the server's own address: every segment request still went through the configured address.

**A view-only account is not offered the DVR actions.** See `DvrAccess.h`: `GetCapabilities()` advertises timers, recording delete and rename only when the account can manage the DVR (admin, or `custom_properties.dvr_access` of `manage`). Confirmed live with a disposable Standard user: Kodi reported `supportstimers: false`, and recordings were still listed and playable.

## A server that ignores Range ends the recording instead of playing wrong bytes (2026-10-02)

`ReadRecordingStream()` and the live-timeshift segment read both accepted a plain 200 to a ranged GET at a non-zero offset and advanced their position as if the bytes started there -- but a 200 means the server (or a proxy that drops `Range`) sent the file from byte 0, so the wrong data was silently spliced into playback. `ServerIgnoredRangeRequest()` (`RecordingHttpUtil.h`) recognises it. Neither obvious response is right: a retry is a storm (Kodi retries a `-1` read near-immediately), and skipping to the offset downloads everything before it on every read. So a recording ends (EOF, once, with a log line and a notification, later reads returning EOF with no request) and a live-timeshift stream is marked fatal. Dispatcharr's own file endpoint and the plugin's file server both answer 206 correctly (checked live), so this only ever fires behind a Range-dropping proxy. Confirmed live with such a proxy.
