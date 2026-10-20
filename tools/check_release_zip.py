#!/usr/bin/env python3
"""The privacy gate every release zip passes, whoever built it (CI or by hand): exits 1 when one fails.

A release zip is what a stranger downloads, so it must not say where or by whom it was built. This is the one
check CI runs on the zips it builds and the one a release is cut with for the hand-built ones (macOS, Android,
CoreELEC), so no platform gets a weaker gate than another. Per zip it checks:

- zip metadata: no extra field on any entry, in the central directory or in an entry's own local header (the
  extended-timestamp field is the build machine's timezone, the Info-ZIP Unix field is its uid/gid), one timestamp
  shared by every entry (what tools/normalize_zip.py writes: a zip made without it carries the build machine's local
  clock, whose difference from the commit or upload time is its timezone), no zip comment, no absolute, `..` or
  drive-letter entry names, no symlink pointing outside the zip, no archive inside the zip (its contents would go
  unchecked), no debug-symbol or log members (`.pdb`, anything in a `.dSYM` bundle, `.debug`, `.map`, `.log`);
- every member's bytes, as plain text and as UTF-16 (Windows binaries keep wide strings), for build-machine paths
  (`/home/`, `/Users/`, `/root/`, `/var/folders/`, `/media/`, `/run/media/`, `/Volumes/`, a CI runner's work
  directory, a `C:\\` or `C:/` path, a UNC path) except the few known-benign upstream strings in ALLOWED_PATH_STRINGS,
  each with the reason it is safe;
- every member's bytes for each term of a private blocklist (one term per line, case-insensitive; the file is
  never in the repository: `--blocklist FILE`, else `$PRIVACY_BLOCKLIST`, else `~/.privacy-blocklist.txt`);
  `--require-blocklist` fails when none is found or the one found has no terms, which is how a release is cut -- CI
  has no blocklist and checks the patterns only; a `--blocklist` path that does not exist is an error, never a
  fallback to another list;
- ELF members: no `.debug_*`, `.zdebug_*` or `.symtab` section (an unstripped library carries the build machine's
  source paths, which is how the hand-built Android libraries once shipped them).

`--expect-release VERSION` additionally fails unless the given zips are exactly the release's asset set, so a
platform cannot be left out of a release by forgetting a step, and checks what is inside them: each addon zip's
`addon.xml` must carry VERSION (CoreELEC's, VERSION plus its package revision), the six addon zips must share one
timestamp (one tag, one normalisation), and each plugin zip's `plugin.json` version must equal its `Plugin` class
version, so a stale zip renamed to the new release's name does not pass.

usage: check_release_zip.py [--blocklist FILE] [--require-blocklist] [--expect-release VERSION] <zip> [<zip> ...]
"""

import fnmatch
import json
import os
import re
import stat
import struct
import sys
import zipfile

PREFIX = "addon-pvr.dispatcharr-unofficial-"

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
    PREFIX + "{v}-coreelec-armv7.zip",  # the CoreELEC package (32-bit userland on Amlogic-ng, as CoreELEC builds it)
    "timeshift_buffer.zip",
    "recording_edl.zip",
)

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

# An archive inside a release zip is never scanned, so nothing may hide in one. By name, and by its first bytes for a
# member named otherwise (zip, gzip, bzip2, xz, 7z; tar has its mark at offset 257).
ARCHIVE_SUFFIXES = (".zip", ".jar", ".apk", ".whl", ".tar", ".gz", ".tgz", ".bz2", ".xz", ".7z", ".zst")
ARCHIVE_MAGIC = (b"PK\x03\x04", b"\x1f\x8b", b"BZh", b"\xfd7zXZ\x00", b"7z\xbc\xaf\x27\x1c", b"\x28\xb5\x2f\xfd")

ADDON_XML_VERSION = re.compile(rb"<addon\b[^>]*?\sversion=\"([^\"]+)\"")
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
    for candidate in (os.environ.get("PRIVACY_BLOCKLIST"), os.path.expanduser("~/.privacy-blocklist.txt")):
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


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
            for term in blocklist_terms:
                if term.lower() in lowered:
                    problems.append("%s: entry name %s contains a blocklist term" % (base, name))
                    break
            if info.is_dir():
                continue
            data = archive.read(info)
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


def _member(archive, suffix):
    """The bytes of the one member whose name ends with `suffix` one directory down (`<dir>/addon.xml`), or None."""
    names = [n for n in archive.namelist() if n.endswith("/" + suffix) and n.count("/") == 1]
    return archive.read(names[0]) if len(names) == 1 else None


def check_release_contents(paths, version):
    """Problems with what the release's zips say about themselves: the addon version inside each addon zip, one
    timestamp across the addon zips, and each plugin's two version strings agreeing."""
    problems = []
    addon_stamps = {}
    for path in paths:
        base = os.path.basename(path)
        try:
            archive = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError):
            continue  # check_zip reports it
        with archive:
            if base.startswith(PREFIX):
                addon_stamps[base] = {info.date_time for info in archive.infolist()}
                xml = _member(archive, "addon.xml")
                match = ADDON_XML_VERSION.search(xml) if xml else None
                found = match.group(1).decode("utf-8", errors="replace") if match else None
                # CoreELEC's package appends its revision to the addon version (0.14.0.1 in 0.14.0's zip)
                wanted = re.escape(version) + (r"\.\d+" if "-coreelec-" in base else "")
                if found is None:
                    problems.append("%s: no addon.xml with a version in it" % base)
                elif not re.fullmatch(wanted, found):
                    problems.append("%s: addon.xml says version %s, not %s" % (base, found, version))
            elif base.endswith(".zip"):
                manifest, source = _member(archive, "plugin.json"), _member(archive, "plugin.py")
                try:
                    declared = json.loads(manifest)["version"] if manifest else None
                except (ValueError, KeyError, TypeError):
                    declared = None
                match = PLUGIN_CLASS_VERSION.search(source.decode("utf-8", errors="replace")) if source else None
                running = match.group(1) if match else None
                if declared is None or running is None:
                    problems.append("%s: no plugin.json version or Plugin class version found" % base)
                elif declared != running:
                    problems.append(
                        "%s: plugin.json says %s but the Plugin class says %s (the class is what Dispatcharr runs)"
                        % (base, declared, running)
                    )
    shared = set().union(*addon_stamps.values()) if addon_stamps else set()
    if len(shared) > 1:
        problems.append(
            "the addon zips carry %d different timestamps; they are built from one tag and normalised to its time"
            % len(shared)
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
