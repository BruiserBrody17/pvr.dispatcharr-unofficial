"""Unit tests for tools/check_doc_refs.py.

Loaded by explicit file path (not a bare `import check_doc_refs`) to
match the same pattern this project's own plugin tests already use.

The module computes its file-location constants (REPO_ROOT/DOC_FILES/
DOCS_DIR/SRC_DIR/PLUGIN_FILES/SETTINGS_XML/BASELINE_PATH) once at import
time, pointed at this repo's own real layout -- every test below that
exercises a check_*()/load_baseline()/write_baseline() function
monkeypatches whichever of those constants that function actually reads,
pointing them at synthetic files under tmp_path instead (REPO_ROOT
itself is pinned to tmp_path for every test via an autouse fixture,
since rel() -- used to build every finding's own message -- reads it
regardless of which specific check ran), so nothing here depends on (or
can accidentally flag) this repo's own real docs/src.

Three of the regression tests below (multi-line lookback, markdown-link
citation form, bold-pseudo-heading recognition) lock in exactly the
kind of bug this checker's own module docstring says were found and
fixed in it during development -- previously with no test of any kind
guarding against a recurrence.
"""

import importlib.util
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).parent.parent / "check_doc_refs.py"
_spec = importlib.util.spec_from_file_location("check_doc_refs", _MODULE_PATH)
check_doc_refs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_doc_refs)


@pytest.fixture(autouse=True)
def _repo_root_is_tmp_path(tmp_path, monkeypatch):
    """rel() -- used to build every finding's own key/message -- reads
    REPO_ROOT regardless of which check produced the finding, so every
    test needs it pinned to its own tmp_path, not this repo's real root."""
    monkeypatch.setattr(check_doc_refs, "REPO_ROOT", tmp_path)


# ---------------------------------------------------------------------
# normalize
# ---------------------------------------------------------------------


def test_normalize_lowercases_and_strips_whitespace():
    assert check_doc_refs.normalize("  Some Heading  ") == "some heading"


def test_normalize_strips_backticks_asterisks_and_underscores():
    # Underscores are stripped too, not just backticks/asterisks -- a
    # setting-id-shaped title like "my_setting" normalizes to "mysetting".
    assert check_doc_refs.normalize("`Some_Function`") == "somefunction"
    assert check_doc_refs.normalize("**Bold Title**") == "bold title"


# ---------------------------------------------------------------------
# get_headings
# ---------------------------------------------------------------------


def test_get_headings_plain_markdown_headings(tmp_path):
    doc = tmp_path / "X.md"
    doc.write_text("# Top Heading\n\nSome text.\n\n## Sub Heading\n")

    headings = check_doc_refs.get_headings(doc)

    assert "top heading" in headings
    assert "sub heading" in headings


def test_get_headings_bold_pseudo_headings(tmp_path):
    """This project's docs use a bold-leading paragraph/bullet as a
    pseudo-heading at least as often as a real "#" heading -- both forms
    must be recognized as legitimate citation targets."""
    doc = tmp_path / "X.md"
    doc.write_text(
        "\n".join(
            [
                "- **The permission requirement**: some explanation.",
                "**Update (2026-09-08):** something changed.",
            ]
        )
    )

    headings = check_doc_refs.get_headings(doc)

    assert "the permission requirement" in headings
    assert "update (2026-09-08):" in headings


# ---------------------------------------------------------------------
# check_section_titles
# ---------------------------------------------------------------------


def _set_docs(monkeypatch, docs_dir, doc_files):
    monkeypatch.setattr(check_doc_refs, "DOCS_DIR", docs_dir)
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", doc_files)


def test_check_section_titles_no_error_for_a_real_heading(tmp_path, monkeypatch):
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    target = docs_dir / "TARGET.md"
    target.write_text("# The Real Heading\n")
    citing = docs_dir / "CITING.md"
    citing.write_text('See docs/TARGET.md\'s "The Real Heading" section for details.\n')

    _set_docs(monkeypatch, docs_dir, [citing, target])

    assert check_doc_refs.check_section_titles() == []


def test_check_section_titles_flags_a_missing_heading(tmp_path, monkeypatch):
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    target = docs_dir / "TARGET.md"
    target.write_text("# A Different Heading\n")
    citing = docs_dir / "CITING.md"
    citing.write_text('See docs/TARGET.md\'s "A Heading That Does Not Exist" section.\n')

    _set_docs(monkeypatch, docs_dir, [citing, target])

    errors = check_doc_refs.check_section_titles()

    assert len(errors) == 1
    key, message = errors[0]
    assert "A Heading That Does Not Exist" in key
    assert "TARGET.md" in key
    assert "no matching heading found" in message


def test_check_section_titles_multiline_lookback_within_a_paragraph(tmp_path, monkeypatch):
    """Regression: which docs/X.md a "..." section citation belongs to is
    often stated a line or two earlier, since this project's prose wraps
    long sentences across lines. A real bug this checker once had: only
    checking the exact line a quote falls on, missing the file mentioned
    on an earlier line of the same paragraph."""
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    target = docs_dir / "TARGET.md"
    target.write_text("# The Real Heading\n")
    citing = docs_dir / "CITING.md"
    citing.write_text(
        "\n".join(
            [
                "See docs/TARGET.md for the full account, including the",
                'confirmed-live details in its "The Real Heading" section.',
            ]
        )
    )

    _set_docs(monkeypatch, docs_dir, [citing, target])

    assert check_doc_refs.check_section_titles() == []


def test_check_section_titles_blank_line_resets_the_lookback(tmp_path, monkeypatch):
    """A blank line ends the paragraph -- a section-title citation after
    one no longer resolves against a docs/X.md mentioned before the
    blank line; it falls back to checking within its own file."""
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    target = docs_dir / "TARGET.md"
    target.write_text("# The Real Heading\n")
    citing = docs_dir / "CITING.md"
    citing.write_text(
        "\n".join(
            [
                "See docs/TARGET.md for background.",
                "",  # blank line -- resets the lookback
                '"The Real Heading" section has more detail.',
            ]
        )
    )

    _set_docs(monkeypatch, docs_dir, [citing, target])

    errors = check_doc_refs.check_section_titles()

    # Checked against CITING.md itself (no heading there), not TARGET.md.
    assert len(errors) == 1
    assert "CITING.md" in errors[0][0]


def test_check_section_titles_recognizes_markdown_link_citation_form(tmp_path, monkeypatch):
    """Regression: "[TIMESHIFT.md](TIMESHIFT.md)"-style relative links
    are a real, common citation form within docs/ itself (no "docs/"
    prefix needed there) -- not just "docs/X.md" written out in prose."""
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    target = docs_dir / "TARGET.md"
    target.write_text("# The Real Heading\n")
    citing = docs_dir / "CITING.md"
    citing.write_text('See [TARGET.md](TARGET.md)\'s "The Real Heading" section.\n')

    _set_docs(monkeypatch, docs_dir, [citing, target])

    assert check_doc_refs.check_section_titles() == []


def test_check_section_titles_skips_generic_placeholder_titles(tmp_path, monkeypatch):
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text('See the "same" section and the "..." section.\n')

    _set_docs(monkeypatch, docs_dir, [citing])

    assert check_doc_refs.check_section_titles() == []


def test_check_section_titles_tolerates_a_partial_title_match(tmp_path, monkeypatch):
    """A citation only needs to be a substring of a real heading (or vice
    versa), not an exact match -- e.g. citing "Concurrent viewers"
    against a real "## Concurrent viewers (a real, live-confirmed bug)"
    heading."""
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    target = docs_dir / "TARGET.md"
    target.write_text("## Concurrent viewers (a real, live-confirmed bug)\n")
    citing = docs_dir / "CITING.md"
    citing.write_text('See docs/TARGET.md\'s "Concurrent viewers" section.\n')

    _set_docs(monkeypatch, docs_dir, [citing, target])

    assert check_doc_refs.check_section_titles() == []


def test_check_section_titles_skips_a_nonexistent_target_file(tmp_path, monkeypatch):
    """A citation naming a docs/X.md that doesn't exist at all is
    silently skipped by this check -- not this function's job to flag a
    missing file, only a missing heading within one that exists."""
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text('See docs/DOES_NOT_EXIST.md\'s "Some Heading" section.\n')

    _set_docs(monkeypatch, docs_dir, [citing])

    assert check_doc_refs.check_section_titles() == []


# ---------------------------------------------------------------------
# check_functions
# ---------------------------------------------------------------------


def test_check_functions_no_error_when_function_exists_in_src(tmp_path, monkeypatch):
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    (src_dir / "Foo.cpp").write_text("void RealFunction() {}\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See `RealFunction()` for details.\n")

    monkeypatch.setattr(check_doc_refs, "SRC_DIR", src_dir)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    assert check_doc_refs.check_functions() == []


def test_check_functions_flags_a_missing_function(tmp_path, monkeypatch):
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    (src_dir / "Foo.cpp").write_text("void SomethingElse() {}\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See `MissingFunction()` for details.\n")

    monkeypatch.setattr(check_doc_refs, "SRC_DIR", src_dir)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    errors = check_doc_refs.check_functions()

    assert len(errors) == 1
    assert "MissingFunction" in errors[0][0]


def test_check_functions_strips_namespace_qualifier_before_matching(tmp_path, monkeypatch):
    """`Namespace::Function()` only needs the trailing name after "::" to
    match -- a doc citing the fully-qualified form still resolves against
    a plain, unqualified definition in source."""
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    (src_dir / "Foo.cpp").write_text("void RealFunction() {}\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See `SomeNamespace::RealFunction()` for details.\n")

    monkeypatch.setattr(check_doc_refs, "SRC_DIR", src_dir)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    assert check_doc_refs.check_functions() == []


def test_check_functions_uses_word_boundaries_not_substring_matching(tmp_path, monkeypatch):
    """A citation for `Get()` must not be satisfied by source that only
    contains a longer identifier like "GetAll" -- the match is anchored
    on word boundaries, not a bare substring search."""
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    (src_dir / "Foo.cpp").write_text("void GetAll() {}\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See `Get()` for details.\n")

    monkeypatch.setattr(check_doc_refs, "SRC_DIR", src_dir)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    errors = check_doc_refs.check_functions()

    assert len(errors) == 1
    assert "Get" in errors[0][0]


def test_check_functions_searches_plugin_py_files_too(tmp_path, monkeypatch):
    """Regression: an early version of this checker only searched
    src/*.cpp/*.h, missing both plugins' own plugin.py corpus entirely --
    a real, found-and-fixed gap, not a hypothetical one."""
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    plugin_file = tmp_path / "plugin.py"
    plugin_file.write_text("def _a_plugin_function():\n    pass\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See `_a_plugin_function()` for details.\n")

    monkeypatch.setattr(check_doc_refs, "SRC_DIR", src_dir)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [plugin_file])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    assert check_doc_refs.check_functions() == []


# ---------------------------------------------------------------------
# get_setting_ids / check_settings
# ---------------------------------------------------------------------


def test_get_setting_ids_reads_settings_xml_and_plugin_action_ids(tmp_path, monkeypatch):
    settings_xml = tmp_path / "settings.xml"
    settings_xml.write_text('<settings><setting id="my_kodi_setting" type="bool"/></settings>\n')
    plugin_file = tmp_path / "plugin.py"
    plugin_file.write_text('fields = [{"id": "my_plugin_setting", "label": "X"}]\n')

    monkeypatch.setattr(check_doc_refs, "SETTINGS_XML", settings_xml)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [plugin_file])

    known = check_doc_refs.get_setting_ids()

    assert "my_kodi_setting" in known
    assert "my_plugin_setting" in known


def test_check_settings_matches_setting_as_a_plain_substring(tmp_path, monkeypatch):
    """The "does this line mention 'setting'" gate is a plain substring
    check (`"setting" not in line.lower()`), not a whole-word match -- a
    token like `not_a_real_setting` triggers it purely because "setting"
    appears inside that word, not because the line separately mentions
    the word on its own."""
    settings_xml = tmp_path / "settings.xml"
    settings_xml.write_text("<settings></settings>\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("Calls `not_a_real_setting` internally.\n")

    monkeypatch.setattr(check_doc_refs, "SETTINGS_XML", settings_xml)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    errors = check_doc_refs.check_settings()
    assert len(errors) == 1


def test_check_settings_skips_lines_with_no_setting_mention_at_all(tmp_path, monkeypatch):
    settings_xml = tmp_path / "settings.xml"
    settings_xml.write_text("<settings></settings>\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("Calls `unrelated_token` internally.\n")

    monkeypatch.setattr(check_doc_refs, "SETTINGS_XML", settings_xml)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    assert check_doc_refs.check_settings() == []


def test_check_settings_no_error_for_a_known_setting_id(tmp_path, monkeypatch):
    settings_xml = tmp_path / "settings.xml"
    settings_xml.write_text('<settings><setting id="recording_pre_offset_minutes" type="number"/></settings>\n')
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See the `recording_pre_offset_minutes` setting.\n")

    monkeypatch.setattr(check_doc_refs, "SETTINGS_XML", settings_xml)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    assert check_doc_refs.check_settings() == []


def test_check_settings_flags_an_unknown_setting_id(tmp_path, monkeypatch):
    settings_xml = tmp_path / "settings.xml"
    settings_xml.write_text("<settings></settings>\n")
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    citing = docs_dir / "CITING.md"
    citing.write_text("See the `nonexistent_setting_name` setting.\n")

    monkeypatch.setattr(check_doc_refs, "SETTINGS_XML", settings_xml)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])

    errors = check_doc_refs.check_settings()

    assert len(errors) == 1
    assert "nonexistent_setting_name" in errors[0][0]


# ---------------------------------------------------------------------
# load_baseline / write_baseline
# ---------------------------------------------------------------------


def test_load_baseline_returns_empty_set_when_file_missing(tmp_path, monkeypatch):
    monkeypatch.setattr(check_doc_refs, "BASELINE_PATH", tmp_path / "does_not_exist.txt")
    assert check_doc_refs.load_baseline() == set()


def test_write_baseline_then_load_baseline_round_trips(tmp_path, monkeypatch):
    baseline_path = tmp_path / "baseline.txt"
    monkeypatch.setattr(check_doc_refs, "BASELINE_PATH", baseline_path)

    check_doc_refs.write_baseline({"doc.md|function|Foo", "doc.md|setting|bar"})

    assert check_doc_refs.load_baseline() == {"doc.md|function|Foo", "doc.md|setting|bar"}


def test_load_baseline_ignores_blank_lines(tmp_path, monkeypatch):
    baseline_path = tmp_path / "baseline.txt"
    baseline_path.write_text("key-one\n\n  \nkey-two\n")
    monkeypatch.setattr(check_doc_refs, "BASELINE_PATH", baseline_path)

    assert check_doc_refs.load_baseline() == {"key-one", "key-two"}


# ---------------------------------------------------------------------
# main
# ---------------------------------------------------------------------


def _empty_repo(tmp_path, monkeypatch):
    """Points every check_*() constant at an empty, synthetic repo layout
    under tmp_path, so main() finds nothing to flag by default."""
    docs_dir = tmp_path / "docs"
    docs_dir.mkdir()
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    settings_xml = tmp_path / "settings.xml"
    settings_xml.write_text("<settings></settings>\n")

    monkeypatch.setattr(check_doc_refs, "DOCS_DIR", docs_dir)
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [])
    monkeypatch.setattr(check_doc_refs, "SRC_DIR", src_dir)
    monkeypatch.setattr(check_doc_refs, "PLUGIN_FILES", [])
    monkeypatch.setattr(check_doc_refs, "SETTINGS_XML", settings_xml)
    monkeypatch.setattr(check_doc_refs, "BASELINE_PATH", tmp_path / "baseline.txt")


def test_main_returns_zero_when_nothing_flagged(tmp_path, monkeypatch, capsys):
    _empty_repo(tmp_path, monkeypatch)
    monkeypatch.setattr(check_doc_refs.sys, "argv", ["check_doc_refs.py"])

    assert check_doc_refs.main() == 0
    assert "no new dangling references" in capsys.readouterr().out


def test_main_returns_one_and_prints_new_findings(tmp_path, monkeypatch, capsys):
    _empty_repo(tmp_path, monkeypatch)
    docs_dir = tmp_path / "docs"
    citing = docs_dir / "CITING.md"
    citing.write_text("See `MissingFunction()` for details.\n")
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])
    monkeypatch.setattr(check_doc_refs.sys, "argv", ["check_doc_refs.py"])

    exit_code = check_doc_refs.main()
    out = capsys.readouterr().out

    assert exit_code == 1
    assert "new possibly-dangling reference" in out
    assert "MissingFunction" in out


def test_main_update_baseline_writes_current_findings_and_returns_zero(tmp_path, monkeypatch, capsys):
    _empty_repo(tmp_path, monkeypatch)
    docs_dir = tmp_path / "docs"
    citing = docs_dir / "CITING.md"
    citing.write_text("See `MissingFunction()` for details.\n")
    monkeypatch.setattr(check_doc_refs, "DOC_FILES", [citing])
    monkeypatch.setattr(check_doc_refs.sys, "argv", ["check_doc_refs.py", "--update-baseline"])

    exit_code = check_doc_refs.main()

    assert exit_code == 0
    assert check_doc_refs.load_baseline()  # the finding was written

    # A second, un-flagged run against the now-baselined finding passes.
    monkeypatch.setattr(check_doc_refs.sys, "argv", ["check_doc_refs.py"])
    assert check_doc_refs.main() == 0


def test_main_reports_resolved_baseline_entries(tmp_path, monkeypatch, capsys):
    """A baseline entry that no longer triggers (the doc was fixed, or
    the reference removed) is surfaced as a suggestion to prune it --
    not silently dropped, and not treated as a new failure either."""
    _empty_repo(tmp_path, monkeypatch)
    check_doc_refs.write_baseline({"stale.md|function|LongGoneFunction"})
    monkeypatch.setattr(check_doc_refs.sys, "argv", ["check_doc_refs.py"])

    exit_code = check_doc_refs.main()
    out = capsys.readouterr().out

    assert exit_code == 0
    assert "no longer triggering" in out
