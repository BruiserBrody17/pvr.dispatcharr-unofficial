#!/usr/bin/env python3
"""Rewrites zip files so every entry carries one fixed UTC timestamp and no extended-timestamp field.

CPack (and Info-ZIP's `zip`) store each entry's modification time twice: as the build machine's local clock
(the DOS field) and as UTC (the "UT" extra field). The difference between the two is the build machine's
timezone, readable by anyone who downloads the release (confirmed 2026-10-07 on the CI-built Linux zip, which the
Windows leg's own PowerShell rewrite and the plugin zips' `TZ=UTC zip -X` had left unfixed there). Python's
zipfile writes only the DOS field, so rewriting each entry through it, stamped with the commit's own time in UTC,
removes the offset. Same entries, same order, same bytes, same permissions; only the metadata changes.

usage: normalize_zip.py <commit-unix-time> <zip> [<zip> ...]       rewrite each zip in place
       normalize_zip.py --check <zip> [<zip> ...]                  exit 1 if any entry still has a UT field
"""

import os
import struct
import sys
import tempfile
import time
import zipfile


def normalize(path: str, commit_time: int) -> int:
    """Rewrites `path` in place. Returns the number of entries."""
    stamp = time.gmtime(commit_time)[:6]
    if stamp[0] < 1980:  # the zip format cannot represent earlier dates
        stamp = (1980, 1, 1, 0, 0, 0)
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(suffix=".zip.tmp", dir=directory)
    os.close(fd)
    try:
        with zipfile.ZipFile(path) as source, zipfile.ZipFile(tmp, "w") as target:
            for info in source.infolist():
                copy = zipfile.ZipInfo(info.filename, date_time=stamp)
                copy.compress_type = zipfile.ZIP_DEFLATED if not info.is_dir() else zipfile.ZIP_STORED
                copy.external_attr = info.external_attr  # permissions (the executable bit) and the directory flag
                copy.create_system = info.create_system
                if info.is_dir():
                    target.writestr(copy, b"")
                else:
                    target.writestr(copy, source.read(info.filename))
            count = len(source.infolist())
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return count


def has_extended_timestamps(path: str) -> bool:
    """Whether any entry carries an extended-timestamp ("UT", header id 0x5455) extra field."""
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            extra, offset = info.extra, 0
            while offset + 4 <= len(extra):
                header_id, size = struct.unpack("<HH", extra[offset : offset + 4])
                if header_id == 0x5455:
                    return True
                offset += 4 + size
    return False


def main(argv: list) -> int:
    if len(argv) >= 2 and argv[1] == "--check":
        bad = [p for p in argv[2:] if has_extended_timestamps(p)]
        for path in bad:
            print(
                "%s still carries UT timestamp fields (the build machine's timezone)" % os.path.basename(path),
                file=sys.stderr,
            )
        if not argv[2:]:
            print("no zip given to check", file=sys.stderr)
            return 2
        return 1 if bad else 0
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        commit_time = int(argv[1])
    except ValueError:
        print("the commit time must be a unix timestamp, got %r" % argv[1], file=sys.stderr)
        return 2
    for path in argv[2:]:
        count = normalize(path, commit_time)
        stamp = time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime(commit_time))
        print("%s: %d entries stamped %s" % (os.path.basename(path), count, stamp))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
