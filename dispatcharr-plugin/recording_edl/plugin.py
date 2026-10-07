"""
Recording EDL -- a Dispatcharr plugin.

Exposes a completed recording's comskip-generated .edl file (commercial
break markers) over Dispatcharr's plugin run/ API, so a client with no
filesystem access to /data/recordings (this plugin was designed alongside
pvr.dispatcharr-unofficial, a Kodi PVR addon) can still fetch the data and show
skip/highlight markers on Kodi's own seekbar.

Why this exists as its own plugin rather than reading the file directly
over HTTP: confirmed against Dispatcharr's own source
(apps/channels/api_views.py's RecordingViewSet) that the only file-serving
action, `file`, always serves exactly custom_properties["file_path"] --
no way to redirect it at a sibling file -- and there's no generic static
route reaching /data/recordings/... either (dispatcharr/urls.py only
exposes Django's own MEDIA_ROOT, an unrelated path inside the app tree,
confirmed via dispatcharr/settings.py). Plugins can't register their own
URL routes either (confirmed via apps/plugins/loader.py -- no
route-registration hook exists), so the plugin run/ API -- already used
by this project's other companion plugin, timeshift_buffer -- is the only
reachable mechanism. This plugin runs inside Dispatcharr's own process,
so it can read the file straight off the same disk Dispatcharr itself
wrote it to, no HTTP round-trip to the recording's storage needed.

Kept as its own small plugin rather than folded into timeshift_buffer:
unrelated concern (comskip/recordings vs. live-channel buffering), no
shared state, no background processes or extra port to expose -- a
single stateless read-a-file-and-parse-it action, installable
independently of whether someone wants server-side live timeshift at
all.

comskip's .edl format (confirmed against docker/comskip.ini's own
[Output] section and apps/channels/tasks.py's own parsing of it): plain
text, one entry per line, whitespace-separated
`<start_seconds> <end_seconds> <type>`, times as fractional seconds.
Dispatcharr's shipped comskip.ini sets edl_skip_field=3, so with it every
line's type is Kodi's own PVR_EDL_TYPE_COMBREAK (3). Dispatcharr lets a user
supply their own comskip.ini, which takes precedence, and comskip's own
default for that field is 0 (a hard cut, PVR_EDL_TYPE_CUT) -- so 3 is what the
shipped ini produces, not something guaranteed. The type is therefore passed
through as whatever the file actually says rather than hardcoded (an
out-of-range value is coerced by the addon, see ParseRecordingEdlEntryJson()).

Only ever populated for a completed recording in comskip "mark" mode:
in the default "cut" mode, Dispatcharr physically removes the
commercials via ffmpeg and deletes the .edl file once done (confirmed in
tasks.py's comskip_process_recording), so there's nothing left to fetch
by the time a recording shows status: completed. get_edl returns an
empty entries list rather than an error in that case (and for any
recording comskip never touched at all) -- "no markers" is the normal,
expected outcome for most recordings, not a failure.

Corrected 2026-09-26 (a 23rd-pass audit, confirmed against Dispatcharr's
own real current upstream source): the "cut" branch's own
custom_properties.comskip still sets "edl" to the now-deleted file's
basename -- it's not simply absent as this docstring previously
claimed -- so _edl_path_for() checks for "segments_kept" instead (a key
only that branch ever sets, unlike a "mode" key, which "mark" sets but
"cut" doesn't) and returns None outright, rather than resolving a path
to a file that's already gone and relying on the resulting
FileNotFoundError being caught.

Also exposes scrub_orphaned_sidecars: a confirmed Dispatcharr-core bug is
that deleting a recording (RecordingViewSet.destroy() in
apps/channels/api_views.py) only removes custom_properties["file_path"]
and, if present, an in-progress HLS staging dir -- it never removes the
comskip "mark"-mode .edl file, nor the .logo.txt file comskip also writes
alongside it, both left behind under /data/recordings forever. This also
silently defeats destroy()'s own _prune_empty_parents() cleanup, since the
show/season folder never actually ends up empty. This action finds and
removes exactly those two sidecar types when the recording they belong to
is genuinely gone (no other file in the same directory shares their base
name), then sweeps for any directory left empty -- by that removal or
already empty beforehand. Confirmed safe against tasks.py's own
comskip_process_recording: comskip never runs until file_path already
exists, and "cut" mode's own file_path replacement is an atomic os.replace
(no window where file_path is transiently absent while the .edl still
exists), so an orphaned sidecar unambiguously means the recording was
deleted, not "still processing."

Scoped strictly to where DVR Settings' four path templates (TV Path
Template, TV Fallback Template, Movie Path Template, Movie Fallback
Template) actually resolve to -- never the bare /data/recordings root,
and never a dot-prefixed directory at any depth. This is a hard-won
lesson, not a stylistic choice: an earlier version of this action always
scanned all of /data/recordings, which on 2026-09-05 caused its
empty-directory sweep to walk into (and remove, once found empty)
Dispatcharr's own .dvr_*_hls staging directories and a .timeshift
directory living at that same top level -- neither of which any DVR path
template controls, and neither of which this action has any business
reasoning about. See _dvr_sidecar_scan_roots and _is_under_dotted_dir.
"""

import contextlib
import math
import os
import re
import shutil
import stat
from pathlib import Path

# Matches Dispatcharr's own hardcoded library_root/recordings_root
# (tasks.py's _build_output_paths, api_views.py's RecordingViewSet) --
# every DVR path template is joined under this unless the template itself
# is absolute (see _dvr_sidecar_scan_roots below).
_RECORDINGS_ROOT = Path("/data/recordings")

# Ordered longest-suffix-first so ".logo.txt" (a compound extension) is
# checked before anything that would only strip its trailing ".txt".
_SIDECAR_SUFFIXES = (".logo.txt", ".edl")


def _parse_edl(text: str):
    entries = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            start_sec = float(parts[0])
            end_sec = float(parts[1])
            # Missing type column defaults to 3 (Kodi commercial-break
            # marker) -- matches what Dispatcharr's own comskip.ini always
            # writes anyway (edl_skip_field=3), just tolerating a file that
            # for some reason only has two columns. OverflowError is caught
            # alongside ValueError, not just for symmetry: int(float("inf"))
            # raises OverflowError, not ValueError, and this line is the
            # only one of the three numeric conversions actually inside
            # this try block -- start/end's own conversion happens below,
            # guarded separately (see the isfinite() check).
            edl_type = int(float(parts[2])) if len(parts) >= 3 else 3
        except (ValueError, OverflowError):
            continue
        # start_sec/end_sec themselves never raise here -- float("nan")
        # and float("inf") both parse successfully -- but round()/int()
        # below would (ValueError for nan, OverflowError for inf), and
        # unlike the try block above, nothing here would catch that: a
        # single such line previously took down the *entire* get_edl
        # result for a recording (no try/except around this function's
        # own call site either), showing zero markers instead of just
        # skipping the one bad line. comskip is the only realistic
        # producer of this file and isn't expected to ever emit either,
        # but a parser reading data from a file shouldn't trust that.
        #
        # Checked on the *millisecond* values actually passed to round(),
        # not start_sec/end_sec themselves -- a real gap found via a
        # project-wide review: a finite-but-huge value (e.g. 1e306)
        # passes isfinite() here but overflows to inf once multiplied by
        # 1000, and round()/int() on that still raises OverflowError
        # uncaught, the same crash this check was meant to prevent.
        start_ms = start_sec * 1000
        end_ms = end_sec * 1000
        if not (math.isfinite(start_ms) and math.isfinite(end_ms)):
            continue
        entries.append(
            {
                "start": int(round(start_ms)),
                "end": int(round(end_ms)),
                "type": edl_type,
            }
        )
    return entries


def _edl_path_for(custom_properties: dict):
    """Resolves the on-disk EDL sidecar path for a recording's own
    custom_properties, or None if there's nothing legitimate to fetch --
    pulled out of Plugin.run()'s get_edl handler specifically so it's
    unit-testable standalone; see tests/test_recording_edl.py.

    Returns None when comskip hasn't run, found nothing, or ran in "cut"
    mode and already deleted the .edl file (see module docstring) --
    not an error, just nothing to show. Also returns None when the
    stored edl filename doesn't look like the plain filename
    Dispatcharr's own comskip tooling always writes here (an absolute
    path, one containing a directory separator, or a ".." component) --
    never reproduced live, but a stored filename shouldn't be trusted to
    stay within file_path's own directory without checking.

    Also returns None (rather than raising) when custom_properties
    itself, or comskip/edl/file_path within it, aren't the types they're
    expected to be -- a real gap found via a project-wide review: a
    non-dict custom_properties (e.g. a stray list/string value) or a
    non-dict comskip raised AttributeError at their own .get(), and a
    non-str edl/file_path raised TypeError at Path(...), either crashing
    get_edl entirely instead of reporting "no entries" the same way a
    missing value already does. custom_properties is externally-sourced
    (Dispatcharr's own DB column), so its shape shouldn't be trusted
    without checking, same reasoning as the path-traversal guard below."""
    if not isinstance(custom_properties, dict):
        return None
    comskip = custom_properties.get("comskip")
    if not isinstance(comskip, dict):
        comskip = {}
    # "cut" mode (Dispatcharr's own default) deletes the .edl file right
    # after a successful cut (os.remove(edl_path), apps/channels/tasks.py) --
    # but still leaves "edl" pointing at that now-deleted filename, unlike
    # the "mark"/no-commercials-skipped branches, which never delete it.
    # "segments_kept" is only ever set by the cut branch, so it's the
    # actual distinguishing signal, not a "mode" key -- confirmed against
    # Dispatcharr's own real current upstream source (a 23rd-pass audit,
    # cloned into a scratchpad, never committed to this repo): the
    # "mark" branch sets mode="mark" but no "segments_kept", and the
    # no/negligible-commercials branch sets neither at all. Fix for a
    # real, confirmed bug: without this, get_edl kept trying to read a
    # file that comskip's own cut mode had already deleted, logging a
    # spurious "Could not read <path>" warning on every single playback
    # of a cut-mode recording with commercials -- functionally harmless
    # today (the caller already treats a read failure as "no entries"),
    # but misleading, and fragile if that read ever silently succeeded
    # against a stale file with the same name reused some other way.
    if "segments_kept" in comskip:
        return None
    edl_filename = comskip.get("edl")
    file_path = custom_properties.get("file_path")
    if not isinstance(edl_filename, str) or not edl_filename:
        return None
    if not isinstance(file_path, str) or not file_path:
        return None
    candidate = Path(edl_filename)
    if candidate.is_absolute() or candidate.name != edl_filename or ".." in candidate.parts:
        return None
    # comskip only ever writes "<recording>.edl". Anything else named here (a video file, a device
    # node) is not an EDL sidecar and is never read -- see _read_edl_text().
    if candidate.suffix.lower() != ".edl":
        return None
    return Path(file_path).parent / edl_filename


# The largest EDL sidecar get_edl will read. A real one is a few lines per commercial break (tens
# of bytes each); the cap exists because the path comes from a recording's custom_properties, which a
# DVR-manage account can write, and the read used to be whole and unbounded (found by the 2026-10-04
# fifth hardening sweep: pointing "edl" at the recording's own multi-GB video, or file_path at a device
# directory, made the Dispatcharr worker read it into memory until it was killed).
_MAX_EDL_BYTES = 1024 * 1024


def _is_under_any_root(path: Path, roots) -> bool:
    """Whether `path`, with symlinks and ".." resolved, lies inside one of `roots` (each resolved the
    same way). Pure filesystem logic, so it is unit-tested directly."""
    try:
        resolved = Path(os.path.realpath(path))
    except (OSError, ValueError):
        return False
    for root in roots:
        try:
            real_root = Path(os.path.realpath(root))
        except (OSError, ValueError):
            continue
        if resolved == real_root or real_root in resolved.parents:
            return True
    return False


def _read_edl_text(path: Path, max_bytes: int = _MAX_EDL_BYTES) -> str:
    """Reads an EDL sidecar's text: only a regular file (never a device, FIFO or directory, opened
    non-blocking so a FIFO cannot hang the worker), and at most `max_bytes`. Raises OSError otherwise,
    which get_edl already reports as "could not read"."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as f:
        if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
            raise OSError("not a regular file")
        data = f.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise OSError(f"larger than {max_bytes} bytes")
    return data.decode("utf-8", errors="replace")


def _edl_result(entries: list) -> dict:
    """The get_edl action's own success response shape -- pulled out
    specifically so it's unit-testable standalone; see
    tests/test_recording_edl.py. "message" is what actually shows up in
    Dispatcharr's own result toast -- confirmed against its frontend
    (PluginCard.jsx's handlePluginRun()) that nothing else in an
    action's response is ever rendered anywhere in that UI, same finding
    that led to timeshift_buffer's own diagnostic actions all getting
    one too."""
    message = (
        "No EDL entries found" if not entries else f"{len(entries)} EDL entr{'y' if len(entries) == 1 else 'ies'} found"
    )
    return {"status": "ok", "message": message, "entries": entries}


def _sidecar_base_name(filename: str):
    """The recording's own base filename a sidecar was generated from, or
    None if `filename` isn't one of the two comskip sidecar types. Can't
    use Path.stem/splitext for .logo.txt -- that would only strip the
    trailing .txt and leave ".logo" attached, making it look unrelated to
    a same-named .mkv/.ts video that has a plain single-extension stem."""
    for suffix in _SIDECAR_SUFFIXES:
        if filename.endswith(suffix):
            return filename[: -len(suffix)]
    return None


def _is_under_dotted_dir(path: Path, root: Path) -> bool:
    """True if any path component between `root` and `path` starts with
    "." -- Dispatcharr's own internal/working directories (.dvr_*_hls
    staging dirs, and apparently others such as .timeshift) are named
    exactly this way and must never be descended into, read as an
    "empty" candidate, or removed by this action, regardless of what a
    DVR path template resolves to. A second, independent layer of
    protection on top of _dvr_sidecar_scan_roots deliberately no longer
    including the bare /data/recordings root -- belt and suspenders after
    a real incident on 2026-09-05 where a .dvr_*_hls staging directory
    and a .timeshift directory were both wiped by an earlier version of
    this action's empty-directory sweep."""
    return any(part.startswith(".") for part in path.relative_to(root).parts)


def _resolve_scan_root(template: str):
    """Pure logic pulled out of _dvr_sidecar_scan_roots specifically so
    it's unit-testable without Dispatcharr's Django models (see
    ../tests/test_plugin.py) -- resolves a single DVR path template
    string to its scan-root directory, or None if the template yields no
    usable subdirectory root. See _dvr_sidecar_scan_roots's own docstring
    for the full reasoning (absolute-vs-relative resolution, the "{"
    placeholder trim, and why a bare _RECORDINGS_ROOT candidate is
    explicitly excluded -- the exact 2026-09-05 incident this guards
    against)."""
    if not template:
        return None
    resolved = template if template.startswith("/") else f"{_RECORDINGS_ROOT}/{template}"
    prefix = resolved.split("{", 1)[0]
    slash_idx = prefix.rfind("/")
    candidate = Path(prefix[:slash_idx]) if slash_idx > 0 else None
    if candidate is None:
        return None
    # Normalize away any ".."/"."/doubled-slash noise before comparing --
    # Path itself never collapses "..": Path("/data/recordings/TV_Shows/
    # ..") != Path("/data/recordings"), and os.path.normpath() alone
    # deliberately leaves a doubled leading "//" untouched (a POSIX
    # special case this project has no reason to honor, since
    # _RECORDINGS_ROOT is a single fixed, already-normal path). Without
    # this, a template like "TV_Shows/../{show}/..." resolved to a
    # candidate that was actually _RECORDINGS_ROOT itself (or, with more
    # ".." segments, an ancestor of it, up to and including "/") once
    # normalized, but compared unequal to it and sailed straight past
    # the very check meant to catch this -- a real, confirmed gap of the
    # same severity as the 2026-09-05 incident this function exists to
    # prevent: the orphan-sidecar scrub and empty-directory sweep would
    # then walk that escaped, far-too-broad root instead of a real
    # recordings subdirectory.
    #
    # Deliberately does NOT require the normalized candidate to be a
    # *descendant* of _RECORDINGS_ROOT -- an absolute template pointing
    # somewhere else entirely (e.g. "/mnt/media/TV/{show}/...") is still
    # something this plugin handles (see the startswith("/") branch above),
    # not something to reject. (Current Dispatcharr no longer serves or
    # deletes such a recording itself -- docs/RECORDING_EDL.md, "Upstream
    # limitations" -- but sidecars and staging directories there still need
    # finding and scrubbing.) Only _RECORDINGS_ROOT itself, or an ancestor
    # of it, is refused.
    normalized = Path(os.path.normpath(re.sub(r"/{2,}", "/", str(candidate))))
    if normalized == _RECORDINGS_ROOT or normalized in _RECORDINGS_ROOT.parents:
        return None
    return normalized


def _dedupe_scan_roots(roots):
    """Pure logic pulled out of _dvr_sidecar_scan_roots -- drops any root
    already covered by another (itself or an ancestor) already kept, so
    an overlapping template doesn't cause the same directory to be
    walked, and its now-empty parents reasoned about for pruning, more
    than once."""
    deduped = []
    for root in sorted(roots, key=lambda p: len(p.parts)):
        if not any(root == kept or kept in root.parents for kept in deduped):
            deduped.append(root)
    return deduped


def _dvr_sidecar_scan_roots():
    """Directories that a recording (and therefore its .edl/.logo.txt
    sidecars) could actually land in, derived from DVR Settings' four
    path templates -- TV Path Template, TV Fallback Template, Movie Path
    Template, Movie Fallback Template (tv_template, tv_fallback_template,
    movie_template, movie_fallback_template in CoreSettings.get_dvr_settings())
    -- rather than assuming everyone left them at their "TV_Shows/"Movies/"
    defaults. Confirmed in tasks.py's _build_output_paths(): a template is
    joined under the hardcoded /data/recordings library_root UNLESS the
    template itself starts with "/", in which case it's used as-is --
    Dispatcharr's own comment there ("so users can structure their
    library under /data as desired") confirms this is a supported way to
    point a template at a separate library entirely, so a scan limited to
    only /data/recordings would silently miss those recordings' sidecars.

    For each template, the scan root is everything up to its first "{"
    placeholder (trimmed back to the last complete path separator) --
    e.g. "TV_Shows/{show}/S{season:02d}E{episode:02d}.mkv" joined under
    /data/recordings gives a root of /data/recordings/TV_Shows, while an
    absolute "/mnt/media/TV/{show}/..." gives /mnt/media/TV.

    Deliberately does NOT include the bare /data/recordings root itself,
    full stop -- including the rare case where a template resolves
    directly into it with no subdirectory at all (e.g. a bare
    "{show}.mkv"), which is also excluded rather than included, a
    corrected claim as of a project-wide review (2026-09-26): an earlier
    version of this docstring claimed that rare case was a supported
    exception, but _resolve_scan_root() has never actually implemented
    one -- see test_resolve_scan_root_bare_root_excluded_regression's
    own docstring for exactly why that's the right call to leave in
    place rather than "fix": telling a template that legitimately
    resolves to the bare root apart from one that only gets there by
    normalizing away a "../" escape (the 2026-09-05 incident below) is
    solvable, but this project has no real installation using a
    subdirectory-less template to justify the added risk, and the
    2026-09-05 incident this whole function exists to prevent was severe
    enough (see below) that erring toward "never scan the bare root,
    even in the one case where it might be safe" is the deliberate
    choice here. Confirmed live on 2026-09-05: an earlier version of this
    function always added /data/recordings as a root regardless, which
    made the empty-directory sweep walk (and remove, once it found them
    empty) Dispatcharr's own unrelated top-level entries living there --
    .dvr_*_hls staging directories and a .timeshift directory -- neither
    of which any DVR path template controls. This action must only ever
    touch the directories your templates actually point recordings into.

    Just orchestration now -- the actual per-template resolution and
    deduplication (_resolve_scan_root/_dedupe_scan_roots) are pure
    functions with no Django dependency, pulled out so they're
    unit-testable standalone.
    """
    from core.models import CoreSettings

    roots = {
        r
        for r in (
            _resolve_scan_root(t)
            for t in (
                CoreSettings.get_dvr_tv_template(),
                CoreSettings.get_dvr_tv_fallback_template(),
                CoreSettings.get_dvr_movie_template(),
                CoreSettings.get_dvr_movie_fallback_template(),
            )
        )
        if r is not None
    }
    return _dedupe_scan_roots(roots)


def _prune_empty_directories(root, logger):
    """Removes every directory under `root` (never `root` itself) left
    with zero entries -- not just ones a sidecar removal happened to
    empty out this run, but any directory that was already empty going
    in (e.g. a show/season folder some earlier, unrelated deletion left
    behind). Walked bottom-up (deepest paths first) in one pass: each
    directory's own emptiness is checked live via iterdir() at the point
    the loop reaches it, so a leaf removed earlier in the same pass makes
    its now-empty parent -- reached later, since it has fewer path parts
    -- eligible too, without needing a second pass."""
    removed = []
    errors = []

    if not root.is_dir():
        return removed, errors

    dirs = sorted(
        (p for p in root.rglob("*") if p.is_dir() and not _is_under_dotted_dir(p, root)),
        key=lambda p: len(p.parts),
        reverse=True,
    )
    for d in dirs:
        try:
            next(d.iterdir())
            continue  # not empty
        except StopIteration:
            pass
        except OSError as exc:
            errors.append(f"{d}: {exc}")
            continue

        try:
            d.rmdir()
        except OSError as exc:
            errors.append(f"{d}: {exc}")
            continue

        removed.append(str(d))
        if logger:
            logger.info("recording_edl: removed empty directory %s", d)

    return removed, errors


def _scrub_orphaned_recording_sidecars(logger):
    """Removes .edl/.logo.txt sidecars whose recording is gone, then, per
    scan root, sweeps for any directory left empty -- by that removal or
    already empty beforehand. See the module docstring for why "no other
    file in this directory shares the sidecar's base name" is a safe,
    unambiguous orphan test, and _dvr_sidecar_scan_roots for why this
    scans more than just the hardcoded /data/recordings root."""
    removed_files = []
    removed_dirs = []
    errors = []

    for root in _dvr_sidecar_scan_roots():
        if not root.is_dir():
            continue

        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if _is_under_dotted_dir(path, root):
                continue
            base = _sidecar_base_name(path.name)
            if base is None:
                continue

            try:
                siblings = list(path.parent.iterdir())
            except OSError as exc:
                errors.append(f"{path}: {exc}")
                continue

            has_owning_recording = any(
                other != path and other.is_file() and _sidecar_base_name(other.name) is None and other.stem == base
                for other in siblings
            )
            if has_owning_recording:
                continue

            try:
                path.unlink()
            except OSError as exc:
                errors.append(f"{path}: {exc}")
                continue

            removed_files.append(str(path))
            if logger:
                logger.info("recording_edl: removed orphaned sidecar %s", path)

        dirs_removed, dir_errors = _prune_empty_directories(root, logger)
        removed_dirs.extend(dirs_removed)
        errors.extend(dir_errors)

    return removed_files, removed_dirs, errors


_HLS_STAGING_DIR_RE = re.compile(r"\.dvr_([0-9]+)_hls", re.ASCII)


def _hls_staging_dir_recording_id(name: str):
    """Extracts the recording id from a `.dvr_<id>_hls` staging
    directory's own name, or None if `name` doesn't match that pattern
    at all (e.g. a non-numeric id like `.dvr_abc_hls`, or an unrelated
    dotfile) -- pulled out of _classify_dvr_hls_dir specifically so it's
    unit-testable standalone; see tests/test_recording_edl.py.

    Only the exact canonical form Dispatcharr itself ever writes
    (`f".dvr_{recording_id}_hls"`, apps/channels/tasks.py -- plain ASCII
    digits, no leading zeros) is accepted. Fix for a real, confirmed gap
    (added 2026-09-27, a 64th-pass audit, found via a project-wide
    review, confirmed by direct reproduction against this exact regex,
    not reproduced live -- the same class as timeshift_buffer's own
    _is_canonical_uuid() strictness fix): the previous
    `re.match(r"^\\.dvr_(\\d+)_hls$", ...)` also accepted Unicode
    decimal digits (Python's str-pattern `\\d` isn't ASCII-only --
    ".dvr_٣_hls" parsed as recording 3), a zero-padded id
    (".dvr_0005_hls" as recording 5), and a trailing newline (`$`
    matches just before one). Any of those names, found by
    _list_dvr_hls_staging_dirs()'s own rglob() under the recordings
    root with no Recording row at the resulting id, was classified
    "orphaned" -- and _delete_orphaned_dvr_hls_dirs() rmtree()s every
    "orphaned" directory, so a directory Dispatcharr never created could
    be deleted outright by this plugin's own destructive scrub action."""
    match = _HLS_STAGING_DIR_RE.fullmatch(name)
    if not match:
        return None
    digits = match.group(1)
    if digits != str(int(digits)):
        return None  # leading zeros -- not a name Dispatcharr ever writes
    return int(digits)


def _count_ts_segments(hls_dir: Path):
    """Counts the `.ts` segment files directly inside `hls_dir` (no
    recursion), or None if the directory can't be listed at all (e.g. a
    race with Dispatcharr's own concurrent removal of it) -- pulled out
    of _classify_dvr_hls_dir specifically so it's unit-testable
    standalone against a real temp filesystem; see
    tests/test_recording_edl.py."""
    try:
        return sum(1 for f in hls_dir.iterdir() if f.is_file() and f.suffix == ".ts")
    except OSError:
        return None


def _classify_hls_dir_info(hls_dir: Path, recording_id: int, recording_exists: bool, custom_properties, segment_count):
    """Pure classification core of _classify_dvr_hls_dir -- takes the
    already-resolved recording_exists/custom_properties instead of
    querying Django directly, so it's unit-testable standalone. See
    _classify_dvr_hls_dir's own docstring for the full classification/
    incident background; see ../tests/test_recording_edl.py for the
    regression coverage."""
    if not recording_exists:
        classification = "orphaned"
        detail = f"No Recording row with id {recording_id} -- its owning recording was deleted"
    else:
        # Fix for a real, confirmed bug (found via a project-wide review,
        # same class as _edl_path_for's own hardening): custom_properties
        # is externally-sourced (Dispatcharr's own DB column), so its
        # shape shouldn't be trusted. A non-dict value (e.g. a stray
        # list/string) previously raised AttributeError at cp.get(), and a
        # non-str/PathLike own_hls_dir raised TypeError at Path(...) --
        # either one aborting the *entire* scan (list_dvr_hls_staging_dirs/
        # delete_orphaned_dvr_hls_dirs's own loops have no try/except
        # around this call), not just this one directory's classification.
        cp = custom_properties if isinstance(custom_properties, dict) else {}
        own_hls_dir = cp.get("_hls_dir")
        status = cp.get("status", "")
        remux_success = cp.get("remux_success")
        same_dir = bool(own_hls_dir) and isinstance(own_hls_dir, (str, os.PathLike)) and Path(own_hls_dir) == hls_dir

        if same_dir and status == "recording":
            classification = "active"
            detail = "Recording is currently in progress"
        elif same_dir and remux_success is False:
            classification = "preserved_failure"
            detail = "Concat/remux failed; Dispatcharr kept this as the only surviving copy"
        elif same_dir:
            classification = "referenced"
            detail = f"Recording {recording_id} (status={status!r}) still references this directory"
        else:
            classification = "referenced"
            detail = (
                f"Recording {recording_id} exists but its _hls_dir "
                f"({own_hls_dir!r}) doesn't match this path -- needs manual review"
            )

    return {
        "path": str(hls_dir),
        "recording_id": recording_id,
        "recording_exists": recording_exists,
        "classification": classification,
        "detail": detail,
        "segment_count": segment_count,
    }


def _classify_dvr_hls_dir(hls_dir: Path):
    """Best-effort, read-only classification of a .dvr_<id>_hls staging
    directory. Not reporting-only: _delete_orphaned_dvr_hls_dirs()
    rmtree()s every directory this classifies "orphaned" (and nothing
    else), so a wrong "orphaned" here is data loss, not just a wrong
    report. (This docstring used to say "for reporting only, never used
    to decide anything destructive" -- true when first written, stale
    since the delete_orphaned_dvr_hls_dirs action was added; corrected
    2026-09-27, a 65th-pass audit.) Confirmed against tasks.py's
    run_recording and api_views.py's RecordingViewSet.destroy(): on a
    successful concat,
    Dispatcharr removes the directory itself (after waiting out an active
    HLS viewer's heartbeat window) and clears custom_properties["_hls_dir"];
    on a failed concat (direct AND the MP4-intermediate fallback both
    failed) it deliberately KEEPS the directory, logging "Keeping HLS
    segments for recovery" -- the only surviving copy of that recording's
    video; and deleting a Recording also removes its _hls_dir via a
    background daemon thread with no persistence or retry, so a
    Dispatcharr restart/crash between the DB delete and that thread
    finishing can leave a directory with no owning Recording row at all.

    classification is one of (see _classify_hls_dir_info for the actual
    decision logic):
      "active"            -- status == "recording"; a recording in
                              progress. Never touch.
      "preserved_failure"  -- concat/remux failed; the only surviving copy
                              of this recording's video. Never touch.
      "referenced"         -- a Recording row still points _hls_dir at
                              this directory, but neither of the above
                              conditions is confirmed. Needs manual review.
      "orphaned"            -- no Recording row with this id exists at
                              all. The only category where a genuinely
                              deleted recording's leftover staging
                              directory is expected to land.
    """
    recording_id = _hls_staging_dir_recording_id(hls_dir.name)
    if recording_id is None:
        return None

    from apps.channels.models import Recording

    recording = Recording.objects.filter(id=recording_id).first()
    segment_count = _count_ts_segments(hls_dir)

    return _classify_hls_dir_info(
        hls_dir,
        recording_id,
        recording is not None,
        recording.custom_properties if recording is not None else None,
        segment_count,
    )


def _dvr_hls_staging_scan_roots():
    """Every directory a `.dvr_<id>_hls` staging directory can be in: the
    default recordings root, plus each root DVR Settings' path templates
    resolve to (_dvr_sidecar_scan_roots()).

    The second half is the point. Dispatcharr creates the staging directory
    next to the recording's own final file --
    `os.path.join(os.path.dirname(final_path), f".dvr_{recording_id}_hls")`
    (_build_output_paths(), apps/channels/tasks.py) -- and a path template
    that is absolute puts that file, and so this directory, somewhere other
    than /data/recordings (apps/channels/tasks.py: "so users can structure
    their library under /data as desired"). This scan only ever looked under
    the default root, so an orphaned staging directory in such a library was
    never listed and never cleaned up (docs/OPEN_ITEMS.md), while the sidecar
    scrub in this same file already walked those roots.

    The default root stays in the list because a template that resolves
    straight into it (no subdirectory) is excluded from
    _dvr_sidecar_scan_roots() on purpose, and a staging directory can sit
    directly under it. _dedupe_scan_roots() drops any template root already
    inside another, so nothing is walked twice.

    Reading the templates needs Dispatcharr's Django models, which this scan
    never needed before; if that lookup fails the scan falls back to the
    default root alone, i.e. exactly what it did before."""
    roots = [_RECORDINGS_ROOT]
    with contextlib.suppress(Exception):
        roots.extend(_dvr_sidecar_scan_roots())
    return _dedupe_scan_roots(roots)


def _list_dvr_hls_staging_dirs():
    """Read-only scan for every .dvr_*_hls staging directory under the
    default /data/recordings root or any root a DVR path template points
    at (_dvr_hls_staging_scan_roots()), each with its best-effort
    classification. Unlike scrub_orphaned_sidecars' own scoped scan roots,
    this includes the whole default recordings root -- a staging directory
    could in principle be found directly under it as well as nested under a
    DVR path template's own subdirectory. This
    scan itself deletes nothing, but it is not only a diagnostic: it's
    also exactly the list _delete_orphaned_dvr_hls_dirs() acts on, so
    that action's own reach is these roots too, limited only by
    _hls_staging_dir_recording_id()'s exact-name match and the
    "orphaned" classification. (Corrected 2026-09-27, a 65th-pass audit:
    this docstring previously justified the whole-root scope as "no
    deletion risk in reading a directory listing", written before the
    delete action existed and stale since.)"""
    results = []
    for root in _dvr_hls_staging_scan_roots():
        if not root.is_dir():
            continue
        for path in sorted(root.rglob(".dvr_*_hls")):
            if not path.is_dir():
                continue
            info = _classify_dvr_hls_dir(path)
            if info:
                results.append(info)
    return results


def _staging_dirs_message(dirs: list) -> str:
    """The list_dvr_hls_staging_dirs action's own "message" -- the one part of
    its result Dispatcharr's Plugins page shows (see _edl_result()). Counts per
    classification in first-seen order, then each directory by name with its
    segment count, "?" when the directory couldn't be listed. Pure, so it is
    unit-tested directly; see tests/test_recording_edl.py."""
    if not dirs:
        return "No .dvr_*_hls staging directories found"
    counts = {}
    for d in dirs:
        counts[d["classification"]] = counts.get(d["classification"], 0) + 1
    summary = ", ".join(f"{count} {label}" for label, count in counts.items())
    entries = "; ".join(
        f"{Path(d['path']).name} ({d['classification']}, "
        f"{d['segment_count'] if d['segment_count'] is not None else '?'} segs)"
        for d in dirs
    )
    return f"{len(dirs)} .dvr_*_hls dir(s) found ({summary}): {entries}"


def _delete_orphaned_dvr_hls_dirs(logger):
    """Deletes every .dvr_*_hls directory _classify_dvr_hls_dir calls
    "orphaned" -- no Recording row with that id exists at all -- and
    nothing else. Never touches "active" (a recording in progress),
    "preserved_failure" (concat failed, kept as the only surviving copy),
    or "referenced" (a Recording row exists but the signal isn't clean --
    needs manual review) directories, empty or not; see
    _classify_dvr_hls_dir's own docstring for the full reasoning behind
    each category. Classification is freshly recomputed here (via
    _list_dvr_hls_staging_dirs(), not a stale list from an earlier click),
    so this always acts on current state at the moment it actually runs.

    Deliberately does not prune parent directories left empty by a
    deletion -- scrub_orphaned_sidecars's own empty-directory sweep
    already covers that for any parent within a DVR path template's own
    scope, and reaching further than that here would repeat exactly the
    scope mistake _dvr_sidecar_scan_roots's own docstring documents."""
    removed = []
    errors = []
    for info in _list_dvr_hls_staging_dirs():
        if info["classification"] != "orphaned":
            continue
        path = Path(info["path"])
        # Re-classified immediately before the removal: the list above can be seconds old by the
        # time a large earlier directory has been removed, and a recording id reused after a database
        # reset could have gone from orphaned to live in between (found by the 2026-10-04 fifth sweep).
        fresh = _classify_dvr_hls_dir(path)
        if not fresh or fresh["classification"] != "orphaned":
            continue
        try:
            shutil.rmtree(path)
        except OSError as exc:
            errors.append(f"{path}: {exc}")
            continue
        removed.append(str(path))
        if logger:
            logger.info("recording_edl: removed orphaned .dvr_*_hls directory %s", path)
    return removed, errors


class Plugin:
    name = "Recording EDL"
    version = "0.2.3"
    description = (
        "Exposes a completed recording's comskip .edl (commercial break "
        "markers) over the plugin run/ API, for clients with no direct "
        "filesystem access to /data/recordings."
    )
    author = "BruiserBrody17"
    help_url = (
        "https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/tree/Omega/dispatcharr-plugin/recording_edl"
    )

    # See timeshift_buffer/plugin.py's own comment on this same pattern:
    # plugin.json's fields/actions are only read for the not-yet-trusted
    # preview; once trusted/loaded, this class is what's actually
    # introspected, so this is the real source of truth.
    fields = [
        {
            "id": "about",
            "label": "About",
            "type": "info",
            "description": (
                "Called by a client (e.g. pvr.dispatcharr-unofficial's recording "
                "playback) via the plugin run/ API, not usually by hand -- "
                "the field below is only for manually testing the action "
                "button."
            ),
        },
        {
            "id": "test_recording_id",
            "label": "Test recording ID",
            "type": "string",
            "default": "",
            "help_text": (
                "Only used by the manual-test button below (plugin action "
                "buttons can't take click-time input) -- paste a "
                "recording's numeric id here, save, then use Get Recording "
                "EDL. The real integration (a client calling run/ over the "
                "REST API) passes recording_id directly and ignores this "
                "field."
            ),
        },
    ]

    actions = [
        {
            "id": "get_edl",
            "label": "Get Recording EDL",
            "description": (
                "Returns the comskip EDL entries for a recording. Params: "
                "recording_id (required, pass it as a param, or paste one "
                "into the test_recording_id setting for manual testing)."
            ),
        },
        {
            "id": "scrub_orphaned_sidecars",
            "label": "Scrub Orphaned EDL/Logo Files",
            "description": (
                "Removes comskip .edl and .logo.txt files left behind by "
                "a deleted recording (Dispatcharr's own recording delete "
                "never removes them), then removes any now-or-already-"
                "empty directory under DVR Settings' TV/Movie Path and "
                "Fallback Template locations. Only ever removes a "
                "sidecar when no other file in its folder shares its "
                "base name, and never descends into or removes a "
                "dot-prefixed directory (.dvr_*_hls staging dirs, "
                "Dispatcharr/plugin-internal directories) -- scoped "
                "strictly to where your own DVR path templates point, "
                "nothing else under /data/recordings."
            ),
            "confirm": {
                "required": True,
                "title": "Scrub orphaned .edl/.logo.txt files?",
                "message": (
                    "Permanently deletes every .edl and .logo.txt file "
                    "with no matching recording left, and removes every "
                    "now-or-already-empty directory under your DVR "
                    "Settings' TV/Movie path templates. This cannot be "
                    "undone."
                ),
            },
        },
        {
            "id": "list_dvr_hls_staging_dirs",
            "label": "List DVR HLS Staging Directories",
            "description": (
                "Read-only diagnostic: finds every .dvr_*_hls staging "
                "directory under /data/recordings and reports its best-"
                'effort classification -- "active" (a recording in '
                'progress), "preserved_failure" (concat failed, kept as '
                'the only surviving copy), "referenced" (a Recording row '
                "points at it but neither of the above is confirmed -- "
                'needs manual review), or "orphaned" (no Recording row '
                "with that id exists at all). Never deletes anything -- "
                "for manual review before deciding what, if anything, is "
                "actually safe to clean up."
            ),
        },
        {
            "id": "delete_orphaned_dvr_hls_dirs",
            "label": "Delete Orphaned DVR HLS Directories",
            "description": (
                "Deletes only the .dvr_*_hls directories List DVR HLS "
                'Staging Directories classifies as "orphaned" -- no '
                "Recording row with that id exists at all. Never touches "
                '"active", "preserved_failure", or "referenced" '
                "directories, empty or not. Does not prune parent "
                "directories left empty by a deletion -- run Scrub "
                "Orphaned EDL/Logo Files for that."
            ),
            "confirm": {
                "required": True,
                "title": "Delete orphaned .dvr_*_hls directories?",
                "message": (
                    "Permanently deletes every .dvr_*_hls directory with "
                    "no Recording row referencing it at all. Active and "
                    "preserved-failure directories are never touched. "
                    "This cannot be undone."
                ),
            },
        },
    ]

    def run(self, action: str, params: dict, context: dict):
        logger = context.get("logger")
        settings_dict = context.get("settings", {})

        if action == "scrub_orphaned_sidecars":
            removed_files, removed_dirs, errors = _scrub_orphaned_recording_sidecars(logger)
            if not removed_files and not removed_dirs and not errors:
                message = "No orphaned .edl/.logo.txt files or empty directories found"
            else:
                message = f"Removed {len(removed_files)} orphaned file(s), {len(removed_dirs)} empty directory(s)"
                if errors:
                    message += f", {len(errors)} error(s) (see plugin log)"
            return {
                "status": "ok",
                "message": message,
                "removed_files": removed_files,
                "removed_directories": removed_dirs,
                "errors": errors,
            }

        if action == "list_dvr_hls_staging_dirs":
            dirs = _list_dvr_hls_staging_dirs()
            return {"status": "ok", "message": _staging_dirs_message(dirs), "directories": dirs}

        if action == "delete_orphaned_dvr_hls_dirs":
            removed, errors = _delete_orphaned_dvr_hls_dirs(logger)
            if not removed and not errors:
                message = "No orphaned .dvr_*_hls directories found"
            else:
                message = f"Removed {len(removed)} orphaned .dvr_*_hls director{'y' if len(removed) == 1 else 'ies'}"
                if errors:
                    message += f", {len(errors)} error(s) (see plugin log)"
            return {"status": "ok", "message": message, "removed": removed, "errors": errors}

        if action != "get_edl":
            return {"status": "error", "message": f"Unknown action: {action}"}

        recording_id = params.get("recording_id") or settings_dict.get("test_recording_id")
        if not recording_id:
            return {
                "status": "error",
                "message": "recording_id is required (pass it as a param, or paste one into "
                "the test_recording_id setting for manual testing)",
            }

        from apps.channels.models import Recording

        try:
            recording = Recording.objects.get(pk=recording_id)
        except Recording.DoesNotExist:
            return {"status": "error", "message": f"No recording with id {recording_id}"}
        except (ValueError, TypeError):
            return {"status": "error", "message": f"Invalid recording_id: {recording_id!r}"}

        cp = recording.custom_properties or {}
        edl_path = _edl_path_for(cp)
        if edl_path is None:
            # Nothing to fetch -- comskip hasn't run, found nothing, ran in
            # "cut" mode and already deleted the .edl file, or the stored
            # filename didn't look legitimate (see _edl_path_for's own
            # docstring). Not an error: just no markers for this recording.
            return {
                "status": "ok",
                "message": 'No EDL file for this recording (comskip hasn\'t run, or ran in "cut" mode)',
                "entries": [],
            }

        # The path is built from custom_properties, which is not trusted to stay near the recording:
        # it must resolve inside a DVR directory (a template root or the recordings root).
        try:
            confined = _is_under_any_root(edl_path, _dvr_hls_staging_scan_roots())
        except Exception:
            # Defensive: _dvr_hls_staging_scan_roots() already falls back to the default recordings
            # root when the DVR settings cannot be read, so this is only reached if that changes.
            confined = False
        if not confined:
            if logger:
                logger.warning("recording_edl: refusing to read %s: outside the DVR directories", edl_path)
            return {"status": "ok", "message": f"Could not read {edl_path.name} (see plugin log)", "entries": []}

        try:
            text = _read_edl_text(edl_path)
        except OSError as exc:
            if logger:
                logger.warning("recording_edl: could not read %s: %s", edl_path, exc)
            return {"status": "ok", "message": f"Could not read {edl_path.name} (see plugin log)", "entries": []}

        return _edl_result(_parse_edl(text))
