*(part of the pvr.dispatcharr-unofficial notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

# Commercial-break markers (comskip EDL) on the recording seekbar

Requested after a user enabled Dispatcharr's built-in comskip integration
and noticed the resulting `.edl` file next to a recording had no visible
effect in Kodi -- no highlighted commercial-break regions on the seekbar.

Two independent gaps, confirmed against real source rather than assumed,
both needed fixing:

## Gap 1: this addon never implemented Kodi's EDL callback at all

`GetCapabilities()` declared `PVRCapabilities::SetSupportsRecordingEdl(false)`
unconditionally, so Kodi never even asked the addon for commercial-break
data regardless of what existed on disk. Fixed by implementing
`GetRecordingEdl()` and flipping that capability to `true` -- see Gap 2
for why it's safe to declare this unconditionally rather than behind a
setting.

## Gap 2: no HTTP-reachable way to fetch the `.edl` file's content at all

Even with the callback implemented, there was nothing to call: confirmed
against Dispatcharr's own source (not guessed) that `RecordingViewSet`'s
`file` action (`apps/channels/api_views.py`) always serves exactly
`custom_properties["file_path"]`, with no parameter to redirect it at a
sibling file, and there's no generic static route reaching
`/data/recordings/...` either -- `dispatcharr/urls.py` only exposes
Django's own `MEDIA_ROOT`, an unrelated path inside the app tree
(confirmed via `dispatcharr/settings.py`). Plugins can't register their
own URL routes either (confirmed via `apps/plugins/loader.py` -- no
route-registration hook exists), so the only reachable mechanism is the
same one this addon's other companion plugin, `timeshift_buffer`, already
uses: Dispatcharr's generic plugin `run/` API.

Fixed with a new, separate companion plugin,
[`dispatcharr-plugin/recording_edl/`](../dispatcharr-plugin/recording_edl/)
(**not** folded into `timeshift_buffer` -- unrelated concern, no shared
state, no background process or extra port needed, so a second small
plugin was cleaner than growing an already-large one). One action,
`get_edl` (`{"recording_id": <id>}`): looks up the `Recording` row via
Django's own ORM (running inside Dispatcharr's process, so it has real
filesystem access Dispatcharr's REST API doesn't expose), reads
`custom_properties.comskip.edl` (the `.edl` filename, confirmed present
on a real recording once comskip has run) and `custom_properties.file_path`
(used only to find the containing directory -- the `.edl` file lives
alongside the recording), and returns the parsed entries.

**Like every other companion-plugin action, `get_edl` needs a real
Dispatcharr admin account** -- this isn't specific to this plugin or to
EDL data: Dispatcharr's plugin `run/` API (`PluginRunAPIView`) is
admin-only for every plugin's every action, a blanket restriction
confirmed against Dispatcharr's own source and detailed in
[TIMESHIFT.md](TIMESHIFT.md)'s "permission requirement" section.
Unlike `timeshift_buffer`, a non-admin account here fails soft rather
than hard: `GetRecordingEdl()` treats any `get_edl` failure the same as
"plugin not installed" (see below) -- logged at debug level, empty
entries returned, no playback impact -- so the only visible symptom is
recordings simply never showing commercial-break markers.

**comskip's `.edl` format**, confirmed against Dispatcharr's own
`docker/comskip.ini` (`[Output]` section) and `apps/channels/tasks.py`'s
own parsing of the same file: plain text, one entry per line,
whitespace-separated `<start_seconds> <end_seconds> <type>`. Dispatcharr's
ini sets `edl_skip_field=3`, so with that ini every line's type is Kodi's own
`PVR_EDL_TYPE_COMBREAK` (3). The plugin passes the type field through
as-written rather than hardcoding it, and that matters: Dispatcharr's DVR settings let a user supply
their own comskip ini, which takes precedence over the shipped one, and comskip's own default for
`edl_skip_field` is `0` (`PVR_EDL_TYPE_CUT`, a hard cut rather than a skippable break). So 3 is what
the shipped ini produces, not a guarantee; a custom ini can legitimately yield other valid types, and
they reach Kodi unchanged (a value outside 0-3 is coerced to 3 by the addon, never cast blindly).

**Only ever populated in comskip "mark" mode.** Confirmed via
`apps/channels/tasks.py`'s `comskip_process_recording`: the *default*
mode is `"cut"`, which uses the same detected timestamps to physically
remove the commercials via ffmpeg and remux the file in place, then
**deletes the `.edl` file** once done (`os.remove(edl_path)`) --
`custom_properties.comskip` in that mode has no `mode` key at all by the
time a recording shows `status: completed`. Only `"mark"` mode (keep the
full recording, just flag the breaks) leaves the `.edl` file on disk to
be fetched.

**Update (2026-09-26, a 23rd-pass audit): "in cut mode
`custom_properties.comskip.edl` is simply absent" above was wrong,
confirmed against Dispatcharr's own real current upstream source
(cloned into a scratchpad, never committed to this repo -- stronger
than the API shape alone, not the same standard as a live test).** The
cut branch's own `cp["comskip"]` still sets `"edl": os.path.basename(edl_path)`
-- pointing at the exact filename it just deleted -- alongside a
`"segments_kept"` key neither the "mark" branch nor the
no-commercials-skipped branch ever sets. So `_edl_path_for()` originally
still resolved a real-looking path for a cut-mode recording, and
`get_edl` then hit a `FileNotFoundError` on the read (caught, logged at
`logger.warning`, still returning "no entries" either way -- so this was
functionally harmless, just a spurious warning on every single playback
of a cut-mode recording with commercials, and fragile if a same-named
file ever legitimately existed at that path some other way). Fixed:
`_edl_path_for()` now returns `None` outright whenever `comskip`
contains `segments_kept` -- the real distinguishing signal, not a
`mode` key (which "mark" sets but "cut" doesn't, and older Dispatcharr
versions' own "mark" branch might not have set either) -- treating it
the same as "nothing legitimate to fetch" from the start, without ever
attempting the doomed read.

Also confirmed via the same source: EDL data is never populated for an
**in-progress** recording -- comskip only ever runs against a file after
the recording finishes, triggered right at that point
(`comskip_process_recording.delay(recording_id)`), never mid-recording.
`GetRecordingEdl()` doesn't need its own in-progress check as a result;
a still-recording item's `custom_properties.comskip` simply won't exist
yet, and the plugin's normal "missing -> empty entries" path already
covers it.

**Confirmed live end-to-end**, against a real recording with comskip
already run in mark mode (`custom_properties.comskip: {"edl":
"S01E01.edl", "mode": "mark", "commercials": 3}`):
- Called the plugin's `get_edl` action directly first, in isolation, to
  confirm it before going through the addon: returned exactly 3 entries,
  type 3, with real millisecond timestamps matching the recording's own
  `commercials: 3` count.
- Then through the actual addon: `GetRecordingEdl()` fired on
  `Player.Open`, and `kodi.log` showed Kodi's own `CEdl` subsystem
  processing all three breaks --
  `CEdl::ReadPvr - Added break [00:05:00.000 - 00:08:00.000] found in PVR
  item for: ...` (and the other two, matching times exactly), plus
  automatic scene markers Kodi itself adds at each break boundary. This is
  the actual mechanism that drives the seekbar's highlighted regions, so
  this is as close to a direct confirmation as log output gets short of a
  visual screenshot.
- Playback itself unaffected throughout (`canseek: true`, normal
  progression) -- EDL fetch failure (confirmed separately, with the
  plugin not yet installed) also doesn't affect playback: logged at
  `ADDON_LOG_DEBUG` rather than `ADDON_LOG_ERROR`, since not installing
  this optional plugin is an entirely normal configuration, not a
  problem -- and `GetRecordingEdl()` returns `PVR_ERROR_NO_ERROR` with an
  empty list either way rather than surfacing the failure to Kodi.

## Why `SetSupportsRecordingEdl` is unconditional, not behind a setting

Unlike `enable_catchup_ffmpegdirect_seek` (which genuinely changes
behavior and fails outright if misconfigured), a missing or not-installed
`recording_edl` plugin just means every `GetRecordingEdl()` call comes
back empty -- confirmed live, no playback impact, no error surfaced to
the user, just a debug-level log line. There's no failure mode that
justifies making this an opt-in setting the way the server-side timeshift
and in-progress-recording features once were before they were also made
unconditional (see `docs/TIMESHIFT.md`/`docs/RECORDINGS.md`) -- this one
never needed the "still experimental, might not be ready" caveat those
did in the first place.

## Orphaned `.edl`/`.logo.txt` sidecar cleanup

A user pasted a real `/data/recordings` listing showing leftover `.edl`
and `.logo.txt` (comskip-generated) files with no recording left to own
them. Root cause, confirmed against Dispatcharr's own source: deleting a
recording (`RecordingViewSet.destroy()`) only removes
`custom_properties["file_path"]` and, if present, an in-progress HLS
staging directory -- it never removes either comskip sidecar file. Since
`destroy()`'s own empty-folder pruning runs once at delete time, before
removing these sidecars would even be possible, the show/season folder is
left behind too.

Added `scrub_orphaned_sidecars`: finds and removes `.edl`/`.logo.txt`
files with no other file in the same directory sharing their base name
(the only safe, unambiguous signal that the recording they belonged to is
actually gone), then sweeps for any directory left empty -- including
ones that were already empty going in, not just ones this removal
happened to empty out. Confirmed safe against `tasks.py`'s own
`comskip_process_recording`: comskip never runs before `file_path`
exists, and "cut" mode's own replacement of `file_path` is an atomic
`os.replace`, so there's no window where a sidecar could exist while its
recording is merely still being written.

**A real incident during development, not a hypothetical, drove the final
scan-root design.** An early version always included the bare
`/data/recordings` root in its scan on the reasoning that it was
harmless, matching Dispatcharr's own hardcoded `library_root`. Live on a
real install, the empty-directory sweep instead walked into and removed
Dispatcharr's own unrelated top-level entries there -- a `.dvr_*_hls`
staging directory and a `.timeshift` directory, the latter requiring a
Dispatcharr restart to recreate. Fixed with two independent guards, not
one: scan roots are now derived strictly from DVR Settings' four path
templates (never the bare `/data/recordings` root, even for a degenerate
template with no subdirectory of its own), and the directory walk
separately refuses to touch anything dot-prefixed at any depth regardless
of which root it started from -- confirmed live afterward against the
exact same layout that caused the original incident: the genuine orphan
was removed, `.dvr_*_hls` and `.timeshift` both survived.

## `.dvr_*_hls` staging directories: diagnose first, delete only what's provably safe

Deliberately kept out of `scrub_orphaned_sidecars` -- much higher stakes
than a stray sidecar file. `.dvr_<recording_id>_hls` is the per-recording
HLS segment staging directory `tasks.py` creates alongside a recording
while it's being written and converted. Dispatcharr manages its own
lifecycle correctly in the common cases (removes it after a successful
concat, once any active viewer's heartbeat window has passed; deliberately
*keeps* it, logging "Keeping HLS segments for recovery," when a concat
fails and its own MP4-intermediate fallback also fails, since it's then
the only surviving copy of that recording). But `RecordingViewSet.destroy()`'s
own cleanup of a deleted recording's `_hls_dir` runs on a bare
`daemon=True` thread with no persistence or retry -- if Dispatcharr
restarts or crashes in the gap between the DB row being deleted and that
thread finishing, the directory is orphaned permanently with nothing left
to ever reference it again.

Added `list_dvr_hls_staging_dirs` first, as a read-only diagnostic:
classifies every such directory it finds as `active` (a Recording row with
`status: recording` still points here), `preserved_failure` (a Recording
row with `remux_success: false` still points here -- the only surviving
copy), `referenced` (a Recording row exists but neither signal above is
confirmed -- needs manual review), or `orphaned` (no Recording row with
that id exists at all). Only once that was live and reviewed against real
data did `delete_orphaned_dvr_hls_dirs` get added, scoped strictly to the
`orphaned` classification -- deliberately not gated on whether a directory
is empty, since a brand-new recording's own staging directory is created
before ffmpeg writes its first segment, so a genuinely active recording
can legitimately look empty for its first few seconds. Emptiness was
never the safety signal; a real, current Recording row is.

**Follow-up (2026-09-27, a 64th-pass audit, confirmed by direct
reproduction against the plugin's own regex, not reproduced live): the
directory-name-to-recording-id step itself was looser than the name
Dispatcharr actually writes.** `tasks.py` only ever creates
`f".dvr_{recording_id}_hls"` -- plain ASCII digits, no padding -- but
`_hls_staging_dir_recording_id()` used `re.match(r"^\.dvr_(\d+)_hls$")`,
which also accepted Unicode decimal digits (Python's `\d` on a `str`
pattern isn't ASCII-only, and `int()` parses them: `.dvr_٣_hls` read as
recording 3), a zero-padded id (`.dvr_0005_hls` as recording 5), and a
trailing newline (`$` matches just before one). `rglob(".dvr_*_hls")`
finds the first two forms, so a directory Dispatcharr never created,
whose parsed id happened to have no Recording row, landed in `orphaned`
-- the one classification `delete_orphaned_dvr_hls_dirs` removes
outright. Now only the exact canonical form is accepted (an ASCII-only
full match plus a no-leading-zeros round-trip check, the same "match
exactly what this system writes, not merely something shaped like it"
approach `timeshift_buffer`'s own `_is_canonical_uuid()` already takes);
anything else is skipped entirely, never classified at all.

## A malformed `.edl` line could take down the whole result, not just itself

Found via a comparative architecture review (the same pass that also
reviewed `timeshift_buffer`), not a user report -- `_parse_edl()`
guards its `float()`/`int()` parsing of each line's three columns
inside a `try`/`except ValueError`, but the *conversion* of the parsed
start/end seconds into milliseconds (`int(round(start_sec * 1000))`)
happened **outside** that block. `float("nan")` and `float("inf")`
both parse successfully (no `ValueError`), but `round()`/`int()` on
either raises afterward -- `ValueError` for `nan`, `OverflowError` for
`inf` -- uncaught, since it's past the `try`, and with no `try`/`except`
around `_parse_edl()`'s own call site in `run()` either. One bad line
anywhere in the file took down the *entire* `get_edl` result for that
recording, showing zero commercial markers instead of just skipping
the one malformed line and returning whatever other entries were
valid. The type-column conversion (which *is* inside the `try` block)
had a narrower version of the same gap: `int(float("inf"))` raises
`OverflowError`, which the `except ValueError:` clause didn't catch
either.

comskip is the only realistic producer of this file and isn't expected
to ever emit `nan`/`inf` timestamps -- this was a defensive gap, not a
reproduced live failure. Fixed by widening the `except` clause to
`(ValueError, OverflowError)` and adding an explicit `math.isfinite()`
check on the parsed start/end seconds before the millisecond
conversion. Verified with a test reproducing the exact pre-fix crash
(`round(float("nan"))` raising uncaught) and confirming the fixed
parser now skips each malformed variant (`nan`/`inf` in any of the
three columns) while still returning every other valid entry in the
same file.

The rest of this plugin held up well under the same review pass: no
HTTP server of its own (a smaller attack surface than `timeshift_buffer`
entirely), and the destructive actions already carry real,
incident-driven safety scoping from earlier sessions (see the orphaned-
sidecar scan-root section above, and `.dvr_*_hls` classification just
above this one) that a fresh read didn't find anything further to add
to.

## Upstream limitations to know (2026-10-02)

Found by audits against Dispatcharr's own source, not reproduced live, and nothing the plugin or addon can fix;
recorded so they are not rediscovered as plugin bugs.

- **A recording under an absolute path template no longer plays or deletes upstream.** Dispatcharr's
  `RecordingViewSet.file()` and `destroy()` resolve a recording's `file_path` through
  `resolve_safe_local_data_path`, which requires the real path to sit under `/data/recordings`. A recording
  whose absolute DVR path template points outside that tree 404s on playback and cannot be deleted through
  the API. This plugin still finds and scrubs sidecars and staging directories in such libraries (see the
  next section), but the older framing of absolute templates as simply "supported" no longer holds on a
  current Dispatcharr.
- **A failed comskip "cut" leaves its temporary files behind.** `comskip_process_recording()`'s cut mode has no
  cleanup on its exception path, so a failure leaves `segment_NNN.mkv`, `concat_list.txt` and
  `<base>.cut.mkv` in the recording's folder. They waste disk and stop this plugin's empty-folder sweep from
  removing that folder; the names are fixed per folder, so two cut runs in one show's folder could overwrite
  each other's segments. The failed run also leaves `custom_properties.comskip` as `{"status": "error"}` with
  no `edl` key, so a still-valid `.edl` from an earlier successful run is not served afterwards. The plugin
  deliberately does not delete these files: nothing marks them as safe against a cut that is still running.

## The staging-directory scan covers absolute-template libraries (0.2.1)

*2026-10-01.* `list_dvr_hls_staging_dirs` and `delete_orphaned_dvr_hls_dirs` only walked
`/data/recordings`, but Dispatcharr creates the staging directory *beside the recording's final file*
(`os.path.join(os.path.dirname(final_path), f".dvr_{recording_id}_hls")`, `_build_output_paths()` in
`apps/channels/tasks.py`), and an absolute DVR path template puts that file in another library
entirely. An orphaned staging directory there was never listed or cleaned, while the sidecar scrub in
the same file already walked those roots (a false negative, not a data-loss risk).
`_dvr_hls_staging_scan_roots()` now returns the default root plus every root
`_dvr_sidecar_scan_roots()` resolves from the four path templates, de-duplicated so nothing is walked
twice. The default root stays because a template that resolves straight into it is excluded from the
sidecar roots on purpose. Reading the templates needs Dispatcharr's Django models, which this scan never
needed before, so if that lookup fails it falls back to the default root alone -- exactly the old
behavior. Widening the roots doesn't widen what can be deleted: it is still only a directory with the
exact `.dvr_<id>_hls` name whose id has no `Recording` row, and a root that isn't mounted is skipped.
