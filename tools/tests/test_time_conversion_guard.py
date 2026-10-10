"""Tripwire for the 32-bit date problem (docs/CLOSED_ITEMS.md, "Nothing on a 64-bit build checks that the date
parsers use the saturating time conversion").

`SaturatingTimeGm()` makes a date from 2038 on read as the latest representable time instead of wrapping or failing
on a 32-bit `time_t` (Android's 32-bit ARM build and CoreELEC's armv7 userland are what this project ships). The unit
tests that exercise it run on a 64-bit host, where a plain `timegm()` gives the same answers, so nothing there fails
if a parser quietly goes back to the plain call. This reads the real sources instead:

- the two parsers that turn a server-supplied date into a `time_t` (`DateTimeFormat.cpp`, `XmlTvParser.cpp`) must call
  `PortableTimeGmSaturating()` and nothing narrower;
- nothing outside `TimeUtil.h` may call `timegm`, `_mkgmtime` or `mktime` directly;
- the plain `PortableTimeGm()` is used only where it is today, with the counts pinned, so a new use is a decision made
  on purpose (add it to the table with a reason) rather than a copy of the nearest example.
"""

import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src"

# Files whose dates come from the server: they must saturate.
PARSERS = ["DateTimeFormat.cpp", "XmlTvParser.cpp"]

# Where the plain PortableTimeGm() is called today, and how many times. TimeUtil.h: its definition, the saturating
# wrapper's lambda and the local-time helper; TimeZoneUtil.cpp: the daylight-saving transitions of the year of an
# instant the caller already holds as a time_t.
PLAIN_USES = {"TimeUtil.h": 3, "TimeZoneUtil.cpp": 6}


def _code(path: Path) -> str:
    """The file's text with // and /* */ comments blanked out, so a mention in a comment is not a call."""
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _calls(path: Path, name: str) -> int:
    return len(re.findall(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(", _code(path)))


def test_the_parsers_use_the_saturating_conversion_and_nothing_narrower():
    for name in PARSERS:
        path = SRC / name
        assert _calls(path, "PortableTimeGmSaturating") >= 1, f"{name} no longer calls PortableTimeGmSaturating()"
        for plain in ("PortableTimeGm", "timegm", "_mkgmtime", "mktime"):
            assert _calls(path, plain) == 0, (
                f"{name} calls {plain}(): a server date past 2038 would wrap or fail on a 32-bit time_t. "
                "Use PortableTimeGmSaturating() (TimeUtil.h)."
            )


def test_nothing_outside_timeutil_calls_the_c_library_conversion_directly():
    offenders = []
    for path in sorted(list(SRC.glob("*.cpp")) + list(SRC.glob("*.h"))):
        if path.name == "TimeUtil.h":
            continue
        for plain in ("timegm", "_mkgmtime", "mktime"):
            if _calls(path, plain):
                offenders.append(f"{path.name}: {plain}()")
    assert offenders == [], "call PortableTimeGm()/PortableTimeGmSaturating() from TimeUtil.h instead: " + ", ".join(
        offenders
    )


def test_the_plain_conversion_is_used_only_where_it_is_today():
    found = {}
    for path in sorted(list(SRC.glob("*.cpp")) + list(SRC.glob("*.h"))):
        count = _calls(path, "PortableTimeGm")
        if count:
            found[path.name] = count
    assert found == PLAIN_USES, (
        "PortableTimeGm() (not the saturating one) is called from a different set of places than the table in this "
        f"test lists: found {found}, expected {PLAIN_USES}. A new use must justify why a date past 2038 cannot reach "
        "it, then be added to PLAIN_USES."
    )


def test_the_guard_itself_notices_a_plain_call(tmp_path):
    fake = tmp_path / "Parser.cpp"
    fake.write_text("// PortableTimeGm(&x) in a comment\ntime_t t = PortableTimeGm(&tmVal);\n/* timegm(&y) */\n")
    assert _calls(fake, "PortableTimeGm") == 1  # the call counts, the comments do not
    assert _calls(fake, "timegm") == 0
    assert _calls(fake, "PortableTimeGmSaturating") == 0
