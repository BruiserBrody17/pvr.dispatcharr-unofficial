"""Cross-file checks of the addon's metadata against the files it points at.

Both found by the 2026-10-05 twelfth hardening sweep: addon.xml.in declared `resources/icon.png`, which
was never committed (so every release zip shipped an add-on whose declared icon is missing), and the root
CMakeLists.txt's DISPATCHARR_HEADERS list had drifted five headers behind src/.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ADDON_DIR = REPO_ROOT / "pvr.dispatcharr-unofficial"


def test_every_asset_path_addon_xml_declares_exists():
    # The CMake placeholders (@ADDON_DEPENDS@, @PLATFORM@) are not XML; they are filled in at build time.
    text = re.sub(r"@[A-Z_]+@", "", (ADDON_DIR / "addon.xml.in").read_text(encoding="utf-8"))
    root = ET.fromstring(text)
    for assets in root.iter("assets"):
        for child in assets:
            path = (child.text or "").strip()
            assert path, f"<{child.tag}> in <assets> has no path"
            assert (ADDON_DIR / path).is_file(), f"addon.xml.in declares {child.tag} {path}, which is not in the repo"


def test_every_source_header_is_listed_in_the_root_cmakelists():
    cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    listed = set(re.findall(r"src/([A-Za-z0-9_]+\.h)\b", cmake))
    on_disk = {p.name for p in (REPO_ROOT / "src").glob("*.h")}
    assert on_disk - listed == set(), f"headers in src/ missing from DISPATCHARR_HEADERS: {sorted(on_disk - listed)}"
    assert listed - on_disk == set(), f"DISPATCHARR_HEADERS lists headers that do not exist: {sorted(listed - on_disk)}"


def test_the_asset_check_would_catch_a_missing_icon(tmp_path, monkeypatch):
    # Guards the check itself: an <assets> entry pointing at nothing must fail it.
    (tmp_path / "addon.xml.in").write_text(
        "<addon><extension><assets><icon>resources/icon.png</icon></assets></extension></addon>"
    )
    monkeypatch.setattr(sys.modules[__name__], "ADDON_DIR", tmp_path)
    try:
        test_every_asset_path_addon_xml_declares_exists()
    except AssertionError:
        return
    raise AssertionError("a declared icon that does not exist was not reported")
