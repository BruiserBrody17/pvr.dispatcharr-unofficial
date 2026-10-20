"""tools/check_release_zip.py: the privacy gate every release zip passes, CI-built or hand-built."""

import importlib.util
import json
import struct
import zipfile
import zlib
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
        b"C:/" + b"Users" + b"/someone/x.pdb",  # a drive path written with forward slashes (MinGW, CMake)
        b"D:/a/_temp/build/x.obj",
        b"D:\\a\\repo\\repo\\build\\x.obj",  # a GitHub Windows runner's workspace
        b"\\\\fileserver\\share\\build",  # a UNC path
        b"/media/someone/disk/x.c",
        b"/run/media/someone/disk/x.c",
        b"/Volumes/Disk/build/x.c",
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
        b"\\\\\\\\\\\\ssl\\record\\ssl3_record.c",  # a run of backslashes, as in Kodi's prebuilt libcurl.dll
        b"http://host:8000/media/x",  # a URL, not a drive path, and /media/ not at the root of a path
        b'e.g. "/mnt/media/TV/{show}/..."',  # a directory named media (recording_edl's docstring)
        b"for every /media/... request",  # Dispatcharr's /media/ route (timeshift_buffer's docstring)
        b"\x00httpA.D$.p://L.l$@H",  # an inlined "http://" in the 0.14.0 Linux library's machine code
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


def test_a_non_ascii_blocklist_term_matches_utf8_content(tmp_path):
    # the one-byte-per-character view once compared the term's characters with UTF-8 bytes, which never matched
    path = _zip(tmp_path / "a.zip", {"addon/strings.po": 'msgid "Z\u00fcrich-Box"'.encode("utf-8")})
    assert any("blocklist" in p for p in gate.check_zip(path, ["z\u00fcrich-box"]))
    assert any("blocklist" in p for p in gate.check_zip(path, ["Z\u00fcrich"]))
    assert gate.check_zip(path, ["Zurich"]) == []


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


def _raw_zip(path, name, data, local_extra):
    """A one-entry stored zip, written byte by byte, whose local header carries `local_extra` while the central
    directory carries none (zipfile writes the same extra field to both)."""
    encoded, crc, dos_time, dos_date = name.encode(), zlib.crc32(data), 0, (46 << 9) | (10 << 5) | 7
    local = struct.pack(
        "<4sHHHHHIIIHH",
        b"PK\x03\x04",
        20,
        0,
        0,
        dos_time,
        dos_date,
        crc,
        len(data),
        len(data),
        len(encoded),
        len(local_extra),
    )
    local += encoded + local_extra + data
    central = struct.pack(
        "<4sHHHHHHIIIHHHHHII",
        b"PK\x01\x02",
        20,
        20,
        0,
        0,
        dos_time,
        dos_date,
        crc,
        len(data),
        len(data),
        len(encoded),
        0,
        0,
        0,
        0,
        0,
        0,
    )
    central += encoded
    end = struct.pack("<4sHHHHIIH", b"PK\x05\x06", 0, 0, 1, 1, len(central), len(local), 0)
    path.write_bytes(local + central + end)
    return str(path)


def test_an_extra_field_only_in_the_local_header_fails(tmp_path):
    ut = struct.pack("<HHB", 0x5455, 5, 1) + struct.pack("<I", 1792201330)
    bad = _raw_zip(tmp_path / "bad.zip", "addon/x.txt", b"x", ut)
    assert any("local header" in p for p in gate.check_zip(bad, []))
    assert gate.check_zip(_raw_zip(tmp_path / "ok.zip", "addon/x.txt", b"x", b""), []) == []


def test_entries_with_different_timestamps_fail(tmp_path):
    # what a zip made without tools/normalize_zip.py looks like: each entry at the build machine's local clock
    path = tmp_path / "a.zip"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("addon/a.txt", date_time=_STAMP), b"a")
        z.writestr(zipfile.ZipInfo("addon/b.txt", date_time=(2026, 10, 7, 4, 41, 2)), b"b")
    assert any("different timestamps" in p for p in gate.check_zip(str(path), []))


def _symlink_zip(path, target):
    with zipfile.ZipFile(path, "w") as z:
        info = zipfile.ZipInfo("addon/lib.so", date_time=_STAMP)
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        z.writestr(info, target)
    return str(path)


@pytest.mark.parametrize("target", [b"/etc/passwd", b"../../outside", b"a/../../b", b"C:x", b"..\\x"])
def test_a_symlink_out_of_the_zip_fails(tmp_path, target):
    assert any("symlink" in p for p in gate.check_zip(_symlink_zip(tmp_path / "a.zip", target), []))


def test_a_symlink_within_the_zip_passes(tmp_path):
    # the Linux and macOS zips carry the library's version chain as symlinks
    assert gate.check_zip(_symlink_zip(tmp_path / "a.zip", b"libpvr.so.0.14.0"), []) == []


@pytest.mark.parametrize(
    "name,data",
    [
        ("addon/inner.zip", b"anything"),
        ("addon/data.tar.gz", b"anything"),
        ("addon/payload.bin", b"PK\x03\x04rest"),
        ("addon/payload.bin", b"\x1f\x8b\x08rest"),
        ("addon/payload.bin", b"\x00" * 257 + b"ustar\x0000"),
    ],
)
def test_an_archive_inside_the_zip_fails(tmp_path, name, data):
    path = _zip(tmp_path / "a.zip", {name: data})
    assert any("archive" in p for p in gate.check_zip(path, []))


def test_an_extra_field_fails(tmp_path):
    # 0x5455 "UT" and 0x7875 "ux" (uid/gid), each a timezone or identity leak
    for header_id in (0x5455, 0x7875):
        extra = struct.pack("<HH", header_id, 5) + b"\x01\x02\x03\x04\x05"
        path = _zip(tmp_path / "a.zip", {"addon/x.txt": b"x"}, extra=extra)
        assert any("extra field" in p for p in gate.check_zip(path, []))


def test_a_zip_comment_fails(tmp_path):
    path = _zip(tmp_path / "a.zip", {"addon/x.txt": b"x"}, comment=b"built by someone")
    assert any("comment" in p for p in gate.check_zip(path, []))


@pytest.mark.parametrize("name", ["/etc/passwd", "../up.txt", "a/../../b", "dir\\file", "C:x.txt", "c:/x.txt"])
def test_an_unsafe_entry_name_fails(tmp_path, name):
    path = _zip(tmp_path / "a.zip", {name: b"x"})
    assert any("entry name" in p for p in gate.check_zip(path, []))


@pytest.mark.parametrize(
    "name",
    [
        "x.pdb",
        "Lib.PDB",
        "a/lib.so.debug",
        "a/b.map",
        "a/build.log",
        "x.dSYM",
        "a/lib.dylib.dSYM/Contents/Resources/DWARF/lib.dylib",  # a member inside the bundle, not the bundle itself
    ],
)
def test_a_debug_or_log_member_fails(tmp_path, name):
    path = _zip(tmp_path / "a.zip", {name: b"x"})
    assert any("debug or log" in p for p in gate.check_zip(path, []))


def test_an_unstripped_elf_fails_and_a_stripped_one_passes(tmp_path):
    assert gate.elf_debug_sections(_elf([".text", ".dynsym"])) == []
    assert gate.elf_debug_sections(_elf([".text", ".debug_info", ".symtab"])) == [".debug_info", ".symtab"]
    assert gate.elf_debug_sections(_elf([".text", ".zdebug_info"])) == [".zdebug_info"]  # compressed debug info
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
        gate.check_release_set(others + ["addon-pvr.dispatcharr-unofficial-0.12.0-coreelec-aarch64.zip"], version) == []
    )
    # CoreELEC's own file name (with its revision suffix) must be renamed before the release, not shipped as built
    for bad in (
        "pvr.dispatcharr-unofficial-0.12.0.1.zip",
        "addon-pvr.dispatcharr-unofficial-0.12.1-coreelec-aarch64.zip",
        # Kodi 21's 32-bit CoreELEC package: not what a Piers release ships
        "addon-pvr.dispatcharr-unofficial-0.12.0-coreelec-armv7.zip",
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
        "coreelec-aarch64",
        "timeshift_buffer",
    ):
        assert platform in names
    assert "recording_edl" in names


def test_the_blocklist_file_is_read_without_comments_and_blanks(tmp_path):
    path = tmp_path / "bl.txt"
    path.write_text("# a comment\n\nalpha\n  beta  \n", encoding="utf-8")
    assert gate.load_blocklist(str(path)) == ["alpha", "beta"]


def test_a_byte_order_mark_does_not_stick_to_the_first_term(tmp_path):
    path = tmp_path / "bl.txt"
    path.write_bytes("\ufeffalpha\r\nbeta\r\n".encode("utf-8"))  # as Notepad saves it
    assert gate.load_blocklist(str(path)) == ["alpha", "beta"]


@pytest.mark.parametrize("encoding", ["utf-16", "utf-16-le"])
def test_a_utf16_blocklist_is_refused(tmp_path, encoding):
    path = tmp_path / "bl.txt"
    path.write_bytes("alpha\nbeta\n".encode(encoding))
    with pytest.raises(gate.BlocklistError):
        gate.load_blocklist(str(path))


def test_a_blocklist_that_blocks_nothing_or_does_not_exist_fails_the_release_run(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))
    clean = _zip(tmp_path / "clean.zip", {"a/x.txt": b"hello"})
    real = tmp_path / "real.txt"
    real.write_text("hello\n", encoding="utf-8")
    monkeypatch.setenv("PRIVACY_BLOCKLIST", str(real))
    # a mistyped explicit path is an error, not a quiet fallback to $PRIVACY_BLOCKLIST
    assert gate.main(["prog", "--blocklist", str(tmp_path / "typo.txt"), clean]) == 2
    empty = tmp_path / "empty.txt"
    for content in ("", "# only a comment\n\n"):
        empty.write_text(content, encoding="utf-8")
        assert gate.main(["prog", "--blocklist", str(empty), "--require-blocklist", clean]) == 2
    utf16 = tmp_path / "wide.txt"
    utf16.write_bytes("hello\n".encode("utf-16"))
    assert gate.main(["prog", "--blocklist", str(utf16), clean]) == 2


def _release(tmp_path, version, addon_versions=None, plugin_versions=None, stamps=None):
    """A full release set of small zips: each addon zip with its addon.xml, each plugin zip with its two versions."""
    paths = []
    for name in _concrete(version):
        members, stamp = {}, (stamps or {}).get(name, _STAMP)
        if name.startswith(gate.PREFIX):
            default = version + ".1" if "-coreelec-" in name else version
            found = (addon_versions or {}).get(name, default)
            members["pvr.dispatcharr-unofficial/addon.xml"] = (
                '<?xml version="1.0"?>\n<addon id="pvr.dispatcharr-unofficial" version="%s" name="x">' % found
            ).encode()
        else:
            plugin = name[: -len(".zip")]
            declared, running = (plugin_versions or {}).get(name, ("1.2.3", "1.2.3"))
            members[plugin + "/plugin.json"] = json.dumps({"name": plugin, "version": declared}).encode()
            members[plugin + "/plugin.py"] = (
                'class Handler:\n    server_version = "x/9"\n\n\nclass Plugin:\n    name = "P"\n    version = "%s"\n'
                % running
            ).encode()
        path = tmp_path / name
        with zipfile.ZipFile(path, "w") as z:
            for member, data in members.items():
                z.writestr(zipfile.ZipInfo(member, date_time=stamp), data)
        paths.append(str(path))
    return paths


def test_a_release_whose_zips_say_the_right_versions_passes(tmp_path):
    assert gate.check_release_contents(_release(tmp_path, "9.9.9"), "9.9.9") == []


def test_a_stale_zip_renamed_to_the_new_version_fails(tmp_path):
    linux = gate.PREFIX + "9.9.9-linux.zip"
    problems = gate.check_release_contents(_release(tmp_path, "9.9.9", {linux: "9.9.8"}), "9.9.9")
    assert any("9.9.8" in p and "linux" in p for p in problems)


@pytest.mark.parametrize("found", ["9.9.9", "9.9.8.1", "9.9.9.x"])
def test_the_coreelec_zip_must_carry_the_version_plus_its_revision(tmp_path, found):
    coreelec = next(n for n in _concrete("9.9.9") if "-coreelec-" in n)
    problems = gate.check_release_contents(_release(tmp_path, "9.9.9", {coreelec: found}), "9.9.9")
    assert any("coreelec" in p for p in problems)


def test_a_plugin_zip_whose_two_versions_disagree_fails(tmp_path):
    paths = _release(tmp_path, "9.9.9", plugin_versions={"timeshift_buffer.zip": ("1.2.4", "1.2.3")})
    problems = gate.check_release_contents(paths, "9.9.9")
    assert any("timeshift_buffer.zip" in p and "Plugin class" in p for p in problems)


def test_addon_zips_from_different_builds_fail(tmp_path):
    windows = gate.PREFIX + "9.9.9-windows-x86_64.zip"
    paths = _release(tmp_path, "9.9.9", stamps={windows: (2026, 10, 7, 9, 0, 0)})
    assert any("different timestamps" in p for p in gate.check_release_contents(paths, "9.9.9"))
    # the plugin zips may be newer (a plugin-only fix replaces its zip in a shipped release)
    later = _release(tmp_path, "9.9.9", stamps={"recording_edl.zip": (2026, 10, 8, 0, 0, 0)})
    assert gate.check_release_contents(later, "9.9.9") == []


def test_main_checks_the_contents_with_expect_release(tmp_path, monkeypatch):
    monkeypatch.delenv("PRIVACY_BLOCKLIST", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))
    paths = _release(tmp_path, "9.9.9", {gate.PREFIX + "9.9.9-osx-arm64.zip": "9.9.8"})
    assert gate.main(["prog", "--expect-release", "9.9.9", *paths]) == 1
    assert gate.main(["prog", *paths]) == 0  # without --expect-release only the privacy checks run


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
