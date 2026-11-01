#!/usr/bin/env python3
"""The privacy gate every release zip passes, whoever built it (CI or by hand): exits 1 when one fails.

A release zip is what a stranger downloads, so it must not say where or by whom it was built. This is the one
check CI runs on the zips it builds and the one a release is cut with for the hand-built ones (macOS, Android,
CoreELEC), so no platform gets a weaker gate than another. Per zip it checks:

- zip metadata: no extra field on any entry, in the central directory or in an entry's own local header (the
  extended-timestamp field is the build machine's timezone, the Info-ZIP Unix field is its uid/gid), one timestamp
  shared by every entry (what tools/normalize_zip.py writes: a zip made without it carries the build machine's local
  clock, whose difference from the commit or upload time is its timezone), no zip comment and no entry comment, no
  byte of the file outside its entries, central directory and end record (prepended data or a gap between entries,
  which every unzipper skips and an entry-by-entry scan never reads), no directory entry carrying data, no local
  header naming its entry differently from the central directory or carrying another time, no encrypted entry, no
  absolute, `..` or drive-letter entry names, no symlink pointing outside the zip, no archive inside the zip (its
  contents would go unchecked), no debug-symbol or log members (`.pdb`, anything in a `.dSYM` bundle, `.debug`,
  `.map`, `.log`), no desktop folder metadata (`.DS_Store`, `._*`, `__MACOSX/`, `Thumbs.db`, `desktop.ini`);
- every member's bytes, as plain text and as UTF-16 (Windows binaries keep wide strings), for build-machine paths
  (`/home/`, `/Users/`, `/root/`, `/var/folders/`, `/media/`, `/run/media/`, `/Volumes/`, a CI runner's work
  directory, a `C:\\` or `C:/` path, a UNC path) except the few known-benign upstream strings in ALLOWED_PATH_STRINGS,
  each with the reason it is safe;
- every member's bytes for each term of a private blocklist (one term per line, case-insensitive; the file is
  never in the repository: `--blocklist FILE`, else `$PRIVACY_BLOCKLIST`, else `~/.privacy-blocklist.txt`);
  `--require-blocklist` fails when none is found or the one found has no terms, which is how a release is cut -- CI
  has no blocklist and checks the patterns only; a `--blocklist` path or a set `$PRIVACY_BLOCKLIST` that does not
  exist is an error, never a fallback to another list;
- ELF members: no `.debug_*`, `.zdebug_*` or `.symtab` section (an unstripped library carries the build machine's
  source paths, which is how the hand-built Android libraries once shipped them).

`--expect-release VERSION` additionally fails unless the given zips are exactly the release's asset set, so a
platform cannot be left out of a release by forgetting a step, and checks what is inside them, so a stale or
mixed-up zip renamed to the new release's name does not pass:

- each addon zip holds only `pvr.dispatcharr-unofficial/`, its `addon.xml` carries that id and VERSION (CoreELEC's,
  VERSION plus its package revision), and the library it names is the binary its asset name promises (a PE x86-64
  DLL for Windows, a Mach-O arm64 one for macOS, an ELF of the right class and machine for the rest);
- the six addon zips share one timestamp (one tag, one normalisation);
- each plugin zip holds only its own folder, its `plugin.json` version equals its `Plugin` class version and the
  `plugin.json` of the gate's own tree (`dispatcharr-plugin/` beside this `tools/`, so the gate is run from the
  release's tree), its files are that tree's plugin folder byte for byte (less `tests/` and `__pycache__/`, as CI
  zips it), and its timestamp is no older than the addon zips' (a plugin-only fix may replace it later).

usage: check_release_zip.py [--blocklist FILE] [--require-blocklist] [--expect-release VERSION] <zip> [<zip> ...]
"""

import fnmatch
import json
import os
import posixpath
import re
import stat
import struct
import sys
import zipfile
import zlib

ADDON_ID = "pvr.dispatcharr-unofficial"
PREFIX = "addon-" + ADDON_ID + "-"

# The plugin sources a stale plugin zip is caught against: the plugin zips are the only assets whose names carry no
# version, so only their contents can say which release they came from.
PLUGIN_SOURCE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dispatcharr-plugin")

# What a release must carry (`{v}` is the version; a name may be a glob), as the names the build harnesses and the CI
# workflow produce. CoreELEC's own packaging names its zip `pvr.dispatcharr-unofficial-<v>.<revision>.zip` (`0.12.0.1`,
# confirmed 2026-10-07 building the real release); it is renamed to the same scheme as the others when collected, since
# the file name is not what Kodi installs by (the zip's own addon.xml is).
RELEASE_ASSETS = (
    PREFIX + "{v}-windows-x86_64.zip",
    PREFIX + "{v}-linux.zip",
    PREFIX + "{v}-osx-arm64.zip",
    PREFIX + "{v}-android-aarch64.zip",
    PREFIX + "{v}-android-armv7.zip",
    PREFIX + "{v}-coreelec-aarch64.zip",  # the CoreELEC package (64-bit userland on Amlogic-no, as CoreELEC 22 builds)
    "timeshift_buffer.zip",
    "recording_edl.zip",
)

# Per addon asset: the addon.xml attribute naming its library, and what that library must be (library_kind()). The
# hand-built and hand-renamed zips (macOS, both Android ABIs, CoreELEC) are where an armv7 library ends up under the
# aarch64 name, and nothing else looks before the release is public.
ADDON_LIBRARIES = {
    "-windows-x86_64.zip": ("library_windows", "PE x86-64"),
    "-linux.zip": ("library_linux", "ELF64 x86-64"),
    "-osx-arm64.zip": ("library_osx", "Mach-O arm64"),
    "-android-aarch64.zip": ("library_android", "ELF64 AArch64"),
    "-android-armv7.zip": ("library_android", "ELF32 ARM"),
    "-coreelec-aarch64.zip": ("library_linux", "ELF64 AArch64"),
}

ELF_MACHINES = {3: "x86", 40: "ARM", 62: "x86-64", 183: "AArch64"}
PE_MACHINES = {0x14C: "x86", 0x8664: "x86-64", 0xAA64: "ARM64"}
MACHO_CPUS = {0x01000007: "x86-64", 0x0100000C: "arm64"}

# Patterns that say a path of the build machine reached the zip. Matched against the bytes decoded as latin-1 (one
# character per byte) and as UTF-16LE at both alignments. The drive-path pattern wants a non-alphanumeric before the
# letter and real path characters after the backslash, because a bare `x:\` turns up by chance in machine code
# (seen 2026-10-07 in the stripped-or-not Android libraries) and a gate that cries wolf gets edited around.
FORBIDDEN_PATTERNS = (
    ("a /home/ path", r"/home/"),
    ("a /Users/ path", r"/Users/"),
    ("a /root/ path", r"/root/"),
    ("a /var/folders/ path", r"/var/folders/"),
    # Removable and mounted volumes name their user or their disk: at the root of a path ("/mnt/media/" is a directory
    # of that name, as in recording_edl's own docstring examples) and with a component after it (timeshift_buffer
    # mentions Dispatcharr's own "/media/" URL route).
    ("a /media/ path", r"(?<![\w.\-/])/media/[^/\s'\"<>\x00.][^/\s'\"<>\x00]*/"),
    ("a /run/media/ path", r"(?<![\w.\-/])/run/media/[^/\s'\"<>\x00.][^/\s'\"<>\x00]*/"),
    ("a /Volumes/ path", r"(?<![\w.\-/])/Volumes/[^/\s'\"<>\x00.][^/\s'\"<>\x00]*/"),
    ("a CI runner work directory", r"/actions-runner|/__w/|/runner/work|/runners?/_work"),
    # After the first character the separator counts as a path character: a GitHub Windows runner works in
    # `D:\a\<repo>`, whose first component is one letter. Never a separator first, so a `p://` cut from an inlined
    # "http://" in machine code is not a path.
    ("a Windows drive path", r"(?<![A-Za-z0-9])[A-Za-z]:\\[A-Za-z0-9_$. \-][A-Za-z0-9_$. \-\\]+"),
    ("a Windows drive path", r"(?<![A-Za-z0-9])[A-Za-z]:/[A-Za-z0-9_$.\-][A-Za-z0-9_$.\-/]+"),
    # Exactly two backslashes: Kodi's prebuilt libcurl.dll has a long run of them in front of an OpenSSL source path.
    ("a UNC path", r"(?<!\\)\\\\[A-Za-z0-9][A-Za-z0-9_.$\-]+\\[A-Za-z0-9_.$ \-]+"),
    ("a Windows user directory", r"\\Users\\"),
)

# Strings that match a pattern above but are upstream defaults baked into a library the zip bundles, not paths of
# the machine that built it. (member-name regex, matched-text regex, why it is safe)
ALLOWED_PATH_STRINGS = (
    (
        r"(^|/)(libcrypto|libssl|libcurl)[^/]*\.dll$",
        r"C:\\Program Files(?: \(x86\))?\\Common Files\\SSL|C:\\Program Files(?: \(x86\))?\\OpenSSL[^\x00]*",
        "OpenSSL's compiled-in default configuration directory, the same in every build of it",
    ),
    (
        r"(^|/)(libcurl|zlib)\.dll$",
        r"[Cc]:\\code\\kodi-deps\\(?:[Bb]uild|package)\\x64",
        "the build and install paths of Kodi's own dependency build (its `Build\\x64` and `package\\x64` trees), baked "
        "into the prebuilt DLLs fetched from Kodi's mirror (confirmed 2026-10-07 in the 0.12.0 Windows zip, OpenSSL's "
        "engines directory and the sources curl, nghttp2 and zlib were built from): identical in every copy of them, "
        "not this project's machine",
    ),
)

BAD_MEMBER_SUFFIXES = (".pdb", ".debug", ".map", ".log", ".dsym")

# What a desktop leaves beside the files it showed: Finder's `.DS_Store` (which lists the folder's names, deleted ones
# too) and its AppleDouble `._*` and `__MACOSX/` companions, Explorer's `Thumbs.db` and `desktop.ini`. Never part of a
# release, so their arrival means the zip was made from a folder somebody browsed.
DESKTOP_METADATA = (".ds_store", "thumbs.db", "desktop.ini")

# What a plugin zip leaves out of its source folder, as build.yml's zip step does (`-x "$plugin/tests/*"`, at the top
# only, and `-x "*/__pycache__/*"`, at any depth).
PLUGIN_TESTS, PYCACHE = "tests", "__pycache__"

# What reading a member can raise for a damaged or unusual zip; RuntimeError is zipfile's for an encrypted entry.
READ_ERRORS = (zipfile.BadZipFile, zlib.error, NotImplementedError, RuntimeError, OSError)

# An archive inside a release zip is never scanned, so nothing may hide in one. By name, and by its first bytes for a
# member named otherwise (zip, gzip, bzip2, xz, 7z; tar has its mark at offset 257).
ARCHIVE_SUFFIXES = (".zip", ".jar", ".apk", ".whl", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".7z", ".zst")
ARCHIVE_MAGIC = (b"PK\x03\x04", b"\x1f\x8b", b"BZh", b"\xfd7zXZ\x00", b"7z\xbc\xaf\x27\x1c", b"\x28\xb5\x2f\xfd")

ADDON_XML_VERSION = re.compile(rb"<addon\b[^>]*?\sversion=\"([^\"]+)\"")
ADDON_XML_ID = re.compile(rb"<addon\b[^>]*?\sid=\"([^\"]+)\"")
# The Plugin class attribute, not the HTTP handler's server_version string (read as tools/tests/test_version_sync.py
# reads it).
PLUGIN_CLASS_VERSION = re.compile(r"^class Plugin:\n(?:.*\n)*?\s+version = \"([^\"]+)\"", re.MULTILINE)


class BlocklistError(Exception):
    pass


def load_blocklist(path):
    """The terms of a blocklist file: one per line, `#` comments and blank lines skipped. UTF-8 (a byte-order mark is
    dropped, not kept on the first term); a file with NUL bytes (UTF-16, or not text at all) is refused, since its
    terms would never match anything."""
    with open(path, "rb") as handle:
        raw = handle.read()
    if b"\x00" in raw:
        raise BlocklistError("%s has NUL bytes (UTF-16, or not a text file); save it as UTF-8" % path)
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as error:
        raise BlocklistError("%s is not UTF-8 (%s)" % (path, error)) from None
    terms = []
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            terms.append(line)
    return terms


def find_blocklist(explicit):
    """The blocklist to use. An explicit path must exist: a mistyped one falling back to another list would pass a
    release against a list nobody chose."""
    if explicit:
        if not os.path.isfile(explicit):
            raise BlocklistError("--blocklist %s: no such file" % explicit)
        return explicit
    # A set $PRIVACY_BLOCKLIST is a choice as much as --blocklist is, and the release recipe sets it rather than pass
    # the flag, so a missing file there is an error too, not a fall back to the home list.
    environment = os.environ.get("PRIVACY_BLOCKLIST")
    if environment:
        if not os.path.isfile(environment):
            raise BlocklistError("$PRIVACY_BLOCKLIST %s: no such file" % environment)
        return environment
    home = os.path.expanduser("~/.privacy-blocklist.txt")
    return home if os.path.isfile(home) else None


def _views(data):
    """The three text views a string can hide in: one character per byte, and UTF-16LE at both alignments."""
    yield "text", data.decode("latin-1")
    yield "UTF-16", data.decode("utf-16-le", errors="ignore")
    yield "UTF-16", data[1:].decode("utf-16-le", errors="ignore")


def _context(text, start, end):
    snippet = text[max(0, start - 12) : end + 24]
    return "".join(c if 32 <= ord(c) < 127 else "." for c in snippet)


def scan_member(name, data, blocklist_terms):
    """Problems found in one member's bytes (a list of strings)."""
    problems = []
    allowed = [re.compile(match) for pattern, match, _ in ALLOWED_PATH_STRINGS if re.search(pattern, name)]
    patterns = [(label, re.compile(regex)) for label, regex in FORBIDDEN_PATTERNS]
    # Each term twice: as characters for the UTF-16 views, and as its UTF-8 bytes for the one-character-per-byte view,
    # where a non-ASCII term written as characters never matched UTF-8 content. Case folds only ASCII letters there,
    # since the other "characters" are pieces of multi-byte sequences.
    wide_terms = [re.compile(re.escape(term), re.IGNORECASE) for term in blocklist_terms]
    byte_terms = [
        re.compile(re.escape(term.encode("utf-8").decode("latin-1")), re.IGNORECASE | re.ASCII)
        for term in blocklist_terms
    ]
    seen = set()
    for kind, text in _views(data):
        terms = byte_terms if kind == "text" else wide_terms
        for label, regex in patterns:
            for match in regex.finditer(text):
                if any(a.match(text, match.start()) for a in allowed):
                    continue
                key = (label, text[match.start() : match.end() + 60])
                if key in seen:
                    continue
                seen.add(key)
                problems.append("%s: %s (%s) near %r" % (name, label, kind, _context(text, match.start(), match.end())))
        for index, regex in enumerate(terms):
            match = regex.search(text)
            if match and ("term", index) not in seen:
                seen.add(("term", index))
                near = _context(text, match.start(), match.end())
                problems.append("%s: a blocklist term (%s) near %r" % (name, kind, near))
    return problems


def elf_debug_sections(data):
    """The names of the `.debug_*`/`.zdebug_*`/`.symtab` sections of an ELF image (empty for anything else or a
    stripped one)."""
    if data[:4] != b"\x7fELF" or len(data) < 64:
        return []
    is64 = data[4] == 2
    little = data[5] != 2
    order = "<" if little else ">"
    try:
        if is64:
            (shoff,) = struct.unpack_from(order + "Q", data, 0x28)
            shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", data, 0x3A)
        else:
            (shoff,) = struct.unpack_from(order + "I", data, 0x20)
            shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", data, 0x2E)
        if shoff == 0 or shnum == 0:
            return []

        def section(index):
            base = shoff + index * shentsize
            if is64:
                name, _, _, _, offset, size = struct.unpack_from(order + "IIQQQQ", data, base)
            else:
                name, _, _, _, offset, size = struct.unpack_from(order + "IIIIII", data, base)
            return name, offset, size

        _, str_offset, str_size = section(shstrndx)
        table = data[str_offset : str_offset + str_size]
        found = []
        for index in range(shnum):
            name_offset = section(index)[0]
            end = table.find(b"\x00", name_offset)
            name = table[name_offset:end].decode("latin-1")
            if name.startswith((".debug_", ".zdebug_")) or name == ".symtab":
                found.append(name)
        return found
    except (struct.error, IndexError):
        return ["<unreadable ELF section table>"]


def _local_header_extra_length(handle, info):
    """The extra-field length in an entry's own local header, which a zip may fill while leaving the central
    directory's copy (what zipfile reports as `info.extra`) empty. None when the header cannot be read."""
    handle.seek(info.header_offset)
    header = handle.read(30)
    if len(header) < 30 or header[:4] != b"PK\x03\x04":
        return None
    return struct.unpack_from("<H", header, 28)[0]


def _dos_time_and_date(date_time):
    """`info.date_time` back in the two 16-bit DOS fields it was read from (exact: zipfile's reading loses nothing)."""
    year, month, day, hour, minute, second = date_time
    return (hour << 11) | (minute << 5) | (second // 2), ((year - 1980) << 9) | (month << 5) | day


def _layout_problems(base, data, archive):
    """Bytes of the file that belong to no entry, the central directory or its end record (prepended data, a gap
    between entries or before the directory, all of which zipfile, Kodi and unzip skip over, so a scan of the entries
    alone never reads them), and local headers that name their entry differently from the central directory."""
    end_record = len(data) - 22 - len(archive.comment)
    if data[end_record : end_record + 4] != b"PK\x05\x06" or data[end_record - 20 : end_record - 16] == b"PK\x06\x07":
        # zip64 only appears past 65535 entries or 2 GiB, which no release zip comes near
        return ["%s: has no plain end-of-directory record at its end (zip64, trailing data or damaged)" % base]
    (directory_size,) = struct.unpack_from("<I", data, end_record + 12)
    spans = [(archive.start_dir, archive.start_dir + directory_size), (end_record, len(data))]
    problems, listed = [], 0
    for info in archive.infolist():
        name = info.orig_filename.encode("utf-8" if info.flag_bits & 0x800 else "cp437")
        listed += 46 + len(name) + len(info.extra) + len(info.comment)
        start = info.header_offset
        header = data[start : start + 30]
        if len(header) < 30 or header[:4] != b"PK\x03\x04":
            continue  # check_zip reports it
        (flags,) = struct.unpack_from("<H", header, 6)
        name_length, extra_length = struct.unpack_from("<HH", header, 26)
        if data[start + 30 : start + 30 + name_length] != name:
            problems.append("%s: entry %s has a different name in its local header" % (base, info.filename))
        # zipfile reports only the central directory's time, which the one-timestamp rule reads; a local header can
        # carry another (the build machine's clock) and unzip shows that one.
        if struct.unpack_from("<HH", header, 10) != _dos_time_and_date(info.date_time):
            problems.append(
                "%s: entry %s has a different time in its local header than in the central directory"
                % (base, info.filename)
            )
        end = start + 30 + name_length + extra_length + info.compress_size
        if flags & 0x08:  # a data descriptor follows, with or without its optional signature
            end += 16 if data[end : end + 4] == b"PK\x07\x08" else 12
        spans.append((start, end))
    if listed != directory_size:
        problems.append(
            "%s: its central directory is %d byte(s) longer than its entries" % (base, directory_size - listed)
        )
    position = 0
    for start, end in sorted(spans):
        if start > position:
            problems.append("%s: %d byte(s) at offset %d belong to no entry" % (base, start - position, position))
        position = max(position, end)
    return problems


def _is_symlink(info):
    return info.create_system == 3 and stat.S_ISLNK(info.external_attr >> 16)


def _escapes(target):
    """Whether a symlink target leaves the zip: absolute, a drive or backslash path, or climbing with `..`."""
    return target.startswith("/") or "\\" in target or re.match(r"[A-Za-z]:", target) or ".." in target.split("/")


def _is_archive(name, data):
    lowered = name.lower()
    return lowered.endswith(ARCHIVE_SUFFIXES) or data.startswith(ARCHIVE_MAGIC) or data[257:262] == b"ustar"


def check_zip(path, blocklist_terms):
    """All the problems found in one zip (a list of strings; empty means it passes)."""
    base = os.path.basename(path)
    problems = []
    try:
        archive = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as error:
        return ["%s: cannot be read as a zip (%s)" % (base, error)]
    with archive, open(path, "rb") as raw:
        if archive.comment:
            problems.append("%s: has a zip comment" % base)
        problems += _layout_problems(base, raw.read(), archive)
        stamps = {info.date_time for info in archive.infolist()}
        if len(stamps) > 1:
            problems.append(
                "%s: its entries carry %d different timestamps (not normalised; run tools/normalize_zip.py)"
                % (base, len(stamps))
            )
        for info in archive.infolist():
            name = info.filename
            if info.extra:
                problems.append("%s: entry %s carries an extra field (timezone or uid/gid)" % (base, name))
            local_extra = _local_header_extra_length(raw, info)
            if local_extra is None:
                problems.append("%s: entry %s has no readable local header" % (base, name))
            elif local_extra and not info.extra:
                problems.append("%s: entry %s carries an extra field in its local header only" % (base, name))
            if name.startswith("/") or ".." in name.split("/") or "\\" in name or re.match(r"[A-Za-z]:", name):
                problems.append(
                    "%s: entry name %r is absolute, climbs out, names a drive or uses a backslash" % (base, name)
                )
            lowered = name.lower().rstrip("/")
            if lowered.endswith(BAD_MEMBER_SUFFIXES) or any(p.endswith(".dsym") for p in lowered.split("/")):
                problems.append("%s: entry %s is a debug or log file" % (base, name))
            parts = lowered.split("/")
            if parts[-1] in DESKTOP_METADATA or parts[-1].startswith("._") or "__macosx" in parts:
                problems.append("%s: entry %s is a desktop's folder metadata (Finder or Explorer)" % (base, name))
            for term in blocklist_terms:
                if term.lower() in lowered:
                    problems.append("%s: entry name %s contains a blocklist term" % (base, name))
                    break
            if info.comment:
                problems.append("%s: entry %s has a comment" % (base, name))
            if info.is_dir():
                if info.file_size or info.compress_size:
                    problems.append("%s: directory entry %s carries data" % (base, name))
                continue
            if info.flag_bits & 0x1:
                # zipfile raises RuntimeError for it, which once ended the run with a traceback and the rest unchecked
                problems.append("%s: entry %s is encrypted, so it cannot be checked" % (base, name))
                continue
            try:
                data = archive.read(info)
            except READ_ERRORS as error:
                problems.append("%s: entry %s cannot be read (%s)" % (base, name, error))
                continue
            if _is_symlink(info) and _escapes(data.decode("utf-8", errors="replace")):
                problems.append("%s: entry %s is a symlink out of the zip (to %r)" % (base, name, data[:80]))
            if _is_archive(name, data):
                problems.append("%s: entry %s is an archive, whose contents this does not check" % (base, name))
            for line in scan_member(name, data, blocklist_terms):
                problems.append("%s: %s" % (base, line))
            sections = elf_debug_sections(data)
            if sections:
                problems.append("%s: %s is not stripped (%s)" % (base, name, ", ".join(sorted(set(sections))[:4])))
    return problems


def check_release_set(paths, version):
    """Problems with the set of zips as a release's assets (missing or unexpected files)."""
    names = [os.path.basename(p) for p in paths]
    patterns = [pattern.format(v=version) for pattern in RELEASE_ASSETS]
    problems = []
    for pattern in patterns:
        matches = [n for n in names if fnmatch.fnmatchcase(n, pattern)]
        if not matches:
            problems.append("release asset missing: %s" % pattern)
        elif len(matches) > 1:
            problems.append("release asset given twice: %s (%s)" % (pattern, ", ".join(matches)))
    problems += [
        "unexpected release asset: %s" % n for n in names if not any(fnmatch.fnmatchcase(n, pat) for pat in patterns)
    ]
    return problems


def library_kind(data):
    """What binary `data` is, as ADDON_LIBRARIES spells it ("ELF64 AArch64", "PE x86-64", "Mach-O arm64")."""
    if data[:4] == b"\x7fELF" and len(data) >= 20:
        (machine,) = struct.unpack_from(">H" if data[5] == 2 else "<H", data, 18)
        return "ELF%d %s" % (64 if data[4] == 2 else 32, ELF_MACHINES.get(machine, "machine %d" % machine))
    if data[:2] == b"MZ" and len(data) >= 0x40:
        (offset,) = struct.unpack_from("<I", data, 0x3C)
        if data[offset : offset + 4] == b"PE\x00\x00" and len(data) >= offset + 6:
            (machine,) = struct.unpack_from("<H", data, offset + 4)
            return "PE %s" % PE_MACHINES.get(machine, "machine 0x%x" % machine)
    if data[:4] == b"\xcf\xfa\xed\xfe" and len(data) >= 8:
        (cpu,) = struct.unpack_from("<I", data, 4)
        return "Mach-O %s" % MACHO_CPUS.get(cpu, "cpu 0x%x" % cpu)
    return "neither an ELF, a PE nor a 64-bit Mach-O library"


def _read(archive, name):
    """A member's bytes, following symlinks within the zip (the Linux and macOS zips carry a version chain of them),
    or None when it is missing or unreadable."""
    for _ in range(8):
        try:
            info = archive.getinfo(name)
            data = archive.read(info)
        except (KeyError,) + READ_ERRORS:
            return None
        if not _is_symlink(info):
            return data
        name = posixpath.normpath(posixpath.join(posixpath.dirname(name), data.decode("utf-8", errors="replace")))
    return None


def _outside(base, archive, folder):
    strays = [n for n in archive.namelist() if not n.startswith(folder + "/")]
    if not strays:
        return []
    return [
        "%s: holds %s outside its %s/ folder" % (base, ", ".join(strays[:3]) + (" ..." if strays[3:] else ""), folder)
    ]


def _xml_attribute(xml, regex):
    match = regex.search(xml)
    return match.group(1).decode("utf-8", errors="replace") if match else None


def _addon_contents(base, archive, version):
    problems = _outside(base, archive, ADDON_ID)
    xml = _read(archive, ADDON_ID + "/addon.xml")
    if xml is None:
        return problems + ["%s: no %s/addon.xml" % (base, ADDON_ID)]
    found_id, found = _xml_attribute(xml, ADDON_XML_ID), _xml_attribute(xml, ADDON_XML_VERSION)
    if found_id != ADDON_ID:
        problems.append("%s: addon.xml is addon %s, not %s" % (base, found_id, ADDON_ID))
    # CoreELEC's package appends its revision to the addon version (0.14.0.1 in 0.14.0's zip)
    wanted = re.escape(version) + (r"\.\d+" if "-coreelec-" in base else "")
    if found is None:
        problems.append("%s: no addon.xml with a version in it" % base)
    elif not re.fullmatch(wanted, found):
        problems.append("%s: addon.xml says version %s, not %s" % (base, found, version))
    suffix = next((s for s in ADDON_LIBRARIES if base.endswith(s)), None)
    if suffix is None:
        return problems  # check_release_set reports an asset it does not know
    attribute, wanted_kind = ADDON_LIBRARIES[suffix]
    library = _xml_attribute(xml, re.compile(rb"\s" + attribute.encode() + rb"=\"([^\"]+)\""))
    data = _read(archive, ADDON_ID + "/" + library) if library else None
    if library is None:
        problems.append("%s: addon.xml names no %s" % (base, attribute))
    elif data is None:
        problems.append("%s: the library its addon.xml names (%s) is not in the zip" % (base, library))
    elif library_kind(data) != wanted_kind:
        problems.append("%s: %s is %s, not the %s its name promises" % (base, library, library_kind(data), wanted_kind))
    return problems


def _manifest_version(manifest):
    try:
        return json.loads(manifest)["version"] if manifest else None
    except (ValueError, KeyError, TypeError):
        return None


def _plugin_contents(base, archive, plugin_source):
    plugin = base[: -len(".zip")]
    problems = _outside(base, archive, plugin)
    manifest, code = _read(archive, plugin + "/plugin.json"), _read(archive, plugin + "/plugin.py")
    declared = _manifest_version(manifest)
    match = PLUGIN_CLASS_VERSION.search(code.decode("utf-8", errors="replace")) if code else None
    running = match.group(1) if match else None
    if declared is None or running is None:
        return problems + ["%s: no plugin.json version or Plugin class version found" % base]
    if declared != running:
        problems.append(
            "%s: plugin.json says %s but the Plugin class says %s (the class is what Dispatcharr runs)"
            % (base, declared, running)
        )
    source = os.path.join(plugin_source, plugin, "plugin.json")
    try:
        with open(source, "rb") as handle:
            expected = _manifest_version(handle.read())
    except OSError:
        expected = None
    if expected is None:
        problems.append(
            "%s: no version in %s to compare it with (run the gate from the release's own tree, with "
            "dispatcharr-plugin/ beside tools/)" % (base, source)
        )
    elif declared != expected:
        problems.append(
            "%s: is version %s, but this tree's dispatcharr-plugin/%s/plugin.json says %s (a zip left over from "
            "another release?)" % (base, declared, plugin, expected)
        )
    else:
        problems += _plugin_differences(base, archive, plugin_source, plugin)
    return problems


def _plugin_source_files(plugin_source, plugin):
    """{member name: bytes} of what a zip of the plugin's source folder holds, less what build.yml leaves out."""
    files = {}
    for root, dirs, names in os.walk(os.path.join(plugin_source, plugin)):
        relative = os.path.relpath(root, plugin_source).replace(os.sep, "/")
        dirs[:] = [d for d in dirs if d != PYCACHE and not (relative == plugin and d == PLUGIN_TESTS)]
        for name in names:
            with open(os.path.join(root, name), "rb") as handle:
                files[relative + "/" + name] = handle.read()
    return files


def _names(names):
    names = sorted(names)
    return ", ".join(names[:3]) + (" ..." if names[3:] else "")


def _plugin_differences(base, archive, plugin_source, plugin):
    """A plugin zip's files against the gate's own tree, byte for byte. The version alone let a working tree's
    uncommitted edit, or a file lying beside the plugin, ship under the release's name (the refresh recipe zipped a
    working tree); the gate's tree is exported from the tag, so it holds exactly what was committed."""
    expected = _plugin_source_files(plugin_source, plugin)
    found = {info.filename: info for info in archive.infolist() if not info.is_dir()}
    problems = []
    extra, missing = set(found) - set(expected), set(expected) - set(found)
    if extra:
        problems.append("%s: holds %s, which this tree's dispatcharr-plugin/ does not" % (base, _names(extra)))
    if missing:
        problems.append("%s: lacks %s, which this tree's dispatcharr-plugin/ has" % (base, _names(missing)))
    differing = []
    for name in set(found) & set(expected):
        try:
            same = archive.read(found[name]) == expected[name]
        except READ_ERRORS:
            same = False  # check_zip reports why
        if not same:
            differing.append(name)
    if differing:
        problems.append(
            "%s: %s differ(s) from this tree's dispatcharr-plugin/ (built from an uncommitted or other tree?)"
            % (base, _names(differing))
        )
    return problems


def _stamp(date_time):
    return "%04d-%02d-%02d %02d:%02d:%02d" % date_time


def check_release_contents(paths, version, plugin_source=None):
    """Problems with what the release's zips say about themselves (the module docstring lists them)."""
    problems = []
    addon_stamps, plugin_stamps = {}, {}
    for path in paths:
        base = os.path.basename(path)
        try:
            archive = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError):
            continue  # check_zip reports it
        with archive:
            stamps = {info.date_time for info in archive.infolist()}
            if base.startswith(PREFIX):
                addon_stamps[base] = stamps
                problems += _addon_contents(base, archive, version)
            elif base.endswith(".zip"):
                plugin_stamps[base] = stamps
                problems += _plugin_contents(base, archive, plugin_source or PLUGIN_SOURCE)
    shared = set().union(*addon_stamps.values()) if addon_stamps else set()
    if len(shared) > 1:
        problems.append(
            "the addon zips carry %d different timestamps; they are built from one tag and normalised to its time"
            % len(shared)
        )
    elif shared:
        (built,) = shared
        for base, stamps in sorted(plugin_stamps.items()):
            if stamps and min(stamps) < built:
                problems.append(
                    "%s: dated %s, before the addon zips' %s (a zip left over from an earlier release?)"
                    % (base, _stamp(min(stamps)), _stamp(built))
                )
    return problems


def main(argv):
    args = argv[1:]
    blocklist_file, require, version, paths = None, False, None, []
    while args:
        arg = args.pop(0)
        if arg == "--blocklist" and args:
            blocklist_file = args.pop(0)
        elif arg == "--require-blocklist":
            require = True
        elif arg == "--expect-release" and args:
            version = args.pop(0)
        elif arg.startswith("--"):
            print("unknown option %s" % arg, file=sys.stderr)
            return 2
        else:
            paths.append(arg)
    if not paths:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        found = find_blocklist(blocklist_file)
        terms = load_blocklist(found) if found else []
    except (BlocklistError, OSError) as error:
        print(error, file=sys.stderr)
        return 2
    if found is None and require:
        print(
            "no privacy blocklist (--blocklist FILE, $PRIVACY_BLOCKLIST or ~/.privacy-blocklist.txt)", file=sys.stderr
        )
        return 2
    if found and not terms and require:
        print("the privacy blocklist %s has no terms, so it blocks nothing" % found, file=sys.stderr)
        return 2
    problems = check_release_set(paths, version) + check_release_contents(paths, version) if version else []
    for path in paths:
        problems += check_zip(path, terms)
    for line in problems:
        print(line, file=sys.stderr)
    if problems:
        print("FAILED: %d problem(s) in %d zip(s)" % (len(problems), len(paths)), file=sys.stderr)
        return 1
    print(
        "%d zip(s) clean (%s)"
        % (len(paths), "patterns and %d blocklist terms" % len(terms) if found else "patterns only, no blocklist")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
