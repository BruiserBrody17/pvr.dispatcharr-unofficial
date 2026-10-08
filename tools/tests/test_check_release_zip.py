"""tools/check_release_zip.py: the privacy gate every release zip passes, CI-built or hand-built."""

import importlib.util
import struct
import zipfile
from pathlib import Path

import pytest

_PATH = Path(__file__).parent.parent / "check_release_zip.py"
_spec = importlib.util.spec_from_file_location("check_release_zip", _PATH)
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)

_STAMP = (2026, 10, 7, 4, 40, 50)


def _zip(path, members, comment=b"", extra=b""):
    """A zip of {name: bytes}; `extra` is attached to every entry's extra field."""
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members.items():
            info = zipfile.ZipInfo(name, date_time=_STAMP)
            info.extra = extra
            z.writestr(info, data)
        z.comment = comment
    return str(path)


def _elf(sections):
    """A minimal little-endian ELF64 image whose section header names are `sections` (after the null section and a
    string table), enough for the check's section-name walk."""
    names = [""] + list(sections) + [".shstrtab"]
    table, offsets = b"\x00", {}
    for name in names[1:]:
        offsets[name] = len(table)
        table += name.encode() + b"\x00"
    shoff = 64 + len(table)
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4], header[5] = 2, 1
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HHH", header, 0x3A, 64, len(names), len(names) - 1)
    out = bytes(header) + table
    for index, name in enumerate(names):
        if index == 0:
            out += bytes(64)
        elif name == ".shstrtab":
            out += struct.pack("<IIQQQQIIQQ", offsets[name], 3, 0, 0, 64, len(table), 0, 0, 1, 0)
        else:
            out += struct.pack("<IIQQQQIIQQ", offsets[name], 1, 0, 0, 64, 0, 0, 0, 1, 0)
    return out


def test_a_clean_zip_passes(tmp_path):
    path = _zip(tmp_path / "a.zip", {"addon/readme.txt": b"nothing to see", "addon/lib.so": _elf([".text", ".dynsym"])})
    assert gate.check_zip(path, []) == []


def _unix_path(top):
    """A build-machine-shaped path assembled at run time, so no literal one sits in this file."""
    return b"/" + top + b"/someone/build/x.cpp"


@pytest.mark.parametrize(
    "text",
    [
        _unix_path(b"home"),
        _unix_path(b"Users"),
        _unix_path(b"root"),
        _unix_path(b"var/folders"),
        b"/actions-runner/_work/x",
        b"/__w/repo/repo",
        b"C:\\build\\obj\\x.pdb",
        b"c:\\anything",
        b"D:\\work",
        b"..\\Users\\x",
    ],
)
def test_a_build_machine_path_fails(tmp_path, text):
    path = _zip(tmp_path / "a.zip", {"addon/lib.dll": b"\x00\x01" + text + b"\x00"})
    problems = gate.check_zip(path, [])
    assert problems, text


@pytest.mark.parametrize(
    "noise",
    [
        b"9%.@9.......S:\\@9..A.T..*1",  # seen in a real Android library: a drive-shaped pair of bytes in code
        b"Wj...u..K_.nq:\\_Wc4.. j",  # a letter before the drive letter: part of a word
        b"x:\\",  # nothing after the backslash
        b"a1:\\b",  # one character is not a path
    ],
)
def test_drive_shaped_noise_in_machine_code_is_not_a_path(tmp_path, noise):
    path = _zip(tmp_path / "a.zip", {"addon/lib.so": b"\x7fgarbage" + noise + b"\x00"})
    assert gate.check_zip(path, []) == []


@pytest.mark.parametrize("shift", [0, 1])
def test_a_utf16_path_fails_at_either_alignment(tmp_path, shift):
    wide = "C:\\Users\\someone\\x.pdb".encode("utf-16-le")
    path = _zip(tmp_path / "a.zip", {"addon/lib.dll": b"\xff" * shift + wide})
    assert any("UTF-16" in p for p in gate.check_zip(path, []))


def test_a_blocklist_term_fails_case_insensitively_in_text_and_utf16(tmp_path):
    for payload in (b"xx SecretHost yy", "xx secrethost yy".encode("utf-16-le")):
        path = _zip(tmp_path / "a.zip", {"addon/lib.so": payload})
        assert any("blocklist" in p for p in gate.check_zip(path, ["SECRETHOST"]))
    assert gate.check_zip(path, ["something else"]) == []


def test_a_blocklist_term_in_an_entry_name_fails(tmp_path):
    path = _zip(tmp_path / "a.zip", {"addon/secrethost.txt": b"x"})
    assert any("entry name" in p for p in gate.check_zip(path, ["secrethost"]))


def test_the_known_upstream_strings_are_allowed_only_in_their_own_members(tmp_path):
    kodi_path = b"C:\\code\\kodi-deps\\Build\\x64"
    kodi_package = b'ENGINESDIR: "C:\\code\\kodi-deps\\package\\x64\\openssl\\lib\\engines-1_1"'
    ok = _zip(
        tmp_path / "ok.zip",
        {"addon/libcurl.dll": kodi_path + b"\x00" + kodi_package, "addon/zlib.dll": kodi_path.lower()},
    )
    assert gate.check_zip(ok, []) == []
    # the same text in the addon's own binary is not excused
    bad = _zip(tmp_path / "bad.zip", {"addon/pvr.dispatcharr-unofficial.dll": kodi_path})
    assert gate.check_zip(bad, [])
    # and an allowed string does not excuse a different path next to it in the same member
    mixed = _zip(tmp_path / "mixed.zip", {"addon/libcurl.dll": kodi_path + b"\x00C:\\Users\\x"})
    assert gate.check_zip(mixed, [])


def test_an_extra_field_fails(tmp_path):
    # 0x5455 "UT" and 0x7875 "ux" (uid/gid), each a timezone or identity leak
    for header_id in (0x5455, 0x7875):
        extra = struct.pack("<HH", header_id, 5) + b"\x01\x02\x03\x04\x05"
        path = _zip(tmp_path / "a.zip", {"addon/x.txt": b"x"}, extra=extra)
        assert any("extra field" in p for p in gate.check_zip(path, []))


def test_a_zip_comment_fails(tmp_path):
    path = _zip(tmp_path / "a.zip", {"addon/x.txt": b"x"}, comment=b"built by someone")
    assert any("comment" in p for p in gate.check_zip(path, []))


@pytest.mark.parametrize("name", ["/etc/passwd", "../up.txt", "a/../../b", "dir\\file"])
def test_an_unsafe_entry_name_fails(tmp_path, name):
    path = _zip(tmp_path / "a.zip", {name: b"x"})
    assert any("entry name" in p for p in gate.check_zip(path, []))


@pytest.mark.parametrize("name", ["x.pdb", "Lib.PDB", "a/lib.so.debug", "a/b.map", "a/build.log", "x.dSYM"])
def test_a_debug_or_log_member_fails(tmp_path, name):
    path = _zip(tmp_path / "a.zip", {name: b"x"})
    assert any("debug or log" in p for p in gate.check_zip(path, []))


def test_an_unstripped_elf_fails_and_a_stripped_one_passes(tmp_path):
    assert gate.elf_debug_sections(_elf([".text", ".dynsym"])) == []
    assert gate.elf_debug_sections(_elf([".text", ".debug_info", ".symtab"])) == [".debug_info", ".symtab"]
    bad = _zip(tmp_path / "bad.zip", {"addon/lib.so": _elf([".text", ".debug_line"])})
    assert any("not stripped" in p for p in gate.check_zip(bad, []))
    assert gate.elf_debug_sections(b"not an elf at all") == []
    assert gate.elf_debug_sections(b"\x7fELF" + b"\x02\x01" + bytes(100)) == []  # no section table


def test_a_damaged_elf_section_table_fails_rather_than_passes(tmp_path):
    truncated = _elf([".text"])[:-40]
    assert gate.elf_debug_sections(truncated) == ["<unreadable ELF section table>"]


def test_not_a_zip_fails(tmp_path):
    path = tmp_path / "a.zip"
    path.write_bytes(b"this is not a zip")
    assert any("cannot be read" in p for p in gate.check_zip(str(path), []))


def _concrete(version):
    """The RELEASE_ASSETS patterns as real file names."""
    return [p.format(v=version) for p in gate.RELEASE_ASSETS]


def test_the_release_set_must_be_exactly_the_assets():
    version = "9.9.9"
    full = _concrete(version)
    assert gate.check_release_set(["/x/" + n for n in full], version) == []
    for index, missing in enumerate(full):
        rest = full[:index] + full[index + 1 :]
        assert any("missing" in p for p in gate.check_release_set(rest, version)), missing
    assert any("unexpected" in p for p in gate.check_release_set(full + ["stray.zip"], version))
    assert any("twice" in p for p in gate.check_release_set(full + [full[0]], version))
    # an old release's zip is not this release's
    assert gate.check_release_set([n.replace(version, "9.9.8") for n in full], version)


def test_the_coreelec_zip_is_named_like_the_other_platforms_and_the_harness_name_is_refused():
    version = "0.12.0"
    others = [n for n in _concrete(version) if "coreelec" not in n]
    assert (
        gate.check_release_set(others + ["addon-pvr.dispatcharr-unofficial-0.12.0-coreelec-armv7.zip"], version) == []
    )
    # CoreELEC's own file name (with its revision suffix) must be renamed before the release, not shipped as built
    for bad in (
        "pvr.dispatcharr-unofficial-0.12.0.1.zip",
        "addon-pvr.dispatcharr-unofficial-0.12.1-coreelec-armv7.zip",
        "addon-pvr.dispatcharr-unofficial-0.12.0-coreelec-aarch64.zip",
    ):
        assert any("missing" in p for p in gate.check_release_set(others + [bad], version)), bad


def test_every_platform_ships_with_every_release():
    names = " ".join(gate.RELEASE_ASSETS)
    for platform in (
        "windows",
        "linux",
        "osx",
        "android-aarch64",
        "android-armv7",
        "coreelec-armv7",
        "timeshift_buffer",
    ):
        assert platform in names
    assert "recording_edl" in names


def test_the_blocklist_file_is_read_without_comments_and_blanks(tmp_path):
    path = tmp_path / "bl.txt"
    path.write_text("# a comment\n\nalpha\n  beta  \n", encoding="utf-8")
    assert gate.load_blocklist(str(path)) == ["alpha", "beta"]


def test_main_exit_codes(tmp_path, capsys, monkeypatch):
    monkeypatch.delenv("PRIVACY_BLOCKLIST", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))  # no ~/.privacy-blocklist.txt here
    clean = _zip(tmp_path / "clean.zip", {"a/x.txt": b"hello"})
    dirty = _zip(tmp_path / "dirty.zip", {"a/x.txt": _unix_path(b"home")})
    assert gate.main(["prog", clean]) == 0
    assert gate.main(["prog", dirty]) == 1
    assert gate.main(["prog"]) == 2
    assert gate.main(["prog", "--bogus", clean]) == 2
    # fail closed: a release is cut with a blocklist, and not having one is an error, not a pass
    assert gate.main(["prog", "--require-blocklist", clean]) == 2
    blocklist = tmp_path / "bl.txt"
    blocklist.write_text("hello\n", encoding="utf-8")
    assert gate.main(["prog", "--blocklist", str(blocklist), clean]) == 1
    blocklist.write_text("nothing-here\n", encoding="utf-8")
    assert gate.main(["prog", "--blocklist", str(blocklist), "--require-blocklist", clean]) == 0
    monkeypatch.setenv("PRIVACY_BLOCKLIST", str(blocklist))
    assert gate.main(["prog", "--require-blocklist", clean]) == 0
    assert "clean" in capsys.readouterr().out


def test_distinct_paths_sharing_a_prefix_are_each_reported(tmp_path):
    # the duplicate suppression once keyed on the match plus eight characters, which hid every path after the first
    # that began alike (found when the first real Windows gate run reported one of two upstream paths)
    data = b"C:\\code\\kodi-deps\\one\x00C:\\code\\kodi-deps\\two\x00C:\\code\\kodi-deps\\three"
    path = _zip(tmp_path / "a.zip", {"addon/lib.dll": data})
    assert len(gate.check_zip(path, [])) == 3


def test_a_kodi_dependency_path_outside_its_two_trees_is_not_excused(tmp_path):
    other = b"C:\\code\\kodi-deps\\secret\\x64"
    path = _zip(tmp_path / "a.zip", {"addon/libcurl.dll": other})
    assert gate.check_zip(path, [])
