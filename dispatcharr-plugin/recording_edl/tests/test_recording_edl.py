"""Unit tests for dispatcharr-plugin/recording_edl/plugin.py.

Loaded by explicit file path with a unique synthetic module name (not a
bare `import plugin`) since timeshift_buffer's own plugin.py shares the
same filename -- importing both by name in one pytest session would
otherwise collide via sys.modules.

Covers only the Dispatcharr-independent pure/filesystem logic --
everything that touches Dispatcharr's own Django models directly (the
Recording.objects.filter()/CoreSettings query inside
_classify_dvr_hls_dir/_dvr_sidecar_scan_roots) stays untested here, same
boundary this project's C++ addon test suite draws at the Kodi SDK:
verification of that layer is still manual/live-instance territory (see
docs/OPEN_ITEMS.md's "No automated test suite exists" entry).
_classify_dvr_hls_dir's own classification decision logic is pulled out
into _classify_hls_dir_info(), which takes the already-resolved
recording_exists/custom_properties instead of querying Django itself,
so it IS covered directly (not just indirectly via run()'s message
formatting below).

Plugin.run()'s own dispatch and message-formatting *is* covered, for
every action branch that either has no Django dependency at all
(scrub_orphaned_sidecars, the recording_id-missing path of get_edl, an
unknown action) or where the one Django-dependent call
(_list_dvr_hls_staging_dirs/_delete_orphaned_dvr_hls_dirs) can be
monkeypatched the same way _scrub_orphaned_recording_sidecars's own
tests already monkeypatch _dvr_sidecar_scan_roots -- exercising the
real message text a client actually sees, not just the underlying
helper functions in isolation.
"""

import importlib.util
import math
import os
from pathlib import Path

import pytest

_PLUGIN_PATH = Path(__file__).parent.parent / "plugin.py"
_spec = importlib.util.spec_from_file_location("recording_edl_plugin", _PLUGIN_PATH)
recording_edl_plugin = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(recording_edl_plugin)


# ---------------------------------------------------------------------
# _parse_edl
# ---------------------------------------------------------------------


def test_parse_edl_valid_lines():
    text = "1.5 3.25 3\n10 20\n"
    entries = recording_edl_plugin._parse_edl(text)
    assert entries == [
        {"start": 1500, "end": 3250, "type": 3},
        {"start": 10000, "end": 20000, "type": 3},  # missing type column defaults to 3
    ]


def test_parse_edl_skips_blank_and_short_lines():
    text = "\n   \n1 2 3\nonly-one-field\n"
    entries = recording_edl_plugin._parse_edl(text)
    assert entries == [{"start": 1000, "end": 2000, "type": 3}]


def test_parse_edl_skips_non_numeric_start():
    text = "not-a-number 2 3\n5 6 3\n"
    entries = recording_edl_plugin._parse_edl(text)
    assert entries == [{"start": 5000, "end": 6000, "type": 3}]


def test_parse_edl_nan_inf_regression():
    """The exact pre-fix crash this project's own docs/RECORDING_EDL.md
    documents: round()/int() on nan/inf raised uncaught (ValueError for
    nan, OverflowError for inf) since that conversion happened outside
    the try/except guarding the initial float() parse. A single bad line
    anywhere in the file took down the *entire* result. Confirms nan/inf
    in any of the three columns is skipped, while every other valid
    entry in the same file still comes back."""
    text = "\n".join(
        [
            "1 2 3",  # valid
            "nan 2 3",  # start is nan
            "1 nan 3",  # end is nan
            "inf 2 3",  # start is inf
            "1 inf 3",  # end is inf
            "1 2 inf",  # type column is inf (OverflowError, not ValueError)
            "3 4 3",  # valid
        ]
    )
    entries = recording_edl_plugin._parse_edl(text)
    assert entries == [
        {"start": 1000, "end": 2000, "type": 3},
        {"start": 3000, "end": 4000, "type": 3},
    ]


def test_parse_edl_rounds_to_nearest_millisecond():
    entries = recording_edl_plugin._parse_edl("1.5006 2.4994 3")
    assert entries == [{"start": 1501, "end": 2499, "type": 3}]


def test_parse_edl_skips_a_finite_value_that_overflows_once_scaled_to_milliseconds():
    """Regression test for a real gap: a finite-but-huge value (e.g.
    1e306) passes isfinite() on start_sec/end_sec themselves, but
    overflows to inf once multiplied by 1000 -- round()/int() on that
    still raised OverflowError uncaught, taking down the entire result
    the same way the nan/inf regression above already guards against."""
    text = "\n".join(
        [
            "1 2 3",  # valid
            "1e306 1e306 3",  # overflows only after the *1000 scale
            "1 1e306 3",  # only the end column overflows
            "3 4 3",  # valid
        ]
    )
    entries = recording_edl_plugin._parse_edl(text)
    assert entries == [
        {"start": 1000, "end": 2000, "type": 3},
        {"start": 3000, "end": 4000, "type": 3},
    ]


def test_isfinite_sanity_for_the_regression_above():
    # Documents the exact distinction the fix relies on: float() parses
    # nan/inf successfully (no ValueError), only the later round()/int()
    # conversion would raise -- isfinite() catches both before that.
    assert not math.isfinite(float("nan"))
    assert not math.isfinite(float("inf"))


# ---------------------------------------------------------------------
# _edl_path_for
# ---------------------------------------------------------------------


def test_edl_path_for_resolves_a_normal_filename():
    cp = {"comskip": {"edl": "recording.edl"}, "file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) == Path("/data/recordings/recording.edl")


def test_edl_path_for_none_when_edl_filename_missing():
    cp = {"file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_none_when_file_path_missing():
    cp = {"comskip": {"edl": "recording.edl"}}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_none_when_comskip_key_absent():
    cp = {"file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_none_when_cut_mode_already_deleted_the_file_the_real_bug_this_fixes():
    """Dispatcharr's own default "cut" mode os.remove()s the .edl file
    right after a successful cut, but still leaves "edl" pointing at
    that now-deleted filename -- "segments_kept" is only ever set by
    that same branch, confirmed against Dispatcharr's own real upstream
    source, so it's the real distinguishing signal, not a "mode" key
    (the "mark" branch sets mode="mark" but never segments_kept)."""
    cp = {
        "comskip": {"status": "completed", "edl": "recording.edl", "segments_kept": 3, "commercials": 2},
        "file_path": "/data/recordings/show.mkv",
    }
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_still_resolves_mark_mode_which_never_deletes_the_file():
    cp = {
        "comskip": {"status": "completed", "mode": "mark", "edl": "recording.edl", "commercials": 2},
        "file_path": "/data/recordings/show.mkv",
    }
    assert recording_edl_plugin._edl_path_for(cp) == Path("/data/recordings/recording.edl")


def test_edl_path_for_still_resolves_the_no_commercials_skipped_branch():
    cp = {
        "comskip": {"status": "completed", "skipped": True, "edl": "recording.edl"},
        "file_path": "/data/recordings/show.mkv",
    }
    assert recording_edl_plugin._edl_path_for(cp) == Path("/data/recordings/recording.edl")


def test_edl_path_for_rejects_an_absolute_edl_filename():
    cp = {"comskip": {"edl": "/etc/passwd"}, "file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_rejects_a_filename_with_a_directory_separator():
    cp = {"comskip": {"edl": "../secrets/leak.edl"}, "file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_rejects_a_bare_dotdot_filename():
    # Path(file_path).parent / ".." would escape one directory level up --
    # not caught by the directory-separator check alone.
    cp = {"comskip": {"edl": ".."}, "file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_none_when_comskip_is_not_a_dict():
    """Regression test for a real crash: comskip.get() on a non-dict
    value (e.g. a string, from a corrupted or unexpectedly-shaped
    custom_properties) raised AttributeError instead of this function's
    usual "nothing legitimate to fetch" None."""
    cp = {"comskip": "not-a-dict", "file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_none_when_edl_filename_is_not_a_string():
    """Regression test for a real crash: Path(edl_filename) on a non-str
    value (e.g. an int) raised TypeError instead of None."""
    cp = {"comskip": {"edl": 12345}, "file_path": "/data/recordings/show.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_edl_path_for_none_when_custom_properties_itself_is_not_a_dict():
    """Regression test for a real crash: custom_properties.get() on a
    non-dict value (e.g. a stray list/string) raised AttributeError
    before even reaching the comskip check, crashing get_edl entirely
    instead of reporting "no entries"."""
    assert recording_edl_plugin._edl_path_for("not-a-dict") is None
    assert recording_edl_plugin._edl_path_for(["not", "a", "dict"]) is None


def test_edl_path_for_none_when_file_path_is_not_a_string():
    cp = {"comskip": {"edl": "recording.edl"}, "file_path": 12345}
    assert recording_edl_plugin._edl_path_for(cp) is None


# ---------------------------------------------------------------------
# _edl_result
# ---------------------------------------------------------------------


def test_edl_result_no_entries():
    result = recording_edl_plugin._edl_result([])
    assert result == {"status": "ok", "message": "No EDL entries found", "entries": []}


def test_edl_result_singular_entry():
    entries = [{"start": 0, "end": 1000, "type": 3}]
    result = recording_edl_plugin._edl_result(entries)
    assert result["message"] == "1 EDL entry found"
    assert result["entries"] == entries


def test_edl_result_plural_entries():
    entries = [{"start": 0, "end": 1000, "type": 3}, {"start": 2000, "end": 3000, "type": 3}]
    result = recording_edl_plugin._edl_result(entries)
    assert result["message"] == "2 EDL entries found"


# ---------------------------------------------------------------------
# _sidecar_base_name
# ---------------------------------------------------------------------


def test_sidecar_base_name_edl():
    assert recording_edl_plugin._sidecar_base_name("MyShow.S01E01.edl") == "MyShow.S01E01"


def test_sidecar_base_name_logo_compound_suffix():
    # Can't use Path.stem/splitext here -- that would only strip the
    # trailing .txt and leave ".logo" attached.
    assert recording_edl_plugin._sidecar_base_name("MyShow.S01E01.logo.txt") == "MyShow.S01E01"


def test_sidecar_base_name_not_a_sidecar():
    assert recording_edl_plugin._sidecar_base_name("MyShow.S01E01.mkv") is None
    assert recording_edl_plugin._sidecar_base_name("MyShow.S01E01.txt") is None


# ---------------------------------------------------------------------
# _is_under_dotted_dir
# ---------------------------------------------------------------------


def test_is_under_dotted_dir_false_for_plain_path():
    root = Path("/data/recordings/TV_Shows")
    path = root / "MyShow" / "S01" / "ep.mkv"
    assert recording_edl_plugin._is_under_dotted_dir(path, root) is False


def test_is_under_dotted_dir_true_for_dotted_component():
    root = Path("/data/recordings")
    path = root / ".dvr_42_hls" / "segment0.ts"
    assert recording_edl_plugin._is_under_dotted_dir(path, root) is True


def test_is_under_dotted_dir_true_for_deeply_nested_dotted_component():
    root = Path("/data/recordings")
    path = root / "TV_Shows" / ".hidden" / "ep.mkv"
    assert recording_edl_plugin._is_under_dotted_dir(path, root) is True


def test_is_under_dotted_dir_false_for_root_itself():
    root = Path("/data/recordings/TV_Shows")
    assert recording_edl_plugin._is_under_dotted_dir(root, root) is False


# ---------------------------------------------------------------------
# _resolve_scan_root / _dedupe_scan_roots
# ---------------------------------------------------------------------


def test_resolve_scan_root_relative_template():
    root = recording_edl_plugin._resolve_scan_root("TV_Shows/{show}/S{season:02d}E{episode:02d}.mkv")
    assert root == Path("/data/recordings/TV_Shows")


def test_resolve_scan_root_absolute_template():
    root = recording_edl_plugin._resolve_scan_root("/mnt/media/TV/{show}/{episode}.mkv")
    assert root == Path("/mnt/media/TV")


def test_resolve_scan_root_empty_template():
    assert recording_edl_plugin._resolve_scan_root("") is None
    assert recording_edl_plugin._resolve_scan_root(None) is None


def test_resolve_scan_root_bare_root_excluded_regression():
    """The exact 2026-09-05 incident this project's own docs document: a
    template with no subdirectory component at all (e.g. a bare
    "{show}.mkv") resolves to _RECORDINGS_ROOT itself, which must be
    excluded -- an earlier version missed this exact case even after
    supposedly no longer adding the bare root, and the resulting
    empty-directory sweep wiped Dispatcharr's own .dvr_*_hls/.timeshift
    directories living at that top level."""
    assert recording_edl_plugin._resolve_scan_root("{show}.mkv") is None


def test_resolve_scan_root_rejects_a_dotdot_escape_to_the_bare_root():
    """Real, confirmed path-traversal-class gap: Path() never collapses
    "..", so this used to resolve to Path("/data/recordings/TV_Shows/..")
    -- unequal to _RECORDINGS_ROOT as a plain Path comparison, even
    though it's exactly _RECORDINGS_ROOT itself once normalized. Same
    severity as the bare-root-exclusion regression above."""
    assert recording_edl_plugin._resolve_scan_root("TV_Shows/../{show}/S{season:02d}E{episode:02d}.mkv") is None


def test_resolve_scan_root_rejects_a_dotdot_escape_above_the_root():
    """A template with more ".." segments than path components escapes
    to an ancestor of _RECORDINGS_ROOT (here, /data itself) -- the scrub
    walking that would be far more destructive than even the bare-root
    case, since /data plausibly holds unrelated data outside Dispatcharr
    entirely."""
    assert recording_edl_plugin._resolve_scan_root("../{show}.mkv") is None


def test_resolve_scan_root_rejects_a_doubled_leading_slash():
    """os.path.normpath() alone deliberately leaves a doubled leading
    "//" untouched (a POSIX special case this project has no reason to
    honor) -- "//data/recordings" must still be recognized as
    _RECORDINGS_ROOT itself and excluded, not treated as some other,
    unrelated root."""
    assert recording_edl_plugin._resolve_scan_root("//data/recordings/{show}.mkv") is None


def test_resolve_scan_root_dotdot_that_stays_within_the_root_is_still_accepted():
    """Not every ".." is an escape -- one that nets out to a genuine
    subdirectory of _RECORDINGS_ROOT once normalized is still a
    perfectly valid scan root, just like any other real subdirectory."""
    root = recording_edl_plugin._resolve_scan_root("TV_Shows/Sub/../../Escaped/{show}.mkv")
    assert root == Path("/data/recordings/Escaped")


def test_resolve_scan_root_absolute_template_outside_the_root_is_still_accepted():
    """The path-traversal fix must not reject a legitimate absolute
    template pointing entirely outside _RECORDINGS_ROOT -- that's an
    already-supported, deliberate configuration (see
    test_resolve_scan_root_absolute_template above), not something to
    guard against. Only _RECORDINGS_ROOT itself, or an ancestor of it,
    is refused."""
    assert recording_edl_plugin._resolve_scan_root("/mnt/media/{show}.mkv") == Path("/mnt/media")


def test_dedupe_scan_roots_drops_descendants_of_a_kept_ancestor():
    roots = {Path("/data/recordings/TV_Shows"), Path("/data/recordings/TV_Shows/Drama")}
    assert recording_edl_plugin._dedupe_scan_roots(roots) == [Path("/data/recordings/TV_Shows")]


def test_dedupe_scan_roots_keeps_unrelated_roots():
    roots = {Path("/data/recordings/TV_Shows"), Path("/data/recordings/Movies")}
    result = recording_edl_plugin._dedupe_scan_roots(roots)
    assert set(result) == roots


# ---------------------------------------------------------------------
# _prune_empty_directories
# ---------------------------------------------------------------------


def test_prune_empty_directories_removes_nested_empty_dirs(tmp_path):
    empty_leaf = tmp_path / "Show" / "S01"
    empty_leaf.mkdir(parents=True)

    removed, errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=None)

    assert errors == []
    assert str(empty_leaf) in removed
    assert str(tmp_path / "Show") in removed  # emptied by the leaf's own removal, same pass
    assert not empty_leaf.exists()
    assert not (tmp_path / "Show").exists()
    assert tmp_path.exists()  # root itself is never removed


def test_prune_empty_directories_keeps_non_empty_dirs(tmp_path):
    show_dir = tmp_path / "Show"
    show_dir.mkdir()
    (show_dir / "episode.mkv").write_text("not empty")

    removed, errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=None)

    assert removed == []
    assert errors == []
    assert show_dir.exists()


def test_prune_empty_directories_skips_dotted_dirs(tmp_path):
    """Regression guard for the same 2026-09-05 incident class: an empty
    directory nested under a dot-prefixed directory must never be
    touched, even though it's otherwise indistinguishable from a
    legitimate empty show/season folder."""
    dotted_empty = tmp_path / ".dvr_42_hls" / "empty_subdir"
    dotted_empty.mkdir(parents=True)

    removed, errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=None)

    assert removed == []
    assert errors == []
    assert dotted_empty.exists()


def test_prune_empty_directories_logs_each_removal(tmp_path):
    (tmp_path / "Show" / "S01").mkdir(parents=True)
    messages = []

    class _FakeLogger:
        def info(self, fmt, *args):
            messages.append(fmt % args)

    removed, _errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=_FakeLogger())
    assert len(messages) == len(removed) == 2


# ---------------------------------------------------------------------
# _scrub_orphaned_recording_sidecars (via monkeypatching the Django-
# dependent _dvr_sidecar_scan_roots, not the pure functions above)
# ---------------------------------------------------------------------


def test_scrub_orphaned_recording_sidecars_removes_true_orphans_only(tmp_path, monkeypatch):
    show_dir = tmp_path / "Show" / "S01"
    show_dir.mkdir(parents=True)
    # A real recording: video + matching sidecars -- must survive.
    (show_dir / "ep1.mkv").write_text("video")
    (show_dir / "ep1.edl").write_text("1 2 3")
    (show_dir / "ep1.logo.txt").write_text("logo")
    # An orphaned recording: sidecars with no matching video -- deleted
    # video, comskip leftovers never cleaned up by Dispatcharr's own
    # RecordingViewSet.destroy() (see this module's docstring).
    (show_dir / "ep2.edl").write_text("1 2 3")
    (show_dir / "ep2.logo.txt").write_text("logo")

    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path])

    removed_files, removed_dirs, errors = recording_edl_plugin._scrub_orphaned_recording_sidecars(logger=None)

    assert errors == []
    assert sorted(Path(f).name for f in removed_files) == ["ep2.edl", "ep2.logo.txt"]
    assert (show_dir / "ep1.mkv").exists()
    assert (show_dir / "ep1.edl").exists()
    assert (show_dir / "ep1.logo.txt").exists()
    assert not (show_dir / "ep2.edl").exists()
    assert not (show_dir / "ep2.logo.txt").exists()
    assert removed_dirs == []  # show_dir still has ep1's files, nothing emptied


def test_scrub_orphaned_recording_sidecars_prunes_emptied_directory(tmp_path, monkeypatch):
    show_dir = tmp_path / "Show" / "S01"
    show_dir.mkdir(parents=True)
    (show_dir / "ep1.edl").write_text("1 2 3")  # the only file here -- orphaned

    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path])

    removed_files, removed_dirs, errors = recording_edl_plugin._scrub_orphaned_recording_sidecars(logger=None)

    assert errors == []
    assert len(removed_files) == 1
    assert str(show_dir) in removed_dirs
    assert not show_dir.exists()


def test_scrub_orphaned_recording_sidecars_never_touches_dotted_dirs(tmp_path, monkeypatch):
    """The exact incident this project's docs describe: a .dvr_*_hls
    staging directory (or any dot-prefixed directory) living under a
    scan root must never be walked into or have its contents removed,
    even if a file inside it happens to look like an orphaned sidecar."""
    dotted_dir = tmp_path / ".dvr_42_hls"
    dotted_dir.mkdir()
    (dotted_dir / "leftover.edl").write_text("1 2 3")

    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path])

    removed_files, removed_dirs, errors = recording_edl_plugin._scrub_orphaned_recording_sidecars(logger=None)

    assert removed_files == []
    assert removed_dirs == []
    assert errors == []
    assert (dotted_dir / "leftover.edl").exists()


# ---------------------------------------------------------------------
# _hls_staging_dir_recording_id
# ---------------------------------------------------------------------


def test_hls_staging_dir_recording_id_matches_a_real_staging_dir_name():
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_42_hls") == 42


def test_hls_staging_dir_recording_id_none_for_a_non_numeric_id():
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_abc_hls") is None


def test_hls_staging_dir_recording_id_none_for_an_unrelated_dotfile():
    assert recording_edl_plugin._hls_staging_dir_recording_id(".timeshift") is None


def test_hls_staging_dir_recording_id_none_for_a_plain_directory_name():
    assert recording_edl_plugin._hls_staging_dir_recording_id("TV_Shows") is None


def test_hls_staging_dir_recording_id_none_for_unicode_digits():
    # Python's str-pattern \d matches any Unicode decimal digit, and
    # int() happily parses one -- the old regex read this as recording 3.
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_٣_hls") is None


def test_hls_staging_dir_recording_id_none_for_a_zero_padded_id():
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_0005_hls") is None


def test_hls_staging_dir_recording_id_none_for_a_trailing_newline():
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_5_hls\n") is None


def test_hls_staging_dir_recording_id_accepts_a_multi_digit_id():
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_1050_hls") == 1050


def test_hls_staging_dir_recording_id_none_for_a_bare_zero_padded_zero():
    # "0" itself round-trips, "00" doesn't.
    assert recording_edl_plugin._hls_staging_dir_recording_id(".dvr_00_hls") is None


# ---------------------------------------------------------------------
# _count_ts_segments
# ---------------------------------------------------------------------


def test_count_ts_segments_counts_only_ts_files(tmp_path):
    (tmp_path / "seg_00001.ts").write_text("")
    (tmp_path / "seg_00002.ts").write_text("")
    (tmp_path / "index.m3u8").write_text("")
    (tmp_path / "subdir").mkdir()

    assert recording_edl_plugin._count_ts_segments(tmp_path) == 2


def test_count_ts_segments_zero_for_an_empty_directory(tmp_path):
    assert recording_edl_plugin._count_ts_segments(tmp_path) == 0


def test_count_ts_segments_none_when_the_directory_does_not_exist(tmp_path):
    assert recording_edl_plugin._count_ts_segments(tmp_path / "gone") is None


# ---------------------------------------------------------------------
# _classify_hls_dir_info
# ---------------------------------------------------------------------


def test_classify_hls_dir_info_orphaned_when_no_recording_row():
    info = recording_edl_plugin._classify_hls_dir_info(
        Path("/data/recordings/.dvr_42_hls"), 42, recording_exists=False, custom_properties=None, segment_count=3
    )

    assert info["classification"] == "orphaned"
    assert info["recording_exists"] is False
    assert "42" in info["detail"]
    assert info["segment_count"] == 3


def test_classify_hls_dir_info_active_when_still_recording():
    hls_dir = Path("/data/recordings/.dvr_1_hls")
    info = recording_edl_plugin._classify_hls_dir_info(
        hls_dir,
        1,
        recording_exists=True,
        custom_properties={"_hls_dir": str(hls_dir), "status": "recording"},
        segment_count=10,
    )

    assert info["classification"] == "active"


def test_classify_hls_dir_info_preserved_failure_when_remux_failed():
    """The real "never delete the only surviving copy" incident this
    function's own docstring documents: a failed concat/remux leaves
    Dispatcharr deliberately keeping the directory -- must classify as
    preserved_failure, never orphaned or plain referenced, regardless of
    what status says."""
    hls_dir = Path("/data/recordings/.dvr_2_hls")
    info = recording_edl_plugin._classify_hls_dir_info(
        hls_dir,
        2,
        recording_exists=True,
        custom_properties={"_hls_dir": str(hls_dir), "status": "stopped", "remux_success": False},
        segment_count=5,
    )

    assert info["classification"] == "preserved_failure"


def test_classify_hls_dir_info_referenced_when_recording_matches_but_not_active_or_failed():
    hls_dir = Path("/data/recordings/.dvr_3_hls")
    info = recording_edl_plugin._classify_hls_dir_info(
        hls_dir,
        3,
        recording_exists=True,
        custom_properties={"_hls_dir": str(hls_dir), "status": "completed"},
        segment_count=0,
    )

    assert info["classification"] == "referenced"
    assert "completed" in info["detail"]


def test_classify_hls_dir_info_referenced_when_recording_points_elsewhere():
    """A Recording row exists but its own _hls_dir doesn't match this
    path -- needs manual review rather than being assumed safe to delete
    just because it superficially looks unreferenced."""
    hls_dir = Path("/data/recordings/.dvr_4_hls")
    info = recording_edl_plugin._classify_hls_dir_info(
        hls_dir,
        4,
        recording_exists=True,
        custom_properties={"_hls_dir": "/data/recordings/.dvr_4_hls_old", "status": "completed"},
        segment_count=0,
    )

    assert info["classification"] == "referenced"
    assert "manual review" in info["detail"]


def test_classify_hls_dir_info_referenced_when_custom_properties_missing_hls_dir():
    """A Recording row exists but never had an _hls_dir at all (e.g.
    custom_properties is None/empty) -- same "needs manual review" bucket
    as pointing elsewhere, not treated as orphaned."""
    info = recording_edl_plugin._classify_hls_dir_info(
        Path("/data/recordings/.dvr_5_hls"), 5, recording_exists=True, custom_properties=None, segment_count=0
    )

    assert info["classification"] == "referenced"


def test_classify_hls_dir_info_referenced_when_custom_properties_is_not_a_dict():
    """Regression test for a real bug: a non-dict custom_properties (e.g.
    a stray list/string from a corrupted or unexpectedly-shaped DB value)
    raised AttributeError at cp.get() instead of falling into the same
    "needs manual review" bucket a missing value already gets -- aborting
    the *entire* scan (the caller's own loop has no try/except around
    this call), not just this one directory's classification."""
    info = recording_edl_plugin._classify_hls_dir_info(
        Path("/data/recordings/.dvr_5_hls"), 5, recording_exists=True, custom_properties="not-a-dict", segment_count=0
    )

    assert info["classification"] == "referenced"


def test_classify_hls_dir_info_referenced_when_hls_dir_is_not_a_string():
    """Regression test for a real bug: a non-str/PathLike _hls_dir (e.g.
    an int) raised TypeError at Path(...) instead of falling into the
    same "needs manual review" bucket a mismatched path already gets."""
    info = recording_edl_plugin._classify_hls_dir_info(
        Path("/data/recordings/.dvr_5_hls"),
        5,
        recording_exists=True,
        custom_properties={"_hls_dir": 12345},
        segment_count=0,
    )

    assert info["classification"] == "referenced"


# ---------------------------------------------------------------------
# _delete_orphaned_dvr_hls_dirs (via monkeypatching the Django-dependent
# _list_dvr_hls_staging_dirs, not the pure classification above) -- this
# is the actual destructive filter; the run()-level tests further below
# only monkeypatch this function itself away, so they never exercise its
# real safety invariant (only "orphaned" ever gets deleted) at all.
# ---------------------------------------------------------------------


def _fake_classify_from(monkeypatch, infos):
    """_delete_orphaned_dvr_hls_dirs() re-classifies each directory just before removing it; this
    answers that call from the same fake list (a path not in it is no longer a staging directory)."""
    by_path = {i["path"]: i for i in infos}
    monkeypatch.setattr(recording_edl_plugin, "_classify_dvr_hls_dir", lambda p: by_path.get(str(p)))


def test_delete_orphaned_dvr_hls_dirs_removes_orphaned_only(tmp_path, monkeypatch):
    dirs = {}
    for classification in ("active", "preserved_failure", "referenced", "orphaned"):
        d = tmp_path / f".dvr_{classification}_hls"
        d.mkdir()
        (d / "segment0.ts").write_text("data")
        dirs[classification] = d

    fake_infos = [
        {
            "path": str(path),
            "classification": classification,
            "recording_id": i,
            "recording_exists": True,
            "detail": "",
            "segment_count": 1,
        }
        for i, (classification, path) in enumerate(dirs.items())
    ]
    monkeypatch.setattr(recording_edl_plugin, "_list_dvr_hls_staging_dirs", lambda: fake_infos)
    _fake_classify_from(monkeypatch, fake_infos)

    removed, errors = recording_edl_plugin._delete_orphaned_dvr_hls_dirs(logger=None)

    assert errors == []
    assert removed == [str(dirs["orphaned"])]
    assert not dirs["orphaned"].exists()
    # Never touch anything else, regardless of category.
    assert dirs["active"].exists()
    assert dirs["preserved_failure"].exists()
    assert dirs["referenced"].exists()


def test_delete_orphaned_dvr_hls_dirs_no_orphans_removes_nothing(tmp_path, monkeypatch):
    active_dir = tmp_path / ".dvr_1_hls"
    active_dir.mkdir()
    fake_infos = [
        {
            "path": str(active_dir),
            "classification": "active",
            "recording_id": 1,
            "recording_exists": True,
            "detail": "",
            "segment_count": 0,
        }
    ]
    monkeypatch.setattr(recording_edl_plugin, "_list_dvr_hls_staging_dirs", lambda: fake_infos)
    _fake_classify_from(monkeypatch, fake_infos)

    removed, errors = recording_edl_plugin._delete_orphaned_dvr_hls_dirs(logger=None)

    assert removed == []
    assert errors == []
    assert active_dir.exists()


def test_delete_orphaned_dvr_hls_dirs_captures_error_and_continues(tmp_path, monkeypatch):
    """One orphaned directory's deletion raising OSError (e.g. a
    permission error, or the directory already having been removed by
    something else between listing and acting) must be recorded in
    errors, not raised -- and must not stop the remaining orphaned
    directories in the same run from being removed."""
    good_dir = tmp_path / ".dvr_1_hls"
    good_dir.mkdir()
    bad_dir = tmp_path / ".dvr_2_hls"
    bad_dir.mkdir()
    fake_infos = [
        {
            "path": str(bad_dir),
            "classification": "orphaned",
            "recording_id": 2,
            "recording_exists": False,
            "detail": "",
            "segment_count": 0,
        },
        {
            "path": str(good_dir),
            "classification": "orphaned",
            "recording_id": 1,
            "recording_exists": False,
            "detail": "",
            "segment_count": 0,
        },
    ]
    monkeypatch.setattr(recording_edl_plugin, "_list_dvr_hls_staging_dirs", lambda: fake_infos)
    _fake_classify_from(monkeypatch, fake_infos)

    real_rmtree = recording_edl_plugin.shutil.rmtree

    def flaky_rmtree(path, *args, **kwargs):
        if str(path) == str(bad_dir):
            raise OSError("Permission denied")
        return real_rmtree(path, *args, **kwargs)

    monkeypatch.setattr(recording_edl_plugin.shutil, "rmtree", flaky_rmtree)

    removed, errors = recording_edl_plugin._delete_orphaned_dvr_hls_dirs(logger=None)

    assert removed == [str(good_dir)]
    assert not good_dir.exists()
    assert len(errors) == 1
    assert str(bad_dir) in errors[0]
    assert bad_dir.exists()  # the failed rmtree left it in place


# ---------------------------------------------------------------------
# Plugin.run() dispatch and message formatting -- only action branches
# (or code paths within one) that don't need Django, either because they
# never touch it at all or because the one Django-dependent call can be
# monkeypatched the same way as above.
# ---------------------------------------------------------------------


def test_run_scrub_orphaned_sidecars_no_orphans_message(tmp_path, monkeypatch):
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path])

    result = recording_edl_plugin.Plugin().run("scrub_orphaned_sidecars", {}, {"logger": None, "settings": {}})

    assert result == {
        "status": "ok",
        "message": "No orphaned .edl/.logo.txt files or empty directories found",
        "removed_files": [],
        "removed_directories": [],
        "errors": [],
    }


def test_run_scrub_orphaned_sidecars_reports_counts(tmp_path, monkeypatch):
    show_dir = tmp_path / "Show"
    show_dir.mkdir()
    (show_dir / "ep.edl").write_text("1 2 3")  # orphaned -- no matching video

    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path])

    result = recording_edl_plugin.Plugin().run("scrub_orphaned_sidecars", {}, {"logger": None, "settings": {}})

    assert result["status"] == "ok"
    assert result["message"] == "Removed 1 orphaned file(s), 1 empty directory(s)"
    assert result["removed_files"] == [str(show_dir / "ep.edl")]
    assert result["removed_directories"] == [str(show_dir)]
    assert result["errors"] == []


def test_run_list_dvr_hls_staging_dirs_no_dirs_message(monkeypatch):
    monkeypatch.setattr(recording_edl_plugin, "_list_dvr_hls_staging_dirs", lambda: [])

    result = recording_edl_plugin.Plugin().run("list_dvr_hls_staging_dirs", {}, {"logger": None, "settings": {}})

    assert result == {
        "status": "ok",
        "message": "No .dvr_*_hls staging directories found",
        "directories": [],
    }


def test_run_list_dvr_hls_staging_dirs_summarizes_classifications(monkeypatch):
    fake_dirs = [
        {"path": "/data/recordings/.dvr_1_hls", "classification": "orphaned", "segment_count": 3},
        {"path": "/data/recordings/.dvr_2_hls", "classification": "active", "segment_count": 7},
    ]
    monkeypatch.setattr(recording_edl_plugin, "_list_dvr_hls_staging_dirs", lambda: fake_dirs)

    result = recording_edl_plugin.Plugin().run("list_dvr_hls_staging_dirs", {}, {"logger": None, "settings": {}})

    assert result["status"] == "ok"
    assert "2 .dvr_*_hls dir(s) found" in result["message"]
    assert "1 orphaned" in result["message"]
    assert "1 active" in result["message"]
    assert ".dvr_1_hls (orphaned, 3 segs)" in result["message"]
    assert ".dvr_2_hls (active, 7 segs)" in result["message"]
    assert result["directories"] == fake_dirs


def test_run_delete_orphaned_dvr_hls_dirs_no_orphans_message(monkeypatch):
    monkeypatch.setattr(recording_edl_plugin, "_delete_orphaned_dvr_hls_dirs", lambda logger: ([], []))

    result = recording_edl_plugin.Plugin().run("delete_orphaned_dvr_hls_dirs", {}, {"logger": None, "settings": {}})

    assert result == {
        "status": "ok",
        "message": "No orphaned .dvr_*_hls directories found",
        "removed": [],
        "errors": [],
    }


def test_run_delete_orphaned_dvr_hls_dirs_singular_plural_message(monkeypatch):
    monkeypatch.setattr(recording_edl_plugin, "_delete_orphaned_dvr_hls_dirs", lambda logger: (["/a"], []))
    result = recording_edl_plugin.Plugin().run("delete_orphaned_dvr_hls_dirs", {}, {"logger": None, "settings": {}})
    assert result["message"] == "Removed 1 orphaned .dvr_*_hls directory"

    monkeypatch.setattr(recording_edl_plugin, "_delete_orphaned_dvr_hls_dirs", lambda logger: (["/a", "/b"], []))
    result = recording_edl_plugin.Plugin().run("delete_orphaned_dvr_hls_dirs", {}, {"logger": None, "settings": {}})
    assert result["message"] == "Removed 2 orphaned .dvr_*_hls directories"


def test_run_delete_orphaned_dvr_hls_dirs_appends_error_count(monkeypatch):
    monkeypatch.setattr(recording_edl_plugin, "_delete_orphaned_dvr_hls_dirs", lambda logger: (["/a"], ["some error"]))

    result = recording_edl_plugin.Plugin().run("delete_orphaned_dvr_hls_dirs", {}, {"logger": None, "settings": {}})

    assert result["message"] == "Removed 1 orphaned .dvr_*_hls directory, 1 error(s) (see plugin log)"


def test_run_unknown_action_returns_error():
    result = recording_edl_plugin.Plugin().run("not_a_real_action", {}, {"logger": None, "settings": {}})

    assert result == {"status": "error", "message": "Unknown action: not_a_real_action"}


def test_run_get_edl_requires_recording_id():
    """The recording_id-missing early return happens before the deferred
    `from apps.channels.models import Recording` import -- Django-free."""
    result = recording_edl_plugin.Plugin().run("get_edl", {}, {"logger": None, "settings": {}})

    assert result["status"] == "error"
    assert "recording_id is required" in result["message"]


# ---------------------------------------------------------------------
# _dvr_hls_staging_scan_roots / _list_dvr_hls_staging_dirs -- a staging
# directory next to a recording in an absolute-template library
# ---------------------------------------------------------------------


def _fake_classify(path):
    return {"path": str(path), "classification": "orphaned", "recording_id": 1}


def test_staging_scan_roots_include_the_default_root_and_every_template_root(monkeypatch):
    monkeypatch.setattr(
        recording_edl_plugin,
        "_dvr_sidecar_scan_roots",
        lambda: [Path("/mnt/media/TV"), Path("/mnt/media/Movies")],
    )

    roots = recording_edl_plugin._dvr_hls_staging_scan_roots()

    assert Path("/data/recordings") in roots
    assert Path("/mnt/media/TV") in roots
    assert Path("/mnt/media/Movies") in roots


def test_staging_scan_roots_drop_a_template_root_already_inside_the_default_root(monkeypatch):
    """A relative template resolves under /data/recordings; walking it separately would list every
    staging directory in it twice."""
    monkeypatch.setattr(
        recording_edl_plugin,
        "_dvr_sidecar_scan_roots",
        lambda: [Path("/data/recordings/TV_Shows"), Path("/mnt/media/TV")],
    )

    assert recording_edl_plugin._dvr_hls_staging_scan_roots() == [Path("/data/recordings"), Path("/mnt/media/TV")]


def test_staging_scan_roots_fall_back_to_the_default_root_when_the_templates_cant_be_read(monkeypatch):
    def broken():
        raise RuntimeError("django not ready")

    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", broken)

    assert recording_edl_plugin._dvr_hls_staging_scan_roots() == [Path("/data/recordings")]


def test_list_staging_dirs_finds_one_in_an_absolute_template_library(tmp_path, monkeypatch):
    """The bug from docs/OPEN_ITEMS.md: Dispatcharr puts .dvr_<id>_hls beside the final file, which for an
    absolute template is outside /data/recordings, where this scan never looked."""
    default_root = tmp_path / "recordings"
    external = tmp_path / "media" / "TV"
    (default_root / "TV_Shows" / "Show A").mkdir(parents=True)
    (default_root / "TV_Shows" / "Show A" / ".dvr_7_hls").mkdir()
    (external / "Show B" / "Season 01").mkdir(parents=True)
    (external / "Show B" / "Season 01" / ".dvr_9_hls").mkdir()
    monkeypatch.setattr(recording_edl_plugin, "_RECORDINGS_ROOT", default_root)
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [external])
    monkeypatch.setattr(recording_edl_plugin, "_classify_dvr_hls_dir", _fake_classify)

    found = [Path(info["path"]) for info in recording_edl_plugin._list_dvr_hls_staging_dirs()]

    assert sorted(found) == sorted(
        [
            default_root / "TV_Shows" / "Show A" / ".dvr_7_hls",
            external / "Show B" / "Season 01" / ".dvr_9_hls",
        ]
    )


def test_list_staging_dirs_lists_each_directory_once(tmp_path, monkeypatch):
    default_root = tmp_path / "recordings"
    (default_root / "TV_Shows").mkdir(parents=True)
    (default_root / "TV_Shows" / ".dvr_3_hls").mkdir()
    monkeypatch.setattr(recording_edl_plugin, "_RECORDINGS_ROOT", default_root)
    # A template root inside the default root, the common case.
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [default_root / "TV_Shows"])
    monkeypatch.setattr(recording_edl_plugin, "_classify_dvr_hls_dir", _fake_classify)

    assert len(recording_edl_plugin._list_dvr_hls_staging_dirs()) == 1


def test_list_staging_dirs_skips_a_root_that_does_not_exist(tmp_path, monkeypatch):
    """An unmounted external library must not break the scan of the others."""
    default_root = tmp_path / "recordings"
    (default_root / ".dvr_4_hls").mkdir(parents=True)
    monkeypatch.setattr(recording_edl_plugin, "_RECORDINGS_ROOT", default_root)
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path / "not-mounted"])
    monkeypatch.setattr(recording_edl_plugin, "_classify_dvr_hls_dir", _fake_classify)

    assert [Path(i["path"]) for i in recording_edl_plugin._list_dvr_hls_staging_dirs()] == [default_root / ".dvr_4_hls"]


def test_list_staging_dirs_only_returns_exactly_named_staging_directories(tmp_path, monkeypatch):
    """Widening the roots is safe only because of this exact-name filter: other directories are skipped."""
    external = tmp_path / "media"
    (external / "Show").mkdir(parents=True)
    (external / ".dvr_5_hls").mkdir()
    (external / "Show" / ".dvr_notanumber_hls").mkdir()
    (external / "Show" / "dvr_6_hls").mkdir()
    (external / "Show" / ".dvr_7_hls.bak").mkdir()
    monkeypatch.setattr(recording_edl_plugin, "_RECORDINGS_ROOT", tmp_path / "recordings")
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [external])
    # Use the real classifier's name check, stubbing only its Django lookup.
    monkeypatch.setattr(
        recording_edl_plugin,
        "_classify_dvr_hls_dir",
        lambda p: _fake_classify(p) if recording_edl_plugin._hls_staging_dir_recording_id(p.name) else None,
    )

    found = [Path(i["path"]).name for i in recording_edl_plugin._list_dvr_hls_staging_dirs()]

    assert found == [".dvr_5_hls"]


# ---------------------------------------------------------------------
# _staging_dirs_message
# ---------------------------------------------------------------------


def test_staging_dirs_message_for_no_directories():
    assert recording_edl_plugin._staging_dirs_message([]) == "No .dvr_*_hls staging directories found"


def test_staging_dirs_message_counts_classifications_in_first_seen_order_and_names_each_directory():
    dirs = [
        {"path": "/data/recordings/.dvr_7_hls", "classification": "orphaned", "segment_count": 12},
        {"path": "/data/recordings/TV/.dvr_9_hls", "classification": "active", "segment_count": 3},
        {"path": "/data/recordings/.dvr_8_hls", "classification": "orphaned", "segment_count": None},
    ]
    assert recording_edl_plugin._staging_dirs_message(dirs) == (
        "3 .dvr_*_hls dir(s) found (2 orphaned, 1 active): "
        ".dvr_7_hls (orphaned, 12 segs); .dvr_9_hls (active, 3 segs); .dvr_8_hls (orphaned, ? segs)"
    )


def test_delete_orphaned_dvr_hls_dirs_reclassifies_each_directory_before_removing_it(tmp_path, monkeypatch):
    """The list is built first and acted on later; a directory that has become live meanwhile (a
    recording id reused after a database reset) must survive, one that is no longer a staging
    directory is skipped, and one still orphaned goes."""
    became_live = tmp_path / ".dvr_1_hls"
    still_orphaned = tmp_path / ".dvr_2_hls"
    vanished = tmp_path / ".dvr_3_hls"
    for d in (became_live, still_orphaned, vanished):
        d.mkdir()
        (d / "seg_0.ts").write_text("x")

    def info(path, classification):
        return {
            "path": str(path),
            "classification": classification,
            "recording_id": 1,
            "recording_exists": False,
            "detail": "",
            "segment_count": 1,
        }

    stale_list = [info(became_live, "orphaned"), info(still_orphaned, "orphaned"), info(vanished, "orphaned")]
    monkeypatch.setattr(recording_edl_plugin, "_list_dvr_hls_staging_dirs", lambda: stale_list)
    fresh = {str(became_live): info(became_live, "active"), str(still_orphaned): info(still_orphaned, "orphaned")}
    monkeypatch.setattr(recording_edl_plugin, "_classify_dvr_hls_dir", lambda p: fresh.get(str(p)))

    removed, errors = recording_edl_plugin._delete_orphaned_dvr_hls_dirs(logger=None)

    assert errors == []
    assert removed == [str(still_orphaned)]
    assert became_live.exists()
    assert vanished.exists()
    assert not still_orphaned.exists()


# ---------------------------------------------------------------------
# The bounded, confined EDL read (the 2026-10-04 fifth hardening sweep)
# ---------------------------------------------------------------------


@pytest.mark.parametrize("name", ["movie.mkv", "movie.ts", "zero", "movie.edl.bak", "movie.EDL.txt", "edl", ".edl.d"])
def test_edl_path_for_only_names_an_edl_file(name):
    cp = {"comskip": {"edl": name}, "file_path": "/data/recordings/TV/movie.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


@pytest.mark.parametrize("name", ["movie.edl", "Movie.EDL", "a b.edl"])
def test_edl_path_for_accepts_an_edl_suffix_in_any_case(name):
    cp = {"comskip": {"edl": name}, "file_path": "/data/recordings/TV/movie.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) == Path("/data/recordings/TV") / name


def test_is_under_any_root_resolves_symlinks_and_dotdot(tmp_path):
    root = tmp_path / "recordings"
    root.mkdir()
    outside = tmp_path / "elsewhere"
    outside.mkdir()
    (outside / "x.edl").write_text("x")
    (root / "link").symlink_to(outside)
    assert recording_edl_plugin._is_under_any_root(root / "a" / "x.edl", [root])
    assert recording_edl_plugin._is_under_any_root(root, [root])
    assert not recording_edl_plugin._is_under_any_root(root / "link" / "x.edl", [root])  # escapes by symlink
    assert not recording_edl_plugin._is_under_any_root(root / ".." / "elsewhere" / "x.edl", [root])
    assert not recording_edl_plugin._is_under_any_root(tmp_path / "recordings2" / "x.edl", [root])  # name prefix
    assert not recording_edl_plugin._is_under_any_root(root / "a.edl", [])


def test_read_edl_text_reads_a_regular_file_up_to_the_cap(tmp_path):
    f = tmp_path / "a.edl"
    f.write_bytes(b"0 1 3\n")
    assert recording_edl_plugin._read_edl_text(f) == "0 1 3\n"
    exact = tmp_path / "exact.edl"
    exact.write_bytes(b"x" * 16)
    assert recording_edl_plugin._read_edl_text(exact, max_bytes=16) == "x" * 16
    over = tmp_path / "over.edl"
    over.write_bytes(b"x" * 17)
    with pytest.raises(OSError):
        recording_edl_plugin._read_edl_text(over, max_bytes=16)


def test_read_edl_text_refuses_anything_that_is_not_a_regular_file(tmp_path):
    with pytest.raises(OSError):
        recording_edl_plugin._read_edl_text(tmp_path)  # a directory
    fifo = tmp_path / "pipe.edl"
    os.mkfifo(fifo)
    with pytest.raises(OSError):
        recording_edl_plugin._read_edl_text(fifo)  # would block a plain open() forever
    with pytest.raises(OSError):
        recording_edl_plugin._read_edl_text(Path("/dev/zero"))  # unbounded


def test_read_edl_text_replaces_undecodable_bytes(tmp_path):
    f = tmp_path / "a.edl"
    f.write_bytes(b"0 1 3\n\xff\xfe\n")
    assert recording_edl_plugin._read_edl_text(f).startswith("0 1 3\n")


# ---------------------------------------------------------------------
# Mutation survivors from the 2026-10-04 fifth hardening sweep
# ---------------------------------------------------------------------


@pytest.mark.parametrize("falsy", [0, [], {}, "", False, None])
def test_edl_path_for_treats_any_segments_kept_key_as_a_cut_recording(falsy):
    # The cut branch sets the key; even an empty value means "comskip cut this", and its .edl is gone.
    cp = {"comskip": {"edl": "movie.edl", "segments_kept": falsy}, "file_path": "/data/recordings/TV/movie.mkv"}
    assert recording_edl_plugin._edl_path_for(cp) is None


def test_a_same_stem_directory_does_not_count_as_the_owning_recording(tmp_path, monkeypatch):
    show = tmp_path / "Show"
    show.mkdir()
    (show / "ep1.edl").write_text("1 2 3")
    (show / "ep1").mkdir()  # a directory with the sidecar's stem is not a video file
    (show / "ep1.mkv.d").mkdir()
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path])

    removed_files, _removed_dirs, errors = recording_edl_plugin._scrub_orphaned_recording_sidecars(logger=None)

    assert errors == []
    assert [Path(f).name for f in removed_files] == ["ep1.edl"]


def test_a_plain_file_named_like_a_staging_directory_is_not_listed(tmp_path, monkeypatch):
    (tmp_path / ".dvr_9_hls").write_text("not a directory")
    real = tmp_path / ".dvr_10_hls"
    real.mkdir()
    monkeypatch.setattr(recording_edl_plugin, "_dvr_hls_staging_scan_roots", lambda: [tmp_path])
    monkeypatch.setattr(
        recording_edl_plugin, "_classify_dvr_hls_dir", lambda p: {"path": str(p), "classification": "orphaned"}
    )

    listed = recording_edl_plugin._list_dvr_hls_staging_dirs()

    assert [Path(i["path"]).name for i in listed] == [".dvr_10_hls"]


def test_a_symlink_named_like_a_staging_directory_never_loses_its_target(tmp_path, monkeypatch):
    target = tmp_path / "precious"
    target.mkdir()
    (target / "keep.mkv").write_text("video")
    (tmp_path / ".dvr_11_hls").symlink_to(target, target_is_directory=True)
    monkeypatch.setattr(recording_edl_plugin, "_dvr_hls_staging_scan_roots", lambda: [tmp_path])
    monkeypatch.setattr(
        recording_edl_plugin, "_classify_dvr_hls_dir", lambda p: {"path": str(p), "classification": "orphaned"}
    )

    removed, errors = recording_edl_plugin._delete_orphaned_dvr_hls_dirs(logger=None)

    assert (target / "keep.mkv").read_text() == "video"  # rmtree refuses a symlink; the target is untouched
    assert removed == []
    assert len(errors) == 1


# ---------------------------------------------------------------------
# Mutation survivors from the 2026-10-04 sixth hardening sweep
# ---------------------------------------------------------------------


def test_an_empty_dotted_leaf_counts_as_under_a_dotted_directory(tmp_path):
    # A fresh .dvr_<id>_hls before its first segment, or an empty .timeshift, is the 2026-09-05
    # incident class: the check must cover the LEAF, not only its parents.
    root = tmp_path
    assert recording_edl_plugin._is_under_dotted_dir(root / ".dvr_1_hls", root) is True
    assert recording_edl_plugin._is_under_dotted_dir(root / "TV" / ".dvr_1_hls", root) is True
    assert recording_edl_plugin._is_under_dotted_dir(root / ".timeshift" / "x", root) is True
    assert recording_edl_plugin._is_under_dotted_dir(root / "TV" / "Show", root) is False


def test_prune_empty_directories_never_removes_an_empty_dotted_leaf(tmp_path):
    (tmp_path / "TV" / ".dvr_1_hls").mkdir(parents=True)
    (tmp_path / ".timeshift").mkdir()
    (tmp_path / "TV" / "Empty Show").mkdir()

    removed, errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=None)

    assert errors == []
    assert (tmp_path / "TV" / ".dvr_1_hls").is_dir()
    assert (tmp_path / ".timeshift").is_dir()
    assert not (tmp_path / "TV" / "Empty Show").exists()
    assert [Path(r).name for r in removed] == ["Empty Show"]


def test_parse_edl_ignores_a_single_token_line_and_reads_a_float_type():
    assert recording_edl_plugin._parse_edl("12.5\n") == []
    assert recording_edl_plugin._parse_edl("1 2 3.0\n") == [{"start": 1000, "end": 2000, "type": 3}]
    assert recording_edl_plugin._parse_edl("1 2\n")[0]["type"] == 3  # no type column: commercial break


def test_a_recording_with_a_different_hls_dir_is_referenced_even_while_recording(tmp_path):
    hls = tmp_path / ".dvr_5_hls"
    other = tmp_path / "somewhere" / ".dvr_5_hls"
    info = recording_edl_plugin._classify_hls_dir_info(hls, 5, True, {"status": "recording", "_hls_dir": str(other)}, 3)
    assert info["classification"] == "referenced"  # not "active": it is not THIS directory
    same = recording_edl_plugin._classify_hls_dir_info(hls, 5, True, {"status": "recording", "_hls_dir": str(hls)}, 3)
    assert same["classification"] == "active"


def test_dedupe_scan_roots_handles_unsorted_input_and_drops_nested_roots():
    f = recording_edl_plugin._dedupe_scan_roots
    assert f([Path("/d/x/y"), Path("/d/x")]) == [Path("/d/x")]  # the nested one listed FIRST
    assert f([Path("/d/x"), Path("/d/x/y"), Path("/d/z")]) == [Path("/d/x"), Path("/d/z")]
    assert f([Path("/d/x"), Path("/d/x")]) == [Path("/d/x")]
    assert f([]) == []


def test_resolve_scan_root_never_returns_the_working_directory():
    f = recording_edl_plugin._resolve_scan_root
    assert f("/{show}/{title}.ts") is None  # "/" is not a usable root, and must not become Path(".")
    assert f("{show}.ts") is None
    assert f("") is None
    assert f("/mnt/media/TV/{show}/{title}.ts") == Path("/mnt/media/TV")


def test_count_ts_segments_ignores_a_directory_named_like_a_segment(tmp_path):
    (tmp_path / "a.ts").write_text("x")
    (tmp_path / "b.ts").write_text("x")
    (tmp_path / "x.ts").mkdir()
    (tmp_path / "notes.txt").write_text("x")
    assert recording_edl_plugin._count_ts_segments(tmp_path) == 2
    assert recording_edl_plugin._count_ts_segments(tmp_path / "missing") is None


def test_staging_dirs_message_distinguishes_zero_segments_from_unknown():
    dirs = [
        {"path": "/d/.dvr_1_hls", "classification": "orphaned", "segment_count": 0},
        {"path": "/d/.dvr_2_hls", "classification": "active", "segment_count": None},
    ]
    message = recording_edl_plugin._staging_dirs_message(dirs)
    assert ".dvr_1_hls (orphaned, 0 segs)" in message
    assert ".dvr_2_hls (active, ? segs)" in message


# ---------------------------------------------------------------------
# Mutation survivors from the 2026-10-05 tenth hardening sweep
# ---------------------------------------------------------------------


@pytest.mark.parametrize("edl_name", ["sub/show.edl", "/data/recordings/show.edl", "../show.edl", "a/../show.edl"])
def test_edl_path_for_refuses_every_name_that_is_not_a_plain_filename(edl_name):
    # Each of the three checks must refuse on its own: a name that only a single check catches.
    custom = {"file_path": "/data/recordings/show.ts", "comskip": {"edl": edl_name}}
    assert recording_edl_plugin._edl_path_for(custom) is None


def test_is_under_any_root_treats_a_path_with_a_nul_byte_as_outside():
    # os.path.realpath() raises ValueError for an embedded NUL; reading the path would too, and
    # get_edl only catches OSError, so it must be refused here as "not under any root".
    assert (
        recording_edl_plugin._is_under_any_root(Path("/data/recordings/a\0b.edl"), [Path("/data/recordings")]) is False
    )
    # A root with a NUL is skipped, and a good root after it is still consulted.
    assert (
        recording_edl_plugin._is_under_any_root(
            Path("/data/recordings/show.edl"), ["/bad\0root", Path("/data/recordings")]
        )
        is True
    )


# ---------------------------------------------------------------------
# The 2026-10-05 twelfth hardening sweep: one bad directory or root must not stop the rest
# ---------------------------------------------------------------------


def test_prune_empty_directories_goes_on_past_a_non_empty_directory(tmp_path):
    deep = tmp_path / "a" / "b" / "c"
    deep.mkdir(parents=True)
    (deep / "keep.mkv").write_text("video")  # the deepest directory, sorted first, is not empty
    (tmp_path / "z").mkdir()  # a shallower empty sibling, reached later
    removed, errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=None)
    assert errors == []
    assert removed == [str(tmp_path / "z")]
    assert deep.exists()


def test_prune_empty_directories_goes_on_past_a_directory_that_cannot_be_listed_or_removed(tmp_path, monkeypatch):
    unlistable = tmp_path / "a" / "b" / "c"
    unremovable = tmp_path / "d" / "e" / "f"
    for d in (unlistable, unremovable, tmp_path / "z"):
        d.mkdir(parents=True)
    real_iterdir, real_rmdir = Path.iterdir, Path.rmdir

    def iterdir(self):
        if self == unlistable:
            raise PermissionError("no")
        return real_iterdir(self)

    def rmdir(self):
        if self == unremovable:
            raise PermissionError("no")
        return real_rmdir(self)

    monkeypatch.setattr(Path, "iterdir", iterdir)
    monkeypatch.setattr(Path, "rmdir", rmdir)
    removed, errors = recording_edl_plugin._prune_empty_directories(tmp_path, logger=None)
    assert str(tmp_path / "z") in removed
    assert not (tmp_path / "z").exists()
    assert len(errors) == 2 and any("c" in e for e in errors) and any("f" in e for e in errors)


def test_scrub_orphaned_recording_sidecars_goes_on_past_a_scan_root_that_does_not_exist(tmp_path, monkeypatch):
    real = tmp_path / "real"
    real.mkdir()
    (real / "ep.edl").write_text("1 2 3")  # an orphaned sidecar in the root after the missing one
    monkeypatch.setattr(recording_edl_plugin, "_dvr_sidecar_scan_roots", lambda: [tmp_path / "missing", real])
    removed_files, _removed_dirs, errors = recording_edl_plugin._scrub_orphaned_recording_sidecars(logger=None)
    assert errors == []
    assert [Path(f).name for f in removed_files] == ["ep.edl"]
