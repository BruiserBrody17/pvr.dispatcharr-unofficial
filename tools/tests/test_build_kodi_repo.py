"""tools/build_kodi_repo.py: the Kodi repository's indexes, the repository addon zip and its landing page.

What Kodi does with them is mirrored here from its own source (Kodi 21.3 and 22.0 RC1 read the same): an index entry
is dropped unless its <platform> names the running platform (CAddonInfoBuilder::PlatformSupportsAddon), a repository
<dir> is used only when Kodi's xbmc.addon version lies within its minversion..maxversion (CRepository::CRepository),
and an HTTP directory item is a link whose text equals its target (CHTTPDirectory::GetDirectory).
"""

import hashlib
import importlib.util
import re
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

import pytest

_TOOLS = Path(__file__).parent.parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _TOOLS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


repo = _load("build_kodi_repo")
gate = _load("check_release_zip")

TAG = "0.14.0-Piers"

# The platform names each Kodi build accepts, from PlatformSupportsAddon's #if blocks.
KODI_PLATFORMS = {
    "linux x86_64": {"all", "linux", "linux-x86_64"},
    "linux aarch64 (CoreELEC)": {"all", "linux", "linux-aarch64"},
    "linux armv7 (CoreELEC)": {"all", "linux", "linux-armv7"},
    "android aarch64": {"all", "android", "android-aarch64"},
    "android armv7": {"all", "android", "android-armv7"},
    "windows x86_64": {"all", "windx", "windows", "windows-x86_64"},
    "macOS arm64": {"all", "osx", "osxarm64", "osx-arm64"},
}

# The <platform> each release zip of 0.14.0 really carries (read from the gated zips), by asset suffix.
REAL_ZIP_PLATFORMS = {
    "windows-x86_64": "windows-x86_64",
    "linux": "linux",
    "osx-arm64": "osx-arm64",
    "android-aarch64": "android-aarch64",
    "android-armv7": "android-armv7",
    "coreelec-aarch64": "",
}


def _addon_xml(version="0.14.0", platform="linux", pvr_api="9.2.0", assets=()):
    asset_xml = "".join("<icon>%s</icon>" % a for a in assets)
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<addon id="pvr.dispatcharr-unofficial" version="%s" name="Dispatcharr PVR Client (Unofficial)"'
        ' provider-name="BruiserBrody17">\n'
        "  <requires>\n"
        '    <import addon="kodi.binary.global.main" minversion="2.0.3" version="2.0.3"/>\n'
        '    <import addon="kodi.binary.instance.pvr" minversion="%s" version="%s"/>\n'
        "    <!-- a comment the index does not need -->\n"
        '    <import addon="inputstream.ffmpegdirect" version="3.0.0" optional="true"/>\n'
        "  </requires>\n"
        '  <extension point="kodi.pvrclient" library_linux="pvr.dispatcharr-unofficial.so"/>\n'
        '  <extension point="xbmc.addon.metadata">\n'
        '    <summary lang="en_GB">Unofficial client</summary>\n'
        "    <platform>%s</platform>\n"
        "    <assets>%s</assets>\n"
        "  </extension>\n"
        "</addon>\n"
    ) % (version, pvr_api, pvr_api, platform, asset_xml)


def _zip(directory, suffix, files=None, version="0.14.0", name=None, own_version=None, **kwargs):
    path = Path(directory) / (name or "addon-pvr.dispatcharr-unofficial-%s-%s.zip" % (version, suffix))
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("pvr.dispatcharr-unofficial/addon.xml", _addon_xml(version=own_version or version, **kwargs))
        for member, data in (files or {}).items():
            z.writestr("pvr.dispatcharr-unofficial/" + member, data)
    return path


def _release(directory, version="0.14.0", pvr_api="9.2.0"):
    """The six addon zips of a release as the builds produce them, plus the two plugin zips."""
    zips = []
    for suffix, platform in REAL_ZIP_PLATFORMS.items():
        own_version = version + ".1" if suffix.startswith("coreelec") else version
        zips.append(
            _zip(
                directory,
                suffix,
                name="addon-pvr.dispatcharr-unofficial-%s-%s.zip" % (version, suffix),
                version=version,
                own_version=own_version,
                platform=platform,
                pvr_api=pvr_api,
            )
        )
    for plugin in ("timeshift_buffer.zip", "recording_edl.zip"):
        with zipfile.ZipFile(Path(directory) / plugin, "w") as z:
            z.writestr(plugin[:-4] + "/plugin.json", "{}")
        zips.append(Path(directory) / plugin)
    return zips


def _entries(index_dir):
    return ET.parse(index_dir / "addons.xml").getroot().findall("addon")


def _meta(entry):
    return next(e for e in entry.findall("extension") if e.get("point") == "xbmc.addon.metadata")


def _platforms_of(entry):
    text = _meta(entry).findtext("platform") or ""
    return {p for p in re.split(r"[ ,]+", text) if p}


def _offered(entries, accepted):
    """What Kodi keeps of an index on a platform accepting `accepted` (an empty <platform> means every platform)."""
    return [e for e in entries if not _platforms_of(e) or _platforms_of(e) & accepted]


def test_each_platform_is_offered_exactly_its_own_build(tmp_path):
    repo.build_index(tmp_path / "site", "22", TAG, _release(tmp_path / "zips"))
    entries = _entries(tmp_path / "site" / "kodi22")
    expected = {
        "linux x86_64": "linux",
        "linux aarch64 (CoreELEC)": "coreelec-aarch64",
        "android aarch64": "android-aarch64",
        "android armv7": "android-armv7",
        "windows x86_64": "windows-x86_64",
        "macOS arm64": "osx-arm64",
    }
    for kodi, accepted in KODI_PLATFORMS.items():
        offered = [_meta(e).findtext("path") for e in _offered(entries, accepted)]
        if kodi in expected:
            assert offered == ["%s/addon-pvr.dispatcharr-unofficial-0.14.0-%s.zip" % (TAG, expected[kodi])], kodi
        else:
            assert offered == [], kodi


def test_the_zips_own_platform_tags_would_not_do(tmp_path):
    # Why the index sets <platform> itself: with the zips' own tags, CoreELEC's empty one is offered to every platform
    # and the x86_64 Linux build to every ARM Linux.
    entries = []
    for z in _release(tmp_path):
        if z.name.startswith("addon-"):
            entries.append(repo.read_addon_xml(z, "pvr.dispatcharr-unofficial"))
    assert len(_offered(entries, KODI_PLATFORMS["windows x86_64"])) == 2
    assert len(_offered(entries, KODI_PLATFORMS["linux aarch64 (CoreELEC)"])) == 2


def test_index_keeps_the_zips_metadata_and_versions(tmp_path):
    repo.build_index(tmp_path / "site", "22", TAG, _release(tmp_path / "zips"))
    by_path = {_meta(e).findtext("path"): e for e in _entries(tmp_path / "site" / "kodi22")}
    coreelec = by_path[TAG + "/addon-pvr.dispatcharr-unofficial-0.14.0-coreelec-aarch64.zip"]
    assert coreelec.get("version") == "0.14.0.1"
    linux = by_path[TAG + "/addon-pvr.dispatcharr-unofficial-0.14.0-linux.zip"]
    assert linux.get("version") == "0.14.0"
    assert [i.get("addon") for i in linux.iter("import")] == [
        "kodi.binary.global.main",
        "kodi.binary.instance.pvr",
        "inputstream.ffmpegdirect",
    ]
    assert _meta(linux).findtext("summary") == "Unofficial client"
    assert len(_meta(linux).findall("path")) == 1
    assert len(_meta(linux).findall("platform")) == 1


def test_checksum_file_is_the_sha256_of_the_index(tmp_path):
    repo.build_index(tmp_path / "site", "22", TAG, _release(tmp_path / "zips"))
    d = tmp_path / "site" / "kodi22"
    digest = (d / "addons.xml.sha256").read_text()
    # Kodi keeps what precedes the first space or newline (CRepository::FetchChecksum).
    assert re.split(r"[ \n]", digest)[0] == hashlib.sha256((d / "addons.xml").read_bytes()).hexdigest()


def test_plugin_zips_are_skipped_and_reported(tmp_path):
    platforms, skipped = repo.build_index(tmp_path / "site", "22", TAG, _release(tmp_path / "zips"))
    assert skipped == ["timeshift_buffer.zip", "recording_edl.zip"]
    assert len(platforms) == 6


def test_kodi_21_line_takes_a_plain_tag_and_api_8(tmp_path):
    zips = _release(tmp_path / "zips", version="0.13.0", pvr_api="8.3.0")
    repo.build_index(tmp_path / "site", "21", "0.13.0", zips)
    paths = {_meta(e).findtext("path") for e in _entries(tmp_path / "site" / "kodi21")}
    assert "0.13.0/addon-pvr.dispatcharr-unofficial-0.13.0-linux.zip" in paths


def test_a_build_for_the_other_kodi_is_refused(tmp_path):
    zips = _release(tmp_path / "zips", pvr_api="9.2.0")
    with pytest.raises(repo.RepoError, match="PVR API 9, not Kodi 21"):
        repo.build_index(tmp_path / "site", "21", TAG, zips)


def test_a_zip_whose_own_platform_contradicts_its_name_is_refused(tmp_path):
    z = _zip(tmp_path, "android-aarch64", platform="android-armv7")
    with pytest.raises(repo.RepoError, match="android-armv7"):
        repo.build_index(tmp_path / "site", "22", TAG, [z])


def test_a_zip_of_another_version_is_refused(tmp_path):
    z = _zip(tmp_path, "linux", name="addon-pvr.dispatcharr-unofficial-0.14.0-linux.zip", own_version="0.13.9")
    with pytest.raises(repo.RepoError, match="0.13.9"):
        repo.build_index(tmp_path / "site", "22", TAG, [z])


def test_a_revision_suffix_is_not_mistaken_for_another_version(tmp_path):
    z = _zip(tmp_path, "linux", name="addon-pvr.dispatcharr-unofficial-0.14.0-linux.zip", own_version="0.14.01")
    with pytest.raises(repo.RepoError):
        repo.build_index(tmp_path / "site", "22", TAG, [z])


def test_unknown_names_and_platforms_are_refused(tmp_path):
    for name in ("something.zip", "addon-pvr.dispatcharr-unofficial-0.14.0-ios-aarch64.zip"):
        z = _zip(tmp_path, "x", name=name)
        with pytest.raises(repo.RepoError):
            repo.build_index(tmp_path / "site", "22", TAG, [z])


def test_two_builds_of_one_platform_are_refused(tmp_path):
    a = _zip(tmp_path / "a", "linux")
    b = _zip(tmp_path / "b", "linux")
    with pytest.raises(repo.RepoError, match="both linux-x86_64"):
        repo.build_index(tmp_path / "site", "22", TAG, [a, b])


def test_a_bad_tag_is_refused(tmp_path):
    with pytest.raises(repo.RepoError, match="not a release tag"):
        repo.build_index(tmp_path / "site", "22", "v0.14.0", [_zip(tmp_path, "linux")])


def test_a_failed_run_leaves_the_published_index_alone(tmp_path):
    site = tmp_path / "site"
    repo.build_index(site, "22", TAG, _release(tmp_path / "zips"))
    before = (site / "kodi22" / "addons.xml").read_bytes()
    # Fails while writing (an asset the zip lacks), after the new index directory was started.
    bad = _zip(tmp_path / "bad", "osx-x86_64", platform="osx-x86_64", assets=["resources/icon.png"])
    with pytest.raises(repo.RepoError, match="does not contain"):
        repo.build_index(site, "22", TAG, [*_release(tmp_path / "zips2"), bad])
    assert (site / "kodi22" / "addons.xml").read_bytes() == before
    assert sorted(p.name for p in site.iterdir()) == ["kodi22"]


def test_rewriting_one_line_leaves_the_other_alone(tmp_path):
    site = tmp_path / "site"
    repo.build_index(site, "22", TAG, _release(tmp_path / "z22"))
    kodi22 = (site / "kodi22" / "addons.xml").read_bytes()
    repo.build_index(site, "21", "0.13.0", _release(tmp_path / "z21", version="0.13.0", pvr_api="8.3.0"))
    assert (site / "kodi22" / "addons.xml").read_bytes() == kodi22
    assert (site / "kodi21" / "addons.xml").is_file()


def test_declared_assets_are_copied_where_kodi_looks_for_them(tmp_path):
    # With a <path>, Kodi reads art from <artdir>/<the path's directory>/<asset> (CAddonInfoBuilder::ParseXML).
    z = _zip(tmp_path, "linux", files={"resources/icon.png": b"PNG"}, assets=["resources/icon.png"])
    repo.build_index(tmp_path / "site", "22", TAG, [z])
    assert (tmp_path / "site" / "kodi22" / "art" / TAG / "resources" / "icon.png").read_bytes() == b"PNG"


def test_a_declared_asset_missing_from_the_zip_is_refused(tmp_path):
    z = _zip(tmp_path, "linux", assets=["resources/icon.png"])
    with pytest.raises(repo.RepoError, match="does not contain"):
        repo.build_index(tmp_path / "site", "22", TAG, [z])


def test_an_asset_path_leaving_the_art_directory_is_refused(tmp_path):
    for asset in ("../../index.html", "/etc/x.png", "resources\\..\\x.png"):
        z = _zip(tmp_path / str(len(asset)), "linux", files={"x.png": b"PNG"}, assets=[asset])
        with pytest.raises(repo.RepoError, match="outside the art directory"):
            repo.build_index(tmp_path / "site", "22", TAG, [z])


def test_rebuilding_drops_art_of_the_previous_release(tmp_path):
    site = tmp_path / "site"
    z = _zip(tmp_path / "a", "linux", files={"resources/icon.png": b"PNG"}, assets=["resources/icon.png"])
    repo.build_index(site, "22", TAG, [z])
    z2 = _zip(
        tmp_path / "b", "linux", version="0.14.1", files={"resources/icon.png": b"PNG"}, assets=["resources/icon.png"]
    )
    repo.build_index(site, "22", "0.14.1-Piers", [z2])
    assert sorted(p.name for p in (site / "kodi22" / "art").iterdir()) == ["0.14.1-Piers"]


# The repository addon.


def _build_repository(site, **kwargs):
    kwargs.setdefault("site_url", repo.SITE_URL)
    kwargs.setdefault("download_url", repo.DOWNLOAD_URL)
    return repo.build_repository(site, stamp=1791348050, **kwargs)


def test_repository_zip_is_reproducible_and_passes_the_release_gate(tmp_path):
    name = _build_repository(tmp_path / "a")
    _build_repository(tmp_path / "b")
    a, b = tmp_path / "a" / name, tmp_path / "b" / name
    assert a.read_bytes() == b.read_bytes()
    assert gate.check_zip(str(a), ["example-private-term"]) == []
    with zipfile.ZipFile(a) as z:
        assert z.namelist() == ["repository.dispatcharr-unofficial/", "repository.dispatcharr-unofficial/addon.xml"]
        xml = z.read("repository.dispatcharr-unofficial/addon.xml").decode()
    assert "@" not in re.sub(r"<!--.*?-->", "", xml, flags=re.S)
    assert repo.SITE_URL + "kodi22/addons.xml" in xml
    assert '<datadir zip="true">%s</datadir>' % repo.DOWNLOAD_URL in xml


def test_repository_indexes_itself_at_the_zip_it_wrote(tmp_path):
    site = tmp_path / "site"
    name = _build_repository(site)
    (entry,) = _entries(site / "repo")
    assert entry.get("id") == "repository.dispatcharr-unofficial"
    assert name == "repository.dispatcharr-unofficial-%s.zip" % entry.get("version")
    assert _meta(entry).findtext("path") == name
    assert (site / name).is_file()
    assert (site / ".nojekyll").is_file()


def test_an_older_repository_zip_is_removed(tmp_path):
    site = tmp_path / "site"
    site.mkdir()
    (site / "repository.dispatcharr-unofficial-0.0.1.zip").write_bytes(b"old")
    name = _build_repository(site)
    assert sorted(p.name for p in site.glob("*.zip")) == [name]


def _kodi_directory_items(page):
    """The items Kodi's HTTP directory parser takes from a page: links whose text equals their target."""
    item = re.compile(r'<a href="([^"]*)"[^>]*>\s*(.*?)\s*</a>(.+?)(?=<a|</tr|$)', re.I | re.S)
    return [link for link, name, _rest in item.findall(page) if link.lstrip("/") == name.strip()]


def test_landing_page_lists_only_the_repository_zip_for_kodi(tmp_path):
    site = tmp_path / "site"
    name = _build_repository(site)
    assert _kodi_directory_items((site / "index.html").read_text()) == [name]


def test_no_index_kodi_reads_sits_in_the_directory_users_browse(tmp_path):
    # Kodi caches the landing page's listing (just the zip) and then refuses every other file in that directory
    # without asking the server (CFile::Open's g_directoryCache check), so an index there never refreshes.
    for d in _dirs():
        for tag in ("info", "checksum"):
            url = d.findtext(tag)
            assert url.startswith(repo.SITE_URL)
            assert "/" in url[len(repo.SITE_URL) :], url


def test_template_urls_are_checked(tmp_path):
    for bad in ("https://example.invalid/no-slash", "ftp://example.invalid/", "https://exa mple.invalid/"):
        with pytest.raises(repo.RepoError):
            _build_repository(tmp_path, site_url=bad)


# The real repository addon.xml.in against the Kodi versions it has to tell apart.


def _version(text):
    return tuple(int(p) for p in text.split("."))


def _dirs():
    text = (repo.REPOSITORY_SOURCE / "addon.xml.in").read_text(encoding="utf-8")
    root = ET.fromstring(repo.fill_template(text, repo.SITE_URL, repo.DOWNLOAD_URL).encode())
    return root.find("extension[@point='xbmc.addon.repository']").findall("dir")


def _dirs_for(addon_api):
    v = _version(addon_api)
    return [
        d.findtext("info")
        for d in _dirs()
        if (not d.get("minversion") or v >= _version(d.get("minversion")))
        and (not d.get("maxversion") or v <= _version(d.get("maxversion")))
    ]


@pytest.mark.parametrize(
    "addon_api, line",
    [
        ("21.0.0", "kodi21"),
        ("21.3.0", "kodi21"),  # Kodi 21.3's ADDON_API (version.txt)
        ("21.90.901", "kodi22"),  # Kodi 22.0 RC1's
        ("22.0.0", "kodi22"),
        ("22.3.0", "kodi22"),
        ("20.2.0", None),
        ("22.90.0", None),  # a Kodi 23 pre-release, by the same numbering
    ],
)
def test_each_kodi_reads_its_own_line_and_the_repository_itself(addon_api, line):
    expected = [repo.SITE_URL + "repo/addons.xml"]
    if line:
        expected.insert(0, repo.SITE_URL + line + "/addons.xml")
    assert _dirs_for(addon_api) == expected


def test_every_kodi_line_has_a_dir_and_the_tool_writes_where_it_points(tmp_path):
    infos = {d.findtext("info") for d in _dirs()}
    for directory, _api in repo.KODI_LINES.values():
        assert repo.SITE_URL + directory + "/addons.xml" in infos
    for d in _dirs():
        info = d.findtext("info")
        assert d.findtext("checksum") == info + ".sha256"
        assert d.find("checksum").get("verify") == "sha256"


def test_main_reports_errors_with_exit_status_1(tmp_path, capsys):
    z = _zip(tmp_path, "linux", platform="android-armv7")
    assert repo.main(["x", "index", "--kodi", "22", "--tag", TAG, "--site", str(tmp_path / "s"), str(z)]) == 1
    assert "error:" in capsys.readouterr().err
    ok = _zip(tmp_path / "ok", "linux")
    assert repo.main(["x", "index", "--kodi", "22", "--tag", TAG, "--site", str(tmp_path / "s"), str(ok)]) == 0
