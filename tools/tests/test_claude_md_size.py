"""CLAUDE.md is loaded in full at the start of every session; it grew to ~200 KB once, so its size is pinned."""

from pathlib import Path

CLAUDE_MD = Path(__file__).resolve().parents[2] / "CLAUDE.md"
LIMIT_BYTES = 60_000


def test_claude_md_stays_small_enough_to_load_every_session():
    size = CLAUDE_MD.stat().st_size
    assert size <= LIMIT_BYTES, (
        "CLAUDE.md is %d bytes (limit %d): move the history to docs/ (docs/TEST_COVERAGE.md for module history, "
        "docs/OPEN_ITEMS.md for fixes) and leave a pointer" % (size, LIMIT_BYTES)
    )
