"""The built-in DST table in src/TimeZoneUtil.cpp against the machine's real tz database.

Found by the 2026-10-04 eighth hardening sweep: tzdata 2026c keeps British Columbia and Alberta on
their summer offset from 2026-11-01, so the hand-written table put every recurring rule in
America/Vancouver and America/Edmonton an hour off from that date, and every unit test passed because
each one pinned a hand-written expectation. This test reads the table (zone, standard offset, DST
family, and the optional permanent-daylight instant) and its aliases straight out of the C++ source,
re-states the two families' transition rules independently (US/Canada: second Sunday of March 02:00
local standard time to the first Sunday of November 02:00 local daylight time; EU: last Sunday of
March 01:00 UTC to the last Sunday of October 01:00 UTC), and compares the offset the table implies
with `zoneinfo` for every zone and alias, hourly around the transition months and daily otherwise,
over the next several years. The next tzdata change to one of these zones then fails CI instead of
shifting users' recordings.

It checks the TABLE (and the rules' assumptions) rather than running the C++, which the Catch2
suite covers with its own transition-instant tests; a compiler is not assumed here. Skipped, loudly,
when the machine has no tz database.
"""

import datetime as dt
import re
from pathlib import Path

import pytest

zoneinfo = pytest.importorskip("zoneinfo")

REPO_ROOT = Path(__file__).resolve().parents[2]
TIMEZONE_CPP = REPO_ROOT / "src" / "TimeZoneUtil.cpp"

_ROW = re.compile(r'\{\s*"([A-Za-z_]+/[A-Za-z_+\-/]+|UTC)"\s*,\s*(-?\d+)\s*,\s*DstFamily::(k\w+)\s*(?:,\s*(\w+)\s*)?\}')
_ALIAS = re.compile(r'\{\s*"([A-Za-z_]+(?:/[A-Za-z_+\-/]+)?)"\s*,\s*"([A-Za-z_]+/[A-Za-z_+\-/]+)"\s*\}')
_CONSTANT = re.compile(r"constexpr\s+time_t\s+(\w+)\s*=\s*(\d+)\s*;")

UTC = dt.timezone.utc


def _parse_table():
    text = TIMEZONE_CPP.read_text(encoding="utf-8")
    constants = {name: int(value) for name, value in _CONSTANT.findall(text)}
    table_start = text.index("kKnownTimeZones[]")
    table = text[table_start : text.index("};", table_start)]
    zones = {}
    for name, standard, family, permanent in _ROW.findall(table):
        zones[name] = (int(standard), family, constants[permanent] if permanent else 0)
    alias_start = text.index("kZoneAliases[]")
    aliases = dict(_ALIAS.findall(text[alias_start : text.index("};", alias_start)]))
    return zones, aliases


def _nth_weekday(year, month, weekday, n):
    """The date of the n-th `weekday` (Mon=0) of the month; n=-1 for the last."""
    if n > 0:
        first = dt.date(year, month, 1)
        return first + dt.timedelta(days=(weekday - first.weekday()) % 7 + 7 * (n - 1))
    nxt = dt.date(year + (month == 12), month % 12 + 1, 1)
    last = nxt - dt.timedelta(days=1)
    return last - dt.timedelta(days=(last.weekday() - weekday) % 7)


def _table_offset_minutes(standard, family, permanent_from, at):
    """What the C++ table implies at the UTC instant `at`."""
    if permanent_from and at.timestamp() >= permanent_from:
        return standard + 60
    if family == "kNone":
        return standard
    year = at.year
    if family == "kUsCanada":
        start_day = _nth_weekday(year, 3, 6, 2)
        end_day = _nth_weekday(year, 11, 6, 1)
        # 02:00 local standard time = 02:00 - standard; 02:00 local daylight time = 02:00 - (standard + 60)
        start = dt.datetime(start_day.year, start_day.month, start_day.day, 2, tzinfo=UTC) - dt.timedelta(
            minutes=standard
        )
        end = dt.datetime(end_day.year, end_day.month, end_day.day, 2, tzinfo=UTC) - dt.timedelta(minutes=standard + 60)
    elif family == "kEu":
        start_day = _nth_weekday(year, 3, 6, -1)
        end_day = _nth_weekday(year, 10, 6, -1)
        start = dt.datetime(start_day.year, start_day.month, start_day.day, 1, tzinfo=UTC)
        end = dt.datetime(end_day.year, end_day.month, end_day.day, 1, tzinfo=UTC)
    else:
        raise AssertionError(f"unknown family {family}")
    return standard + 60 if start <= at < end else standard


def _instants():
    """Hourly through the months transitions happen in, daily otherwise, 2026-2031."""
    at = dt.datetime(2026, 1, 1, tzinfo=UTC)
    stop = dt.datetime(2031, 1, 1, tzinfo=UTC)
    while at < stop:
        yield at
        at += dt.timedelta(hours=1) if at.month in (3, 4, 10, 11) else dt.timedelta(hours=12)


@pytest.fixture(scope="module")
def table():
    return _parse_table()


def _real_offset_minutes(name, at):
    return int(at.astimezone(zoneinfo.ZoneInfo(name)).utcoffset().total_seconds() // 60)


def _database_predates_2026_bc_alberta_change():
    """True when the machine's tz database still has British Columbia changing its clocks in 2027.

    The table encodes tzdata 2026c (BC and Alberta stay on daylight time from 2026-11-01). A CI runner
    image built before 2026c would fail this whole module for that reason alone -- a stale runner, not
    a table bug -- so the module is skipped there, with the reason, instead of going red (found by the
    ninth hardening sweep). Detected by behaviour rather than by parsing a version string, which not
    every tz package ships."""
    try:
        zone = zoneinfo.ZoneInfo("America/Vancouver")
    except zoneinfo.ZoneInfoNotFoundError:
        return False
    january = dt.datetime(2027, 1, 15, 12, tzinfo=UTC).astimezone(zone).utcoffset()
    return january == dt.timedelta(hours=-8)


def _require_database(name):
    try:
        zoneinfo.ZoneInfo(name)
    except zoneinfo.ZoneInfoNotFoundError:
        pytest.skip("this machine has no tz database (install tzdata)")
    if _database_predates_2026_bc_alberta_change():
        pytest.skip("this machine's tz database predates tzdata 2026c (BC/AB permanent daylight time)")


def test_the_table_was_parsed(table):
    zones, aliases = table
    assert len(zones) >= 50
    assert "America/Vancouver" in zones and zones["America/Vancouver"][2] > 0
    assert aliases["US/Eastern"] == "America/New_York"


def test_every_table_zone_matches_the_tz_database_through_2030(table):
    zones, _ = table
    _require_database("America/New_York")
    problems = []
    for name, (standard, family, permanent) in sorted(zones.items()):
        _require_database(name)
        bad = [
            at
            for at in _instants()
            if _table_offset_minutes(standard, family, permanent, at) != _real_offset_minutes(name, at)
        ]
        if bad:
            problems.append(
                f"{name}: {len(bad)} mismatching instants, first {bad[0].isoformat()} "
                f"(table {_table_offset_minutes(standard, family, permanent, bad[0])} min, "
                f"tz database {_real_offset_minutes(name, bad[0])} min)"
            )
    assert not problems, "the DST table disagrees with the tz database:\n" + "\n".join(problems)


def test_every_alias_resolves_to_a_zone_with_the_same_rules(table):
    zones, aliases = table
    _require_database("America/New_York")
    for alias, canonical in sorted(aliases.items()):
        assert canonical in zones, f"{alias} -> {canonical}: not a table zone"
        try:
            zoneinfo.ZoneInfo(alias)
        except zoneinfo.ZoneInfoNotFoundError:
            continue  # an alias this machine's database no longer carries
        standard, family, permanent = zones[canonical]
        for at in _instants():
            if at.day % 5:  # a sample is enough: the canonical zone is checked exhaustively above
                continue
            assert _table_offset_minutes(standard, family, permanent, at) == _real_offset_minutes(
                alias, at
            ), f"{alias} ({canonical}) at {at.isoformat()}"


def test_an_old_tz_database_is_recognised_so_the_module_skips_instead_of_failing(monkeypatch):
    class _Old(dt.tzinfo):
        def utcoffset(self, _):
            return dt.timedelta(hours=-8)  # Vancouver in January, before the 2026c change

        def dst(self, _):
            return dt.timedelta(0)

    class _New(_Old):
        def utcoffset(self, _):
            return dt.timedelta(hours=-7)

    monkeypatch.setattr(zoneinfo, "ZoneInfo", lambda name: _Old())
    assert _database_predates_2026_bc_alberta_change() is True
    monkeypatch.setattr(zoneinfo, "ZoneInfo", lambda name: _New())
    assert _database_predates_2026_bc_alberta_change() is False

    def _missing(name):
        raise zoneinfo.ZoneInfoNotFoundError(name)

    monkeypatch.setattr(zoneinfo, "ZoneInfo", _missing)
    assert _database_predates_2026_bc_alberta_change() is False  # no database: the other skip handles it
