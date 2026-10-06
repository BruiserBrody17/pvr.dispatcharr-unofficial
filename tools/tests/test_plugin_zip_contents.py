"""The plugin zips CI builds and attaches to a release leave out each plugin's tests/ folder.

Dispatcharr only loads plugin.py and plugin.json; the tests need this repo's layout and pytest and are
over half of timeshift_buffer's zip by size. This test pulls the `zip -r` line out of
.github/workflows/build.yml and runs it, unchanged, over a stand-in plugin tree that has a tests/
folder and a __pycache__, so the workflow's real exclusions are what is checked, not a copy of them.
Skipped where the `zip` tool is not installed.
"""

import os
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "build.yml"

pytestmark = pytest.mark.skipif(shutil.which("zip") is None, reason="the zip tool is not installed")


def _zip_command():
    text = WORKFLOW.read_text(encoding="utf-8")
    match = re.search(
        r"^\s*((?:TZ=UTC )?zip (?:-X )?-r \"\.\./dist/\$\{plugin\}\.zip\" \"\$plugin\"[^\n]*)$", text, re.MULTILINE
    )
    assert match, "the plugin zip command was not found in build.yml (did the step change shape?)"
    return match.group(1).strip()


def _build(tmp_path, plugin, env=None, mtime=None):
    root = tmp_path / "dispatcharr-plugin"
    folder = root / plugin
    (folder / "tests").mkdir(parents=True)
    (folder / "tests" / "__pycache__").mkdir()
    (folder / "__pycache__").mkdir()
    for name in ("plugin.py", "plugin.json", "README.md"):
        (folder / name).write_text("x")
    (folder / "tests" / "test_plugin.py").write_text("x")
    (folder / "tests" / "__pycache__" / "t.pyc").write_bytes(b"x")
    (folder / "__pycache__" / "plugin.pyc").write_bytes(b"x")
    if mtime is not None:
        for path in [folder, *folder.rglob("*")]:
            os.utime(path, (mtime, mtime))
    (tmp_path / "dist").mkdir()
    subprocess.run(
        ["bash", "-c", f"plugin={plugin}; {_zip_command()}"],
        cwd=root,
        check=True,
        capture_output=True,
        env={**os.environ, **(env or {})},
    )
    return zipfile.ZipFile(tmp_path / "dist" / f"{plugin}.zip").namelist()


@pytest.mark.parametrize("plugin", ["timeshift_buffer", "recording_edl"])
def test_the_plugin_zip_has_no_tests_folder_and_keeps_the_runtime_files(tmp_path, plugin):
    names = _build(tmp_path, plugin)
    assert not [n for n in names if "/tests" in n or "tests/" in n]
    assert not [n for n in names if "__pycache__" in n]
    for kept in ("plugin.py", "plugin.json", "README.md"):
        assert f"{plugin}/{kept}" in names
    # Dispatcharr identifies a plugin by the folder name inside the zip.
    assert all(n.startswith(f"{plugin}/") for n in names)


def test_the_workflow_also_checks_the_zips_after_building_them():
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "Check the plugin zips' contents" in text
    assert "contains a tests/ entry" in text


def test_claude_md_quotes_the_workflows_exclusions_for_a_hand_built_zip():
    # The documented manual rebuild of a plugin zip for an already-published release is the one path
    # the workflow's own contents check does not guard, so the exact command is quoted there.
    command = _zip_command()
    exclusions = re.findall(r'-x "[^"]+"', command)
    assert exclusions, "the workflow's zip command no longer has -x exclusions"
    claude_md = (WORKFLOW.parents[2] / "CLAUDE.md").read_text(encoding="utf-8")
    for exclusion in exclusions:
        assert exclusion in claude_md


def test_the_zip_publishes_no_local_time_and_no_timestamp_extra_field_whatever_the_build_machines_zone(tmp_path):
    """Info-ZIP stores a DOS time in LOCAL time next to a UT extra field in UTC, so a zip built on a
    machine that is not on UTC publishes its timezone offset to anyone who downloads the asset (the
    same leak CLAUDE.md's release gate describes for the Windows zip; found by the eleventh hardening
    sweep). The build runs here under a non-UTC POSIX zone, with a file time that makes the
    local and UTC clock times differ."""
    utc_time = 1_790_000_000  # 2026-09-21 13:33:20 UTC, well inside a day so the date is the same too
    _build(tmp_path, "timeshift_buffer", env={"TZ": "XXX-3"}, mtime=utc_time)
    with zipfile.ZipFile(tmp_path / "dist" / "timeshift_buffer.zip") as z:
        expected = tuple(__import__("time").gmtime(utc_time)[:6])
        for info in z.infolist():
            assert info.extra == b"", f"{info.filename} carries extra fields {info.extra!r}"
            # DOS time has two-second resolution.
            got = info.date_time
            assert got[:5] == expected[:5] and abs(got[5] - expected[5]) <= 1, (info.filename, got, expected)
