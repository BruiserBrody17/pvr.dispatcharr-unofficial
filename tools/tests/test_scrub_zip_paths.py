"""tools/scrub_zip_paths.py: overwriting a build machine's directory prefix inside a zip without changing any length."""

import importlib.util
import stat
import zipfile
from pathlib import Path

import pytest

_TOOLS = Path(__file__).parent.parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _TOOLS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


scrub = _load("scrub_zip_paths")
gate = _load("check_release_zip")

_STAMP = (2026, 10, 7, 4, 40, 50)
# assembled at run time so no literal build-machine path sits in this file
_HOME = b"/" + b"home" + b"/someone"


def _zip(path, members, modes=None):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members.items():
            info = zipfile.ZipInfo(name, date_time=_STAMP)
            info.external_attr = (modes or {}).get(name, 0o644) << 16
            if name.endswith("/"):
                info.external_attr = (0o40755 << 16) | 0x10
            z.writestr(info, data)
    return str(path)


def test_the_replacement_has_the_same_length_and_is_a_valid_path():
    for prefix in (_HOME, b"/Users/somebody-else", b"/abcdef"):
        out = scrub.replacement_for(prefix)
        assert len(out) == len(prefix)
        assert out.startswith(b"/build")
        assert set(out[len(b"/build") :]) <= {ord("/")}


def test_every_occurrence_goes_and_nothing_else_changes():
    data = b"\x00head" + _HOME + b"/android-build/x.c\x00" + b"middle" + _HOME + b"/ssl\x00tail"
    out, count = scrub.scrub_bytes(data, [_HOME])
    assert count == 2
    assert len(out) == len(data)
    assert _HOME not in out
    assert out.startswith(b"\x00head/build") and out.endswith(b"/ssl\x00tail")
    assert b"/android-build/x.c" in out and b"middle" in out


def test_no_occurrence_is_a_no_op():
    data = b"nothing to see here"
    assert scrub.scrub_bytes(data, [_HOME]) == (data, 0)


def test_the_longer_prefix_wins_when_one_contains_the_other():
    short, long = _HOME, _HOME + b"/android-build"
    data = b"a " + long + b"/x b " + short + b"/y"
    out, count = scrub.scrub_bytes(data, [short, long])
    assert count == 2 and len(out) == len(data)
    assert short not in out
    # the long one was replaced whole, not as the short prefix followed by a stray remainder
    assert out.startswith(b"a /build") and b"/android-build" not in out.split(b" ")[1]


def test_a_zip_is_rewritten_in_place_with_the_same_members_sizes_and_modes(tmp_path):
    path = _zip(
        tmp_path / "a.zip",
        {"addon/": b"", "addon/lib.so": b"x" + _HOME + b"/y", "addon/addon.xml": b"<addon/>"},
        modes={"addon/lib.so": 0o755},
    )
    before = {i.filename: (i.file_size, i.external_attr, i.date_time) for i in zipfile.ZipFile(path).infolist()}
    assert scrub.scrub_zip(path, [_HOME]) == 1
    with zipfile.ZipFile(path) as z:
        after = {i.filename: (i.file_size, i.external_attr, i.date_time) for i in z.infolist()}
        assert after == before
        assert _HOME not in z.read("addon/lib.so")
        assert z.read("addon/addon.xml") == b"<addon/>"
        assert stat.S_IMODE(z.getinfo("addon/lib.so").external_attr >> 16) == 0o755
        assert [i.filename for i in z.infolist()] == ["addon/", "addon/lib.so", "addon/addon.xml"]
    assert not list(tmp_path.glob("*.tmp"))


def test_the_gate_fails_before_and_passes_after(tmp_path):
    path = _zip(tmp_path / "a.zip", {"addon/lib.so": b"\x00assert " + _HOME + b"/src/x.c\x00"})
    assert gate.check_zip(path, [])
    scrub.scrub_zip(path, [_HOME])
    assert gate.check_zip(path, []) == []


def test_a_bad_zip_leaves_no_temp_file_and_the_original_alone(tmp_path):
    path = tmp_path / "a.zip"
    path.write_bytes(b"not a zip")
    with pytest.raises(zipfile.BadZipFile):
        scrub.scrub_zip(str(path), [_HOME])
    assert path.read_bytes() == b"not a zip"
    assert not list(tmp_path.glob("*.tmp"))


@pytest.mark.parametrize("bad", ["", "/", "/abc", "relative/path", "home/someone"])
def test_a_prefix_that_is_not_a_real_absolute_path_is_refused(bad):
    with pytest.raises(ValueError):
        scrub.check_prefix(bad)


def test_a_trailing_slash_is_ignored():
    assert scrub.check_prefix(_HOME.decode() + "/") == _HOME


def test_main_exit_codes(tmp_path, capsys):
    path = _zip(tmp_path / "a.zip", {"a/x": _HOME + b"/y"})
    prefix = _HOME.decode()
    assert scrub.main(["prog", "--prefix", prefix, path]) == 0
    assert "1 replacement" in capsys.readouterr().out
    assert scrub.main(["prog", path]) == 2  # no prefix
    assert scrub.main(["prog", "--prefix", prefix]) == 2  # no zip
    assert scrub.main(["prog", "--prefix", "/x", path]) == 2  # too short
    assert scrub.main(["prog", "--bogus", path]) == 2
