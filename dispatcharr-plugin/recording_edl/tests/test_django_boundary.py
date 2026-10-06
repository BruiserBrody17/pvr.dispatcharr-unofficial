"""recording_edl's code that reaches into Dispatcharr's own Django models, run against fakes.

The module docstring of test_recording_edl.py draws the boundary at Django: the
decision logic is pulled out (`_classify_hls_dir_info`, `_resolve_scan_root`, ...)
and tested directly. This file covers the thin glue that remains on the other side of
it -- `Plugin.run("get_edl")`, `_classify_dvr_hls_dir()` and `_dvr_sidecar_scan_roots()`
-- by putting fake `apps.channels.models` / `core.models` modules in `sys.modules`
(the plugin imports them inside the function bodies, so nothing is imported at load
time). It was the 2026-10-04 hardening sweep's missing-tests list: `get_edl` is what
the Kodi addon calls for every finished recording it plays, and its error branches
(missing id, unknown recording, invalid id, unreadable file) had no test at all.
"""

import importlib.util
import sys
import types
from pathlib import Path

import pytest

_PLUGIN_PATH = Path(__file__).parent.parent / "plugin.py"
_spec = importlib.util.spec_from_file_location("recording_edl_plugin_django", _PLUGIN_PATH)
plugin = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(plugin)
_REAL_STAGING_ROOTS = plugin._dvr_hls_staging_scan_roots


class _Logger:
    def __init__(self):
        self.warnings = []

    def warning(self, fmt, *args):
        self.warnings.append(fmt % args if args else fmt)

    def info(self, *a, **k):
        pass

    def error(self, *a, **k):
        pass

    def debug(self, *a, **k):
        pass


def _install_recording_model(monkeypatch, rows):
    """Fake apps.channels.models.Recording over {pk: custom_properties}."""

    class DoesNotExist(Exception):
        pass

    class _Row:
        def __init__(self, pk, custom_properties):
            self.id = pk
            self.custom_properties = custom_properties

    class _Query:
        def __init__(self, found):
            self._found = found

        def first(self):
            return self._found[0] if self._found else None

    class _Manager:
        def get(self, pk):
            # Django coerces the pk and raises ValueError/TypeError for something it cannot.
            key = int(pk)
            if key not in rows:
                raise DoesNotExist()
            return _Row(key, rows[key])

        def filter(self, id):  # noqa: A002 -- the keyword Django's own API uses
            return _Query([_Row(id, rows[id])] if id in rows else [])

    class Recording:
        objects = _Manager()

    Recording.DoesNotExist = DoesNotExist
    models = types.ModuleType("apps.channels.models")
    models.Recording = Recording
    for name, mod in (
        ("apps", types.ModuleType("apps")),
        ("apps.channels", types.ModuleType("apps.channels")),
        ("apps.channels.models", models),
    ):
        monkeypatch.setitem(sys.modules, name, mod)


@pytest.fixture(autouse=True)
def _dvr_roots(monkeypatch, tmp_path):
    """get_edl only reads a file that resolves inside a DVR directory; these tests keep their files in
    tmp_path, so that is the one DVR root (a test can re-patch it to prove a refusal)."""
    monkeypatch.setattr(plugin, "_dvr_hls_staging_scan_roots", lambda: [tmp_path])


def _run_get_edl(params, settings=None, logger=None):
    return plugin.Plugin().run("get_edl", params, {"logger": logger, "settings": settings or {}})


def test_get_edl_requires_a_recording_id(monkeypatch):
    _install_recording_model(monkeypatch, {})
    result = _run_get_edl({})
    assert result["status"] == "error"
    assert "recording_id is required" in result["message"]


def test_get_edl_falls_back_to_the_test_setting(monkeypatch, tmp_path):
    edl = tmp_path / "show.edl"
    edl.write_text("1 2 3\n")
    _install_recording_model(
        monkeypatch, {7: {"comskip": {"edl": "show.edl"}, "file_path": str(tmp_path / "show.mkv")}}
    )
    result = _run_get_edl({}, settings={"test_recording_id": 7})
    assert result["status"] == "ok"
    assert result["entries"] == [{"start": 1000, "end": 2000, "type": 3}]


def test_get_edl_for_an_unknown_recording_is_an_error(monkeypatch):
    _install_recording_model(monkeypatch, {})
    result = _run_get_edl({"recording_id": 999})
    assert result == {"status": "error", "message": "No recording with id 999"}


@pytest.mark.parametrize("bad", ["abc", [1], {"a": 1}])
def test_get_edl_for_an_invalid_id_is_an_error_not_a_crash(monkeypatch, bad):
    _install_recording_model(monkeypatch, {1: {}})
    result = _run_get_edl({"recording_id": bad})
    assert result["status"] == "error"
    assert "Invalid recording_id" in result["message"]


def test_get_edl_with_no_edl_file_is_ok_and_empty(monkeypatch):
    # comskip has not run, found nothing, or ran in "cut" mode and removed the file: not an error.
    _install_recording_model(monkeypatch, {1: {}, 2: None, 3: {"comskip": {"segments_kept": 3, "edl": "x.edl"}}})
    for rid in (1, 2, 3):
        result = _run_get_edl({"recording_id": rid})
        assert result["status"] == "ok", rid
        assert result["entries"] == [], rid
        assert "No EDL file" in result["message"], rid


def test_get_edl_with_an_unreadable_file_is_ok_and_logged(monkeypatch, tmp_path):
    # The custom_properties name an .edl that is not there (deleted, or on a path the
    # worker cannot read): the addon must get an empty list, not an exception.
    _install_recording_model(
        monkeypatch, {1: {"comskip": {"edl": "gone.edl"}, "file_path": str(tmp_path / "show.mkv")}}
    )
    logger = _Logger()
    result = _run_get_edl({"recording_id": 1}, logger=logger)
    assert result["status"] == "ok"
    assert result["entries"] == []
    assert "gone.edl" in result["message"]
    assert any("could not read" in w for w in logger.warnings)


def test_get_edl_returns_the_parsed_entries(monkeypatch, tmp_path):
    (tmp_path / "show.edl").write_text("0 10 3\n20.5 30 0\nnot a line\n")
    _install_recording_model(
        monkeypatch, {1: {"comskip": {"edl": "show.edl"}, "file_path": str(tmp_path / "show.mkv")}}
    )
    result = _run_get_edl({"recording_id": "1"})  # a string id, as a JSON-RPC caller may send
    assert result["status"] == "ok"
    assert result["entries"] == [{"start": 0, "end": 10000, "type": 3}, {"start": 20500, "end": 30000, "type": 0}]


def test_classify_dvr_hls_dir_for_a_directory_with_no_recording_row_is_orphaned(monkeypatch, tmp_path):
    _install_recording_model(monkeypatch, {})
    staging = tmp_path / ".dvr_77_hls"
    staging.mkdir()
    (staging / "seg_0.ts").write_bytes(b"x")
    info = plugin._classify_dvr_hls_dir(staging)
    assert info["classification"] == "orphaned"
    assert info["segment_count"] == 1


def test_classify_dvr_hls_dir_never_calls_an_active_recording_orphaned(monkeypatch, tmp_path):
    # _delete_orphaned_dvr_hls_dirs() rmtree()s whatever this calls "orphaned": a recording in
    # progress must never be it, and neither may one that merely has a row.
    staging = tmp_path / ".dvr_77_hls"
    staging.mkdir()
    _install_recording_model(monkeypatch, {77: {"status": "recording", "_hls_dir": str(staging)}})
    assert plugin._classify_dvr_hls_dir(staging)["classification"] == "active"

    _install_recording_model(monkeypatch, {77: {"status": "stopped", "_hls_dir": str(staging), "remux_success": False}})
    assert plugin._classify_dvr_hls_dir(staging)["classification"] == "preserved_failure"

    # A row whose _hls_dir names some other directory still needs a person: referenced, not orphaned.
    _install_recording_model(monkeypatch, {77: {"status": "completed", "_hls_dir": str(tmp_path / "elsewhere")}})
    assert plugin._classify_dvr_hls_dir(staging)["classification"] == "referenced"


def test_classify_dvr_hls_dir_ignores_a_name_that_is_not_a_staging_directory(monkeypatch, tmp_path):
    _install_recording_model(monkeypatch, {})
    other = tmp_path / "TV_Shows"
    other.mkdir()
    assert plugin._classify_dvr_hls_dir(other) is None
    assert plugin._classify_dvr_hls_dir(tmp_path / ".dvr_abc_hls") is None


def _install_core_settings(monkeypatch, tv, tv_fallback, movie, movie_fallback):
    class CoreSettings:
        @staticmethod
        def get_dvr_tv_template():
            return tv

        @staticmethod
        def get_dvr_tv_fallback_template():
            return tv_fallback

        @staticmethod
        def get_dvr_movie_template():
            return movie

        @staticmethod
        def get_dvr_movie_fallback_template():
            return movie_fallback

    models = types.ModuleType("core.models")
    models.CoreSettings = CoreSettings
    monkeypatch.setitem(sys.modules, "core", types.ModuleType("core"))
    monkeypatch.setitem(sys.modules, "core.models", models)


def test_sidecar_scan_roots_come_from_the_four_path_templates(monkeypatch):
    _install_core_settings(
        monkeypatch,
        tv="TV_Shows/{show}/S{season:02d}E{episode:02d}.mkv",
        tv_fallback="TV_Shows/{show}/{start}.mkv",
        movie="Movies/{title} ({year}).mkv",
        movie_fallback="/mnt/media/Movies/{title}.mkv",
    )
    roots = plugin._dvr_sidecar_scan_roots()
    names = {str(r) for r in roots}
    assert "/data/recordings/TV_Shows" in names
    assert "/data/recordings/Movies" in names
    assert "/mnt/media/Movies" in names
    # Never the bare library root, or Dispatcharr's own top-level staging directories get swept.
    assert "/data/recordings" not in names


def test_sidecar_scan_roots_drop_a_template_that_resolves_to_the_library_or_above(monkeypatch):
    _install_core_settings(
        monkeypatch,
        tv="../../{show}.mkv",  # normalizes to "/": an ancestor of the library
        tv_fallback="../{show}.mkv",  # "/data": also an ancestor
        movie="Movies/{title}.mkv",
        movie_fallback="{title}.mkv",  # straight into the bare root: excluded on purpose
    )
    names = {str(r) for r in plugin._dvr_sidecar_scan_roots()}
    assert names == {"/data/recordings/Movies"}


def test_sidecar_scan_roots_keep_an_absolute_template_outside_the_library(monkeypatch):
    # By design (see _resolve_scan_root): only the library root and its ancestors are refused, so
    # a template pointing at a separate library is still scanned.
    _install_core_settings(monkeypatch, tv="/mnt/media/TV/{show}.mkv", tv_fallback="", movie="", movie_fallback="")
    assert {str(r) for r in plugin._dvr_sidecar_scan_roots()} == {"/mnt/media/TV"}


def test_get_edl_refuses_a_path_outside_the_dvr_directories(monkeypatch, tmp_path):
    root = tmp_path / "dvr"
    root.mkdir()
    outside = tmp_path / "other"
    outside.mkdir()
    (outside / "show.edl").write_text("0 10 3\n")
    monkeypatch.setattr(plugin, "_dvr_hls_staging_scan_roots", lambda: [root])
    _install_recording_model(monkeypatch, {1: {"comskip": {"edl": "show.edl"}, "file_path": str(outside / "show.mkv")}})
    logger = _Logger()
    result = _run_get_edl({"recording_id": 1}, logger=logger)
    assert result["status"] == "ok"
    assert result["entries"] == []
    assert any("outside the DVR directories" in w for w in logger.warnings)


def test_get_edl_refuses_a_symlinked_sidecar_that_escapes(monkeypatch, tmp_path):
    root = tmp_path / "dvr"
    root.mkdir()
    secret = tmp_path / "secret.edl"
    secret.write_text("0 10 3\n")
    (root / "show.edl").symlink_to(secret)
    monkeypatch.setattr(plugin, "_dvr_hls_staging_scan_roots", lambda: [root])
    _install_recording_model(monkeypatch, {1: {"comskip": {"edl": "show.edl"}, "file_path": str(root / "show.mkv")}})
    assert _run_get_edl({"recording_id": 1})["entries"] == []


def test_get_edl_refuses_when_the_dvr_roots_cannot_be_determined(monkeypatch, tmp_path):
    (tmp_path / "show.edl").write_text("0 10 3\n")

    def broken():
        raise RuntimeError("settings unavailable")

    monkeypatch.setattr(plugin, "_dvr_hls_staging_scan_roots", broken)
    _install_recording_model(monkeypatch, {1: {"comskip": {"edl": "show.edl"}, "file_path": str(tmp_path / "s.mkv")}})
    assert _run_get_edl({"recording_id": 1})["entries"] == []


def test_get_edl_does_not_read_a_file_past_the_cap(monkeypatch, tmp_path):
    big = tmp_path / "show.edl"
    big.write_bytes(b"0 10 3\n" * (plugin._MAX_EDL_BYTES // 7 + 10))
    _install_recording_model(monkeypatch, {1: {"comskip": {"edl": "show.edl"}, "file_path": str(tmp_path / "s.mkv")}})
    logger = _Logger()
    result = _run_get_edl({"recording_id": 1}, logger=logger)
    assert result["entries"] == []
    assert any("larger than" in w for w in logger.warnings)


def test_get_edl_ignores_a_stored_name_that_is_not_an_edl(monkeypatch, tmp_path):
    (tmp_path / "show.mkv").write_bytes(b"0 10 3\n")
    _install_recording_model(
        monkeypatch, {1: {"comskip": {"edl": "show.mkv"}, "file_path": str(tmp_path / "show.mkv")}}
    )
    result = _run_get_edl({"recording_id": 1})
    assert result["entries"] == []
    assert "No EDL file" in result["message"]


def test_get_edl_with_unreadable_dvr_settings_is_confined_to_the_default_recordings_root(monkeypatch, tmp_path):
    # _dvr_hls_staging_scan_roots() swallows a failed settings read and falls back to the default
    # root, so the read is confined to /data/recordings -- not refused outright, and not unconfined.
    (tmp_path / "show.edl").write_text("0 10 3\n")

    def broken_roots():
        raise RuntimeError("settings unavailable")

    monkeypatch.setattr(plugin, "_dvr_sidecar_scan_roots", broken_roots)
    monkeypatch.setattr(plugin, "_dvr_hls_staging_scan_roots", _REAL_STAGING_ROOTS)
    assert _REAL_STAGING_ROOTS() == [plugin._RECORDINGS_ROOT]
    _install_recording_model(monkeypatch, {1: {"comskip": {"edl": "show.edl"}, "file_path": str(tmp_path / "s.mkv")}})
    # tmp_path is outside the default root, so it is refused...
    assert _run_get_edl({"recording_id": 1})["entries"] == []
    # ...while a file inside it would be read: the confinement check itself accepts it.
    assert plugin._is_under_any_root(plugin._RECORDINGS_ROOT / "TV" / "x.edl", _REAL_STAGING_ROOTS())
