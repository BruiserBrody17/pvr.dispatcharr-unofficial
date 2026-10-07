#!/usr/bin/env python3
"""The privacy gate every release zip passes, whoever built it (CI or by hand): exits 1 when one fails.

A release zip is what a stranger downloads, so it must not say where or by whom it was built. This is the one
check CI runs on the zips it builds and the one a release is cut with for the hand-built ones (macOS, Android,
CoreELEC), so no platform gets a weaker gate than another. Per zip it checks:

- zip metadata: no extra field on any entry (the extended-timestamp field is the build machine's timezone, the
  Info-ZIP Unix field is its uid/gid), no zip comment, no absolute or `..` entry names, no debug-symbol or log
  members (`.pdb`, `.dSYM`, `.debug`, `.map`, `.log`);
- every member's bytes, as plain text and as UTF-16 (Windows binaries keep wide strings), for build-machine paths
  (`/home/`, `/Users/`, `/root/`, `/var/folders/`, a CI runner's work directory, a `C:\\` path) except the few
  known-benign upstream strings in ALLOWED_PATH_STRINGS, each with the reason it is safe;
- every member's bytes for each term of a private blocklist (one term per line, case-insensitive; the file is
  never in the repository: `--blocklist FILE`, else `$PRIVACY_BLOCKLIST`, else `~/.privacy-blocklist.txt`);
  `--require-blocklist` fails when none is found, which is how a release is cut -- CI has no blocklist and
  checks the patterns only;
- ELF members: no `.debug_*` or `.symtab` section (an unstripped library carries the build machine's source
  paths, which is how the hand-built Android libraries once shipped them).

`--expect-release VERSION` additionally fails unless the given zips are exactly the release's asset set, so a
platform cannot be left out of a release by forgetting a step.

usage: check_release_zip.py [--blocklist FILE] [--require-blocklist] [--expect-release VERSION] <zip> [<zip> ...]
"""

import os
import re
import struct
import sys
import zipfile

PREFIX = "addon-pvr.dispatcharr-unofficial-"

# What a release must carry (`{v}` is the version), as the names the build harnesses and the CI workflow produce.
RELEASE_ASSETS = (
    PREFIX + "{v}-windows-x86_64.zip",
    PREFIX + "{v}-linux.zip",
    PREFIX + "{v}-osx-arm64.zip",
    PREFIX + "{v}-android-aarch64.zip",
    PREFIX + "{v}-android-armv7.zip",
    "pvr.dispatcharr-unofficial-{v}.zip",  # the CoreELEC package
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
    ("a CI runner work directory", r"/actions-runner|/__w/|/runner/work|/runners?/_work"),
    ("a Windows drive path", r"(?<![A-Za-z0-9])[A-Za-z]:\\[A-Za-z0-9_$. \-]{2,}"),
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


def load_blocklist(path):
    terms = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                terms.append(line)
    return terms


def find_blocklist(explicit):
    for candidate in (explicit, os.environ.get("PRIVACY_BLOCKLIST"), os.path.expanduser("~/.privacy-blocklist.txt")):
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
    terms = [re.compile(re.escape(term), re.IGNORECASE) for term in blocklist_terms]
    seen = set()
    for kind, text in _views(data):
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
    """The names of the `.debug_*`/`.symtab` sections of an ELF image (empty for anything else or a stripped one)."""
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
            if name.startswith(".debug_") or name == ".symtab":
                found.append(name)
        return found
    except (struct.error, IndexError):
        return ["<unreadable ELF section table>"]


def check_zip(path, blocklist_terms):
    """All the problems found in one zip (a list of strings; empty means it passes)."""
    base = os.path.basename(path)
    problems = []
    try:
        archive = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as error:
        return ["%s: cannot be read as a zip (%s)" % (base, error)]
    with archive:
        if archive.comment:
            problems.append("%s: has a zip comment" % base)
        for info in archive.infolist():
            name = info.filename
            if info.extra:
                problems.append("%s: entry %s carries an extra field (timezone or uid/gid)" % (base, name))
            if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                problems.append("%s: entry name %r is absolute, climbs out, or uses a backslash" % (base, name))
            lowered = name.lower().rstrip("/")
            if lowered.endswith(BAD_MEMBER_SUFFIXES):
                problems.append("%s: entry %s is a debug or log file" % (base, name))
            for term in blocklist_terms:
                if term.lower() in lowered:
                    problems.append("%s: entry name %s contains a blocklist term" % (base, name))
                    break
            if info.is_dir():
                continue
            data = archive.read(info)
            for line in scan_member(name, data, blocklist_terms):
                problems.append("%s: %s" % (base, line))
            sections = elf_debug_sections(data)
            if sections:
                problems.append("%s: %s is not stripped (%s)" % (base, name, ", ".join(sorted(set(sections))[:4])))
    return problems


def check_release_set(paths, version):
    """Problems with the set of zips as a release's assets (missing or unexpected files)."""
    names = [os.path.basename(p) for p in paths]
    expected = [pattern.format(v=version) for pattern in RELEASE_ASSETS]
    problems = ["release asset missing: %s" % name for name in expected if name not in names]
    problems += ["unexpected release asset: %s" % name for name in names if name not in expected]
    problems += ["release asset given twice: %s" % name for name in sorted(set(names)) if names.count(name) > 1]
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
    found = find_blocklist(blocklist_file)
    if found is None and require:
        print(
            "no privacy blocklist (--blocklist FILE, $PRIVACY_BLOCKLIST or ~/.privacy-blocklist.txt)", file=sys.stderr
        )
        return 2
    terms = load_blocklist(found) if found else []
    problems = check_release_set(paths, version) if version else []
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
