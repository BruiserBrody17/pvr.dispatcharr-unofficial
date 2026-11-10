*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**15 open, 399 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One from the 2026-10-25 stream-switch re-test on Dispatcharr 0.33.0: a codec-changing switch, documented and deferred.
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One from the 2026-11-09 change that buffers every audio track: the live checks it has not had yet.
- Nine, all LOW, from the 2026-11-10 bug-hunting sweep, made on `Piers` and checked against `Omega`'s code (six
  read-only reviews by area, the sixth over the seams between the addon, the plugins, the test fakes and the docs; each
  finding checked against the code before it was filed): one `timeshift_buffer` regression that is in no release yet,
  two in the addon, five in the release gate and the CI path filter and one in the glue harness's fake server. Nine of
  the sweep's eleven findings hold here; the other two are `Piers` only (an in-progress recording's segment cache with
  two streams of it open, which `Omega` never has, and a table in `BRANCHES.md`, which `Omega` does not carry).

Nothing is left from the 2026-10-18 read-only audit, made on `Piers` and checked against `Omega`'s code (all 21 findings
held here), from the 2026-10-19 live check of four audit fixes on `Piers`, from the 2026-10-23 sweep, made on `Piers`
and checked against `Omega`'s code (all 17 findings held here, the last fixed on 2026-11-04), from the 2026-10-24 sweep (one reviewer
over the whole project, seven findings), or from the 2026-10-31 sweep, made on `Piers` and checked against `Omega`'s
code (all 11 findings held here; nine fixed, two closed without a change, the last on 2026-11-04 by keeping a
recording's 20 s retry budget).

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 2 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 2 |
| Open: Known gaps, deliberately deferred | 3 |
| Open: Tooling (tools/) | 5 |
| Closed (in CLOSED_ITEMS.md): Fixed | 347 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 42 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

#### `timeshift_buffer` 0.8.18's audio-track mapping has only been seen on generated streams

**Opened 2026-11-10, after the change that buffers every usable audio track** (`docs/TIMESHIFT.md`, "Every audio track,
not just the first (0.8.18)"). The audio switch (live, after a pause, after a rewind), DVB subtitle, teletext, audio-only and
two-rendition channels were checked on Kodi 21 and Kodi 22 against a real Dispatcharr, but every source was a generated
stream served from a test machine. Still to see: (1) a real broadcast channel with several audio tracks (ideally one
mixing codecs, such as MPEG audio beside AC-3 or E-AC-3, or carrying an audio-description track), switched in Kodi the same
way; (2) the probe's two non-happy paths on a real server: a probe that times out is retried on the next start, and an
ffmpeg that accepts neither spelling falls back to the 0.8.17 command line with `-sn`. Both are covered only by the unit
and real-ffmpeg tests so far. Close this with how each was confirmed.

### Fix known, not yet done

#### A recording or recurring-rule id outside 1 to 0x1FFFFFFF is taken for another kind of timer

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, the decoding reproduced with a small program, the delete and update that would
follow read in the code, not run.** `GetTimers()` gives a one-off recording's timer the server's recording id as its
client index unchanged, a recurring rule `rule.id | kRecurringRuleIndexFlag` (bit 29) and a series rule a hashed index
with bit 30 set; `DecodeTimerClientIndex()` (`src/TimerIdentity.h`) tests bit 30, then bit 29. The comment beside the
recurring case says its namespace bit means it "can never collide", which holds only while every server id stays below
0x20000000 (about 537 million). A recording whose id is 0x20000005 decodes as recurring rule 5, so `DeleteTimer()` on it
would call `DeleteRecurringRule()` for rule 5, which removes that rule and its future recordings, and `UpdateTimer()`
would PATCH rule 5; an id with bit 30 set, or a negative one, decodes as a series rule; and an id of 0 is the value
Kodi uses for "no index". Dispatcharr's ids are auto-increment integers from 1, so reaching this takes a server that
has handed out more than half a billion of them, or a malformed answer. Fix: leave out, and log once, any recording or
recurring rule whose id is 0 or less or 0x20000000 or more, in `GetTimers()` and `GetTimersAmount()` alike (the two
counts must agree), with the range check as a free function tested in `tests/test_timer_identity.cpp`; optionally have
delete and update confirm that a decoded rule id is one in `m_cachedRecurringRules` before acting on it.

#### `HasActiveOrImminentOccurrence()` overflows a 32-bit `time_t` for a start far in the past

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced with a `-m32` build and under UBSan.** `src/ManagedRecurringRule.cpp`
tests `rec.startTime - now < safetyMarginSeconds` for an upcoming recording whose end is still ahead. A start more than
2^31 seconds before now, which is what a date clamped to `kEarliestRepresentableTime` is, overflows that subtraction
where `time_t` is 32 bits: UBSan reports the signed overflow, and the `-m32` build answers false where the 64-bit build
answers true. Its callers are the two guards that put off a rule PATCH while an occurrence is recording or about to
(`ShouldRenewRecurringRule()` and `PlanRecurringRuleAdoption()`), so on such a build that recording no longer holds the
PATCH back. Narrow: a 32-bit `time_t`, and a server answer with an upcoming recording whose start parsed to the floor.
The same class as `CLOSED_ITEMS.md`'s "Windows (and 32-bit) parse a pre-1970 (pre-1901) date to a floor that later
subtractions overflow", which fixed the other subtractions and missed this one. Fix: compare in 64 bits, and add a case
with a floor-valued start to `tests/test_managed_recurring_rule.cpp` so that `unit-tests-32bit` (which runs under
UBSan) pins it.

### Architectural / concurrency

None open.

### Design decision needed

#### `timeshift_buffer` 0.8.18 never becomes ready on a channel that declares a video stream it does not send

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced with a generated stream on ffmpeg 6.1.1, not seen on a real channel.** A
regression of the change that buffers every audio track (`docs/TIMESHIFT.md`, "Every audio track, not just the first
(0.8.18)"), which is in no release yet. With a usable audio map, `_build_ffmpeg_command()` maps the video explicitly as
`-map 0:v:0?`. The `?` forgives only a stream that is not declared at all. One the programme map declares but never
sends (a radio service that lists a video PID, or a dead first video PID beside a live second one) is mapped, and the
HLS muxer then waits for a video keyframe that never comes: no segment is cut, `live.m3u8` never appears, ffmpeg stays
alive and the plugin never reports the buffer ready. 0.8.17's command line (no `-map`, so ffmpeg's own pick of streams)
and 0.8.18's own `-sn` fallback both buffer the same input: run side by side, the 0.8.18 command had no playlist and no
segment after 15 s, the other two a playlist within 9 s and seven segments by 15 s. `ffmpeg.log` shows only "Could not
find codec parameters for stream 0 (Video: h264, none): unspecified size", the same warning a harmless mid-GOP start
prints. Not checked: the ffmpeg 8.1.2 in Dispatcharr 0.33.0's container, and whether Dispatcharr's proxy passes such a
programme map through at all. Options: (a) restart ffmpeg once on the fallback command line (no audio map) when it is
alive, has written no `live.m3u8` after about 12 to 15 s and its log carries that warning, at the price of one audio
track on that channel; or (b) accept it and document it. Settle before the next plugin release: 0.8.17 buffers such a
channel and 0.8.18 as it stands would not.

### Release, CI and manual testing

#### The glue harness's fake Dispatcharr keys series rules more loosely than the real server

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading.** Dispatcharr keys a series rule by (`tvg_id`, `title`,
`epg_source_id`) and its delete takes all three as query parameters (the series-rule delete row of the endpoint table in
`docs/API_NOTES.md`; `BuildSeriesRuleDeleteQuery()`). `tests/glue/fake_dispatcharr.py` replaces a rule on (`title`, `tvg_id`) and deletes
every rule with the given title, whatever else the request says. A regression that sent a wrong or missing `tvg_id` or
`epg_source_id` on a delete, which on a real server removes nothing or the wrong rule, would therefore still pass the
harness's timer scenarios. Fix: have the fake match on all three fields, seed two rules that share a title and assert
that deleting one leaves the other.

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

#### Server-side mode freezes the picture after a stream switch that changes codec (and Kodi 21 does in every mode)

**Opened 2026-10-24 from the Dispatcharr 0.33.0 stream-switch re-test; checked live 2026-10-25 and turned into a design
question.** Three disposable channels on the lab, each with a synthetic first stream that stops sending 20 s after it
connects and a synthetic second stream (no provider involved): 720p H.264 then 1080p HEVC, the reverse, and H.264 both
times as the control. Dispatcharr switched at about 45 s every time. The control played on in every mode. With a codec
change, watched for 120 s with screenshots compared at 50, 80 and 110 s: on Kodi 22.0-RC1, Off and Local mode switched
decoder (H.264 1280 ↔ HEVC 1920) and played on in both directions; on Kodi 21.3, Off and Local mode froze the picture on
the old stream's last frame while sound and the player time went on (Kodi kept feeding the new stream to the old
decoder, logging `[h264] no frame!` over and over), which is Kodi's demuxer not acting on the PMT's new stream type for
the same PID; and in Server-side mode both Kodi versions froze in both directions, the decoder never changed and the
player time reset to 0 every few seconds from then on (the symptom first seen on 2026-10-24 with a real channel). So
Server-side is the case on the addon's own path: the buffer's ffmpeg (`-c copy -f hls`) keeps writing segments for the
codec it opened with. Options: watch the buffer's ffmpeg for a codec or stream-layout change on its input and restart it
(a fresh segmenter; the addon then has to treat the new segments like a buffer restart), or re-mux each segment's codec
parameters some other way; or accept it and document it (on Kodi 21 nothing on the addon side helps in Off or Local
mode). Real channels change codec on a switch only when their streams come from different sources, so how often this
bites is unknown.

**Deferred 2026-11-04, by the maintainer's decision: documented, not fixed for now.** The user-facing account is
`TROUBLESHOOTING.md`'s "The picture freezes after a channel switches streams, while playback keeps running" (backup
streams in the same video format; Off or Local mode on Kodi 22). Pick up the ffmpeg restart above if users report it on
real channels.

### Tooling (tools/)

#### The release gate skips its release checks for an empty `--expect-release`, and an empty `--blocklist` falls back to another list

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced by running the gate.** `main` in `tools/check_release_zip.py` runs the
release-set and content checks only `if version`, and `find_blocklist` honours its explicit path only `if explicit`, so
an empty string counts as the option not having been given. Seven of a release's eight zips with `--expect-release
9.9.9` fail ("release asset missing"); the same seven with `--expect-release ""` print "7 zip(s) clean". An empty value
is what an unset shell variable expands to (`--expect-release "$VERSION"`), which is how a release would meet it.
`--blocklist ""` likewise goes on to `$PRIVACY_BLOCKLIST` or the home-directory list, the fallback that
`find_blocklist`'s own docstring says a mistyped path must not get. Fix: test `is not None`, and exit 2 on an empty
value for either option and on a version that is not three dot-separated numbers, with a test for each in
`tools/tests/test_check_release_zip.py`.

#### The CI path filter answers "skip" for a changed file whose name git quotes

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced by giving the filter the name as git prints it.** `changed_files` in
`tools/ci_build_needed.py` reads `git diff --name-only --no-renames` line by line. Git prints a path that holds a
non-ASCII byte, a double quote, a backslash or a control character C-quoted, with a leading `"` (`core.quotepath`), so
the patterns in `needs_build` and `needs_plugins`, which are anchored at the start of the path, do not match it, and a
push that changes only such a file skips the compile-and-package jobs. No tracked file has such a name today, so
nothing has been skipped. Fix: `git -c core.quotepath=off diff -z --name-only --no-renames`, split on NUL, with a test
in `tools/tests/test_ci_build_needed.py` that feeds a name git would have quoted.

#### The release gate checks the last of several same-named zip entries, and Kodi reads the first

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, the gate's side reproduced with crafted zips, Kodi's side read in its source (its
zip manager's entry lookup returns the first match), not run.** The gate reads `addon.xml` and the library through
`archive.getinfo`, which answers the last entry of a name, and `_plugin_differences` builds a dict keyed by name, which
keeps the last too. A zip whose first `addon.xml` carries another version and whose first library is another
architecture's passes when correct copies follow them (the reverse order fails, as it should). So does a zip with an
`ADDON.XML` beside its `addon.xml`, which are one file on a case-insensitive file system, and one whose stored entry
name has a blocklist term after a NUL byte: Python's `zipfile` cuts `filename` at the NUL and the name checks read
that. Related: `tools/normalize_zip.py` and `tools/scrub_zip_paths.py` read each entry's data by `info.filename`, so
both write the last copy's data into every entry of a repeated name. The project's own builds never write a repeated
name; what does not hold is the gate's promise that what it checked is what gets installed, for a zip assembled by
hand. Fix: fail on a repeated name (compared without case), on `orig_filename != filename` and on a control character
in a name; have the two rewriting tools read by the `ZipInfo` object, not by name.

#### The release gate does not scan bytes inside an entry's data span that decompression never returns

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced with crafted zips.** Since `CLOSED_ITEMS.md`'s "The release gate does not
scan bytes outside a member's decompressed data" the gate accounts for every byte of the archive by span (local header,
name, extra field and `compress_size` bytes of data for each entry) and scans each entry's decompressed data. Bytes
inside an entry's `compress_size` that the decompressor never returns are in neither: plain text after the end of the
deflate stream, a deflate stream that goes on past `file_size` (`zipfile` stops reading there), and a stored entry
whose `compress_size` is larger than its `file_size`. A blocklist term placed in any of the three passes. As with the
earlier item, no tool the project builds with writes such an entry; the gate is the one place meant not to rely on
that. Fix: inflate each deflated entry with `zlib.decompressobj(-15)` and require the stream to end, with nothing left
over, at exactly `file_size` bytes; require `compress_size == file_size` for a stored one.

#### The release gate passes a zip entry compressed with a method Kodi does not read

**Opened 2026-11-10, from the 2026-11-10 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, the gate's side reproduced with crafted zips, Kodi's side read in its source
(`xbmc/filesystem/ZipFile.cpp` opens only stored and deflated entries), not run.** Python's `zipfile` reads bzip2 and
LZMA entries, so a release zip whose `addon.xml` is compressed with either passes every check, and Kodi would then fail
to install it. No tool the project builds with chooses those methods. Fix: fail any entry whose `compress_type` is not
stored (0) or deflated (8).
