"""Unit tests for dispatcharr-plugin/recording_edl/plugin.py.

Loaded by explicit file path with a unique synthetic module name (not a
bare `import plugin`) since timeshift_buffer's own plugin.py shares the
same filename -- importing both by name in one pytest session would
otherwise collide via sys.modules.

Covers only the Dispatcharr-independent pure/filesystem logic --
everything that touches Dispatcharr's own Django models (Recording,
CoreSettings) via the deferred imports inside _classify_dvr_hls_dir/
_dvr_sidecar_scan_roots/Plugin.run stays untested here, same boundary
this project's C++ addon test suite draws at the Kodi SDK: verification
of that layer is still manual/live-instance territory (see
docs/OPEN_ITEMS.md's "No automated test suite exists" entry).
"""

import importlib.util
import math
from pathlib import Path

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


def test_isfinite_sanity_for_the_regression_above():
    # Documents the exact distinction the fix relies on: float() parses
    # nan/inf successfully (no ValueError), only the later round()/int()
    # conversion would raise -- isfinite() catches both before that.
    assert not math.isfinite(float("nan"))
    assert not math.isfinite(float("inf"))


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
