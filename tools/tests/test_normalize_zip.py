"""tools/normalize_zip.py: the zip metadata rewrite that keeps a release zip from publishing the build machine's
timezone (CPack's zip stores each entry's time as the local clock AND as UTC; the difference is the offset)."""

import importlib.util
import os
import shutil
import stat
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest

_PATH = Path(__file__).parent.parent / "normalize_zip.py"
_spec = importlib.util.spec_from_file_location("normalize_zip", _PATH)
normalize_zip = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(normalize_zip)

# 2026-10-07 04:40:50 UTC (an even second: a zip stores times to two seconds)
_COMMIT = 1791348050
_STAMP = (2026, 10, 7, 4, 40, 50)


def _extra_ids(path):
    """The extra-field header ids of every entry (0x5455 is the "UT" extended timestamp)."""
    ids = []
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            extra, offset = info.extra, 0
            while offset + 4 <= len(extra):
                hid, size = struct.unpack("<HH", extra[offset : offset + 4])
                ids.append(hid)
                offset += 4 + size
    return ids


def _make_zip_with_local_times(tmp_path, tz="XXX-3"):
    """A zip built the way the CI did it: Info-ZIP's `zip` under a non-UTC zone, which writes the UT field."""
    if shutil.which("zip") is None:
        pytest.skip("zip is not installed")
    src = tmp_path / "src"
    (src / "addon").mkdir(parents=True)
    (src / "addon" / "lib.so").write_bytes(b"\x7fELF" + os.urandom(64))
    (src / "addon" / "lib.so").chmod(0o755)
    (src / "addon" / "readme.txt").write_text("hello\n")
    out = tmp_path / "addon.zip"
    env = dict(os.environ, TZ=tz)
    subprocess.run(["zip", "-q", "-r", str(out), "addon"], cwd=src, env=env, check=True)
    return out


def test_the_fixture_really_carries_the_timestamp_field(tmp_path):
    # Without this the tests below would pass for a zip that never had the problem.
    assert 0x5455 in _extra_ids(_make_zip_with_local_times(tmp_path))


def test_normalizing_removes_the_extended_timestamp_and_stamps_every_entry_in_utc(tmp_path):
    z = _make_zip_with_local_times(tmp_path)
    count = normalize_zip.normalize(str(z), _COMMIT)
    assert count == 3  # the directory entry and the two files
    assert 0x5455 not in _extra_ids(z)
    with zipfile.ZipFile(z) as zf:
        assert {i.date_time for i in zf.infolist()} == {_STAMP}


def test_contents_names_order_and_permissions_survive(tmp_path):
    z = _make_zip_with_local_times(tmp_path)
    with zipfile.ZipFile(z) as before:
        names = before.namelist()
        data = {n: before.read(n) for n in names if not n.endswith("/")}
        modes = {i.filename: stat.S_IMODE(i.external_attr >> 16) for i in before.infolist()}
    normalize_zip.normalize(str(z), _COMMIT)
    with zipfile.ZipFile(z) as after:
        assert after.namelist() == names
        assert {n: after.read(n) for n in names if not n.endswith("/")} == data
        assert {i.filename: stat.S_IMODE(i.external_attr >> 16) for i in after.infolist()} == modes
        assert modes["addon/lib.so"] == 0o755  # the executable bit is not lost
        assert after.testzip() is None


def test_the_result_does_not_depend_on_the_zone_it_was_built_in(tmp_path):
    first_dir, second_dir = tmp_path / "a", tmp_path / "b"
    first_dir.mkdir()
    second_dir.mkdir()
    first = _make_zip_with_local_times(first_dir, "Asia/Tokyo")
    second = _make_zip_with_local_times(second_dir, "America/Los_Angeles")
    normalize_zip.normalize(str(first), _COMMIT)
    normalize_zip.normalize(str(second), _COMMIT)
    with zipfile.ZipFile(first) as a, zipfile.ZipFile(second) as b:
        # (`zip -r` lists a directory in whatever order the filesystem gives, so compare sorted)
        assert sorted((i.filename, i.date_time) for i in a.infolist()) == sorted(
            (i.filename, i.date_time) for i in b.infolist()
        )


def test_a_date_before_1980_is_clamped_instead_of_failing(tmp_path):
    z = _make_zip_with_local_times(tmp_path)
    normalize_zip.normalize(str(z), 0)
    with zipfile.ZipFile(z) as zf:
        assert {i.date_time for i in zf.infolist()} == {(1980, 1, 1, 0, 0, 0)}


def test_a_failed_rewrite_leaves_the_original_and_no_temp_file(tmp_path):
    bad = tmp_path / "broken.zip"
    bad.write_bytes(b"this is not a zip")
    with pytest.raises(zipfile.BadZipFile):
        normalize_zip.normalize(str(bad), _COMMIT)
    assert bad.read_bytes() == b"this is not a zip"
    assert [p.name for p in tmp_path.iterdir()] == ["broken.zip"]


def test_main_rewrites_every_zip_it_is_given_and_reports(tmp_path, capsys):
    one = _make_zip_with_local_times(tmp_path)
    two_dir = tmp_path / "two"
    two_dir.mkdir()
    two = _make_zip_with_local_times(two_dir)
    assert normalize_zip.main(["normalize_zip.py", str(_COMMIT), str(one), str(two)]) == 0
    out = capsys.readouterr().out
    assert "addon.zip: 3 entries stamped 2026-10-07 04:40:50Z" in out
    assert out.count("entries stamped") == 2
    for z in (one, two):
        assert 0x5455 not in _extra_ids(z)


def test_main_refuses_bad_arguments(capsys):
    assert normalize_zip.main(["normalize_zip.py"]) == 2
    assert normalize_zip.main(["normalize_zip.py", "not-a-number", "x.zip"]) == 2
    assert "unix timestamp" in capsys.readouterr().err


def test_check_flags_a_zip_that_still_has_the_timestamp_field_and_passes_a_normalized_one(tmp_path, capsys):
    z = _make_zip_with_local_times(tmp_path)
    assert normalize_zip.main(["normalize_zip.py", "--check", str(z)]) == 1
    assert "UT timestamp fields" in capsys.readouterr().err
    normalize_zip.normalize(str(z), _COMMIT)
    assert normalize_zip.main(["normalize_zip.py", "--check", str(z)]) == 0
    assert normalize_zip.main(["normalize_zip.py", "--check"]) == 2


def test_check_passes_a_zip_made_with_the_flags_the_plugin_packaging_uses(tmp_path):
    if shutil.which("zip") is None:
        pytest.skip("zip is not installed")
    (tmp_path / "plugin").mkdir()
    (tmp_path / "plugin" / "plugin.py").write_text("x = 1\n")
    out = tmp_path / "plugin.zip"
    subprocess.run(
        ["zip", "-X", "-q", "-r", str(out), "plugin"], cwd=tmp_path, env=dict(os.environ, TZ="Asia/Tokyo"), check=True
    )
    assert not normalize_zip.has_extended_timestamps(str(out))  # `zip -X` already writes none
