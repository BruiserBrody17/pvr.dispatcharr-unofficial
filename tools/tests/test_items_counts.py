"""Cross-file consistency check: docs/OPEN_ITEMS.md's "Status at a glance" counts (the headline and the per-section
table) must match the entries actually in OPEN_ITEMS.md and CLOSED_ITEMS.md. They are kept by hand, one increment per
change, so an entry moved without a bump drifted them silently: by 2026-11-20 both branches' tables were one Fixed
entry short and the headline total disagreed with the table's own rows. Reads the real files, like the other
cross-file checks here.
"""

import re
from pathlib import Path

DOCS = Path(__file__).resolve().parents[2] / "docs"
OPEN_ITEMS = DOCS / "OPEN_ITEMS.md"
CLOSED_ITEMS = DOCS / "CLOSED_ITEMS.md"

_HEADLINE = re.compile(r"^\*\*(\d+) open, (\d+) closed\.\*\*", re.MULTILINE)
_TABLE_ROW = re.compile(r"^\| (Open|Closed \(in CLOSED_ITEMS\.md\)): (.+?) \| (\d+) \|$", re.MULTILINE)


def _entries_per_section(text: str, top_heading: str) -> dict[str, int]:
    """Counts the #### entries under each ### section of the given ## heading."""
    counts: dict[str, int] = {}
    in_top = False
    section = None
    for line in text.splitlines():
        if line.startswith("## "):
            in_top = line[3:].strip() == top_heading
            section = None
        elif in_top and line.startswith("### "):
            section = line[4:].strip()
            counts[section] = 0
        elif in_top and section is not None and line.startswith("#### "):
            counts[section] += 1
    return counts


def _table() -> dict[tuple[str, str], int]:
    return {(kind, name): int(n) for kind, name, n in _TABLE_ROW.findall(OPEN_ITEMS.read_text(encoding="utf-8"))}


def test_open_table_matches_open_entries():
    actual = _entries_per_section(OPEN_ITEMS.read_text(encoding="utf-8"), "Open")
    table = {name: n for (kind, name), n in _table().items() if kind == "Open"}
    assert actual, "no ### sections found under ## Open in OPEN_ITEMS.md"
    assert table == actual


def test_closed_table_matches_closed_entries():
    actual = _entries_per_section(CLOSED_ITEMS.read_text(encoding="utf-8"), "Closed")
    table = {name: n for (kind, name), n in _table().items() if kind != "Open"}
    assert actual, "no ### sections found under ## Closed in CLOSED_ITEMS.md"
    assert table == actual


def test_headline_totals_match_the_table():
    match = _HEADLINE.search(OPEN_ITEMS.read_text(encoding="utf-8"))
    assert match, "OPEN_ITEMS.md has no '**N open, M closed.**' headline"
    table = _table()
    open_total = sum(n for (kind, _), n in table.items() if kind == "Open")
    closed_total = sum(n for (kind, _), n in table.items() if kind != "Open")
    assert (int(match.group(1)), int(match.group(2))) == (open_total, closed_total)
