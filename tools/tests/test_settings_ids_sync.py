"""Cross-file consistency checks between the addon's C++ source, its
settings.xml and its strings.po (same family as test_timezone_settings_sync.py,
which covers the one dropdown; this covers the rest).

- Every setting id the C++ reads, writes or reacts to exists in settings.xml:
  Kodi answers a GetSetting*() for an undeclared id with the caller's default
  and drops a SetSetting*() for one, both silently.
- Every setting settings.xml declares is referenced by the C++ (the read-only
  reference settings included: the addon writes them), so a renamed or removed
  setting can't linger in the dialog doing nothing.
- Every label/help id settings.xml uses, on a category, a setting or an option,
  has a strings.po entry, or Kodi shows the bare number.
Reads this repo's real files on purpose: the real triple is what is checked.
"""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = REPO_ROOT / "src"
SETTINGS_XML = REPO_ROOT / "pvr.dispatcharr-unofficial" / "resources" / "settings.xml"
STRINGS_PO = (
    REPO_ROOT / "pvr.dispatcharr-unofficial" / "resources" / "language" / "resource.language.en_gb" / "strings.po"
)

_CPP_SETTING_REF = re.compile(
    r'(?:GetSetting(?:String|Int|Boolean)|SetSetting(?:String|Int|Boolean))\(\s*"([a-z_]+)"'
    r'|settingName\s*==\s*"([a-z_]+)"'
)


def _cpp_setting_ids() -> set[str]:
    ids = set()
    for path in sorted(SRC_DIR.glob("*.cpp")) + sorted(SRC_DIR.glob("*.h")):
        for match in _CPP_SETTING_REF.finditer(path.read_text(encoding="utf-8")):
            ids.add(match.group(1) or match.group(2))
    return ids


def _settings_root():
    return ET.parse(SETTINGS_XML).getroot()


def _xml_setting_ids() -> set[str]:
    return {s.get("id") for s in _settings_root().iter("setting") if s.get("id")}


def _xml_label_ids() -> set[str]:
    ids = set()
    for element in _settings_root().iter():
        for attr in ("label", "help"):
            value = element.get(attr)
            if value and value.isdigit():
                ids.add(value)
    return ids


def _po_ids() -> set[str]:
    return set(re.findall(r'^msgctxt "#(\d+)"', STRINGS_PO.read_text(encoding="utf-8"), re.MULTILINE))


def test_extractors_find_a_plausible_number_of_ids():
    # A regex that silently matched nothing would make the comparisons below
    # vacuously pass.
    assert len(_cpp_setting_ids()) >= 15
    assert len(_xml_setting_ids()) >= 15
    assert len(_xml_label_ids()) >= 40
    assert len(_po_ids()) >= 40


def test_every_setting_the_cpp_references_is_declared_in_settings_xml():
    missing = _cpp_setting_ids() - _xml_setting_ids()
    assert not missing, f"referenced by src/ but not declared in settings.xml: {sorted(missing)}"


def test_every_declared_setting_is_referenced_by_the_cpp():
    unused = _xml_setting_ids() - _cpp_setting_ids()
    assert not unused, f"declared in settings.xml but never read, written or handled by src/: {sorted(unused)}"


def test_every_label_and_help_id_has_a_strings_po_entry():
    missing = _xml_label_ids() - _po_ids()
    assert not missing, f"settings.xml label/help ids with no strings.po entry: {sorted(missing)}"


def test_strings_po_ids_are_unique():
    ids = re.findall(r'^msgctxt "#(\d+)"', STRINGS_PO.read_text(encoding="utf-8"), re.MULTILINE)
    assert len(ids) == len(set(ids)), "duplicate msgctxt ids in strings.po"
