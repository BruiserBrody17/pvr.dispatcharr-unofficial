"""Cross-file consistency check for the independent version numbers that live in more than one file
(CLAUDE.md, "Independent version numbers, decoupled since 1.0.1"; the Kodi repository addon's lives in one file only).

Each piece has two places its version must move together, and missing one is
documented there as a real, easy mistake: the addon's addon.xml.in and the
CoreELEC package.mk (CoreELEC packages the addon binary from the tag that
version names), and each plugin's plugin.json (only read for Dispatcharr's
not-yet-trusted import preview) and its plugin.py Plugin.version (what
Dispatcharr actually runs once trusted). Reads this repo's real files, like
test_timezone_settings_sync.py.
"""

import json
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
ADDON_XML_IN = REPO_ROOT / "pvr.dispatcharr-unofficial" / "addon.xml.in"
PACKAGE_MK = REPO_ROOT / "packaging" / "coreelec" / "pvr.dispatcharr-unofficial" / "package.mk"
CHANGELOG = REPO_ROOT / "CHANGELOG.md"
PLUGINS = ("timeshift_buffer", "recording_edl")
# The branch this tree is the release line of. A release tag carries it as a suffix (0.14.0-Piers) so Omega's and
# Piers' tags never collide; the addon's own version stays plain numeric, since Kodi reads "-suffix" in an addon
# version as a revision. CoreELEC's package.mk names the tag, so it carries the suffix too. The Omega branch has the
# empty string here until its next release is tagged (its 0.13.0 tag predates the suffix).
RELEASE_TAG_SUFFIX = "-Piers"

_SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def _addon_xml_version() -> str:
    text = ADDON_XML_IN.read_text(encoding="utf-8")
    # The <addon ...> element's own version attribute, not an <import>'s: the
    # first version="..." in the file belongs to the <?xml ...?> declaration
    # and is skipped by anchoring on the addon element.
    match = re.search(r"<addon\b[^>]*?\sversion=\"([^\"]+)\"", text, re.DOTALL)
    assert match, "no <addon version=...> in addon.xml.in"
    return match.group(1)


def _package_mk_version() -> str:
    match = re.search(r'^PKG_VERSION="([^"]+)"', PACKAGE_MK.read_text(encoding="utf-8"), re.MULTILINE)
    assert match, "no PKG_VERSION in package.mk"
    return match.group(1)


def _plugin_json_version(plugin: str) -> str:
    return json.loads((REPO_ROOT / "dispatcharr-plugin" / plugin / "plugin.json").read_text(encoding="utf-8"))[
        "version"
    ]


def _plugin_py_version(plugin: str) -> str:
    text = (REPO_ROOT / "dispatcharr-plugin" / plugin / "plugin.py").read_text(encoding="utf-8")
    # The Plugin class attribute, not the HTTP handler's server_version string.
    match = re.search(r"^class Plugin:\n(?:.*\n)*?\s+version = \"([^\"]+)\"", text, re.MULTILINE)
    assert match, f"no Plugin.version in {plugin}/plugin.py"
    return match.group(1)


def test_addon_version_is_semver_and_matches_the_coreelec_package():
    version = _addon_xml_version()
    assert _SEMVER.match(version), version
    assert _package_mk_version() == version + RELEASE_TAG_SUFFIX, (
        "addon.xml.in and packaging/coreelec/.../package.mk disagree (package.mk's PKG_VERSION is the release tag: "
        "the addon version plus the branch suffix)"
    )


def test_coreelec_package_downloads_the_tag_it_names():
    # CoreELEC unpacks the tarball into build/<PKG_NAME>-<PKG_VERSION> and GitHub names a tag archive's top directory
    # after the tag, so the URL must be built from PKG_VERSION as is: a URL with its own suffix would unpack into a
    # directory CoreELEC does not look in.
    text = PACKAGE_MK.read_text(encoding="utf-8")
    match = re.search(r'^PKG_URL="([^"]+)"', text, re.MULTILINE)
    assert match, "no PKG_URL in package.mk"
    assert match.group(1).endswith("/archive/${PKG_VERSION}.tar.gz"), match.group(1)


def test_addon_version_has_a_changelog_entry():
    # CHANGELOG.md's addon entries are "## [X.Y.Z] - date"; a plugin's are
    # "## `name` [X.Y.Z] - date". The current addon version must have one.
    version = _addon_xml_version()
    assert f"## [{version}]" in CHANGELOG.read_text(encoding="utf-8"), f"no CHANGELOG.md entry for {version}"


@pytest.mark.parametrize("plugin", PLUGINS)
def test_plugin_json_and_plugin_py_versions_match(plugin):
    json_version = _plugin_json_version(plugin)
    assert _SEMVER.match(json_version), json_version
    assert _plugin_py_version(plugin) == json_version, f"{plugin}: plugin.json and plugin.py's Plugin.version disagree"


@pytest.mark.parametrize("plugin", PLUGINS)
def test_plugin_version_has_a_changelog_entry(plugin):
    version = _plugin_json_version(plugin)
    assert f"## `{plugin}` [{version}]" in CHANGELOG.read_text(
        encoding="utf-8"
    ), f"no CHANGELOG.md entry for {plugin} {version}"
