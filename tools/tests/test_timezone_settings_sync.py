"""Cross-file consistency check: the IANA zones hardcoded in
src/TimeZoneUtil.cpp's kKnownTimeZones and the `recurring_rule_timezone`
options in settings.xml must list exactly the same zones (plus the
settings-only "manual" sentinel). Drift between the two already caused one
real regression (2026-09-09) -- a zone offered in the settings dropdown
that the C++ side didn't recognize silently fell back to a stale manual
offset, and a zone known to C++ but missing from the dropdown was simply
unreachable. Follows check_doc_refs.py's own precedent for cross-file
checks living under tools/tests (docs/OPEN_ITEMS.md, 27th-pass audit).
Reads this repo's real files -- deliberately, unlike test_check_doc_refs.py,
since the real pair is exactly what's being checked.
"""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TIMEZONE_CPP = REPO_ROOT / "src" / "TimeZoneUtil.cpp"
SETTINGS_XML = REPO_ROOT / "pvr.dispatcharr-unofficial" / "resources" / "settings.xml"
STRINGS_PO = (
    REPO_ROOT / "pvr.dispatcharr-unofficial" / "resources" / "language" / "resource.language.en_gb" / "strings.po"
)

# Matches one kKnownTimeZones initializer row: {"Area/City", -300, DstFamily::kX}
_CPP_ROW = re.compile(r'\{\s*"([A-Za-z_]+/[A-Za-z_+\-/]+|UTC)"\s*,\s*-?\d+\s*,\s*DstFamily::k\w+\s*(?:,\s*\w+\s*)?\}')


def _cpp_zone_names() -> list[str]:
    text = TIMEZONE_CPP.read_text(encoding="utf-8")
    start = text.index("kKnownTimeZones[]")
    end = text.index("};", start)
    return _CPP_ROW.findall(text[start:end])


def _settings_options() -> list[tuple[str, str]]:
    """(label id, value) for every recurring_rule_timezone option."""
    root = ET.parse(SETTINGS_XML).getroot()
    for setting in root.iter("setting"):
        if setting.get("id") == "recurring_rule_timezone":
            return [(o.get("label"), (o.text or "").strip()) for o in setting.iter("option")]
    raise AssertionError("recurring_rule_timezone setting not found in settings.xml")


def test_extractors_find_a_plausible_number_of_zones():
    # Guards the extractors themselves: a regex that silently matches
    # nothing would make every comparison below vacuously pass.
    assert len(_cpp_zone_names()) >= 40
    assert len(_settings_options()) >= 40


def test_cpp_zone_names_are_unique():
    names = _cpp_zone_names()
    assert len(names) == len(set(names))


def test_settings_zone_options_are_unique():
    values = [v for _label, v in _settings_options()]
    assert len(values) == len(set(values))


def test_settings_offers_manual_as_its_default_sentinel():
    assert "manual" in [v for _label, v in _settings_options()]


def test_settings_and_cpp_list_exactly_the_same_zones():
    cpp = set(_cpp_zone_names())
    settings = {v for _label, v in _settings_options()} - {"manual"}
    only_in_settings = sorted(settings - cpp)
    only_in_cpp = sorted(cpp - settings)
    assert not only_in_settings, (
        f"offered in settings.xml but unknown to TimeZoneUtil.cpp's kKnownTimeZones "
        f"(would silently fall back to manual): {only_in_settings}"
    )
    assert (
        not only_in_cpp
    ), f"known to kKnownTimeZones but missing from settings.xml's dropdown (unreachable): {only_in_cpp}"


def test_every_settings_option_label_has_a_strings_po_entry():
    po = STRINGS_PO.read_text(encoding="utf-8")
    missing = [label for label, _v in _settings_options() if f'msgctxt "#{label}"' not in po]
    assert not missing, f"settings.xml options with no strings.po entry: {missing}"
