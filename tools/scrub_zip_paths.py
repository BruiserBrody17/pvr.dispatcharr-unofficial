#!/usr/bin/env python3
"""Removes the build machine's directory prefix from every member of a zip, without changing any length or offset.

Some libraries a hand-built zip links statically carry the paths of the machine that built *them*, which stripping
cannot remove because they are string data, not debug information: nghttp2's assertion messages name their source
files, and OpenSSL records its configure line, compiler and install directories (confirmed 2026-10-07 in the Android
zips, built against Kodi's own dependency tree under the builder's home directory). Rebuilding that tree under a
neutral path takes hours, so after the link:

- in a binary member (one containing NUL bytes), each C string that contains the prefix is replaced whole by `/build`
  and NUL padding to the same size, so what remains is neither the path nor the build tree's layout (the first
  version overwrote only the prefix and left `/build//////android-build/kodi-source-arm/...` behind, which still
  described the builder's directory layout);
- in a text member, each occurrence of the prefix is replaced by `/build` padded with slashes to the same length.

Same size either way, so every offset stays valid and the binary is the same program: the text is only ever an error
message, a configure line or a directory a device does not have. A binary string that is not plain text, or is
absurdly long, is a refusal, never a guess: this must not blank code. `tools/check_release_zip.py` then verifies the
result, which is the point: this is a tidy-up, the gate decides.

The prefix is a private value (a home directory), so it is an argument and never stored in the repository.

usage: scrub_zip_paths.py --prefix PATH [--prefix PATH ...] <zip> [<zip> ...]     rewrite each zip in place
"""

import os
import sys
import tempfile
import zipfile

MIN_PREFIX_LENGTH = 6  # "/build" is the shortest replacement; anything shorter would need to change the length


def replacement_for(prefix: bytes) -> bytes:
    """`/build` padded with slashes to the prefix's length (repeated slashes are one slash to every path lookup)."""
    return b"/build" + b"/" * (len(prefix) - len(b"/build"))


def check_prefix(prefix: str) -> bytes:
    raw = prefix.rstrip("/").encode()
    if not raw.startswith(b"/") or len(raw) < MIN_PREFIX_LENGTH:
        raise ValueError(
            "a prefix must be an absolute path of at least %d characters, got %r" % (MIN_PREFIX_LENGTH, prefix)
        )
    return raw


MAX_BLANKED_STRING = 16384  # the longest C string this will blank (OpenSSL's configure line is a couple of KB)
MIN_PRINTABLE_FRACTION = 0.95


def _blank_c_strings(data: bytes, prefixes: list) -> tuple:
    """(new bytes, count): every NUL-delimited string containing a prefix becomes `/build` + NULs, same size."""
    out = bytearray(data)
    count = 0
    for prefix in sorted(prefixes, key=len, reverse=True):
        start = 0
        while True:
            at = out.find(prefix, start)
            if at < 0:
                break
            begin = out.rfind(b"\x00", 0, at) + 1
            end = out.find(b"\x00", at)
            if end < 0:
                end = len(out)
            span = out[begin:end]
            if len(span) > MAX_BLANKED_STRING:
                raise ValueError("a string containing the prefix is %d bytes long; refusing to blank it" % len(span))
            printable = sum(1 for b in span if b in (9, 10, 13) or 32 <= b < 127)
            if printable < MIN_PRINTABLE_FRACTION * len(span):
                raise ValueError(
                    "the bytes around an occurrence of the prefix are not plain text; refusing to blank them"
                )
            out[begin:end] = (b"/build" + bytes(len(span)))[: len(span)] if len(span) >= 6 else bytes(len(span))
            count += 1
            start = end
    return bytes(out), count


def scrub_bytes(data: bytes, prefixes: list) -> tuple:
    """(new bytes, count). A binary member (any NUL byte) has each string containing a prefix blanked whole; a text
    member has the prefix itself overwritten (longer prefixes first, so one containing another is not half-replaced)."""
    if b"\x00" in data:
        return _blank_c_strings(data, prefixes)
    count = 0
    for prefix in sorted(prefixes, key=len, reverse=True):
        found = data.count(prefix)
        if found:
            data = data.replace(prefix, replacement_for(prefix))
            count += found
    return data, count


def scrub_zip(path: str, prefixes: list) -> int:
    """Rewrites `path` in place. Returns the number of replacements made across all members."""
    total = 0
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(suffix=".zip.tmp", dir=directory)
    os.close(fd)
    try:
        with zipfile.ZipFile(path) as source, zipfile.ZipFile(tmp, "w") as target:
            for info in source.infolist():
                copy = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                copy.compress_type = info.compress_type
                copy.external_attr = info.external_attr
                copy.create_system = info.create_system
                copy.extra = info.extra
                if info.is_dir():
                    target.writestr(copy, b"")
                    continue
                data, count = scrub_bytes(source.read(info.filename), prefixes)
                total += count
                target.writestr(copy, data)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return total


def main(argv: list) -> int:
    args, prefixes, paths = argv[1:], [], []
    while args:
        arg = args.pop(0)
        if arg == "--prefix" and args:
            try:
                prefixes.append(check_prefix(args.pop(0)))
            except ValueError as error:
                print(error, file=sys.stderr)
                return 2
        elif arg.startswith("--"):
            print("unknown option %s" % arg, file=sys.stderr)
            return 2
        else:
            paths.append(arg)
    if not prefixes or not paths:
        print(__doc__, file=sys.stderr)
        return 2
    for path in paths:
        print("%s: %d replacement(s)" % (os.path.basename(path), scrub_zip(path, prefixes)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
