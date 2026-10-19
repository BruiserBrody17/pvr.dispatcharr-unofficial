#!/usr/bin/env python3
"""Builds the static site Kodi reads the addon's repository from (served as-is, e.g. by GitHub Pages).

    <site>/index.html, .nojekyll                     a page linking the repository zip (Kodi's file manager reads it)
    <site>/repository.dispatcharr-unofficial-<v>.zip the repository addon, installed once by hand
    <site>/repo/addons.xml, addons.xml.sha256        the index of the repository addon itself (it updates itself)
    <site>/kodi21/addons.xml, .sha256, art/          the index of the Kodi 21 (Omega) build of each platform
    <site>/kodi22/addons.xml, .sha256, art/          the same for Kodi 22 (Piers)

The addon zips are not copied: each index entry names its zip under the GitHub release download URL with a <path>, so
Kodi downloads exactly the assets that passed the release gate. Each Kodi line is rewritten on its own (`index`), from
the zips of that line's latest release; the other line's directory is left alone.

The index entry is the zip's own addon.xml with its <platform> set from the asset's name. Kodi drops every entry whose
<platform> does not name the running platform (CAddonInfoBuilder::PlatformSupportsAddon), and the zips' own tags are
not precise enough for that: the Linux build says `linux` (which an ARM Linux also matches) and CoreELEC's harness
leaves the tag empty (docs/BUILDING.md's CoreELEC section), which Kodi reads as "every platform".

usage: build_kodi_repo.py index --kodi {21,22} --tag TAG --site DIR <addon zip> [...]
       build_kodi_repo.py repository --site DIR [--site-url URL] [--download-url URL] [--time UNIX]
"""

import argparse
import hashlib
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_SOURCE = REPO_ROOT / "kodi-repository" / "repository.dispatcharr-unofficial"

ADDON_ID = "pvr.dispatcharr-unofficial"
REPOSITORY_ID = "repository.dispatcharr-unofficial"
ASSET_PREFIX = "addon-" + ADDON_ID + "-"
PLUGIN_ZIPS = ("timeshift_buffer.zip", "recording_edl.zip")

SITE_URL = "https://bruiserbrody17.github.io/pvr.dispatcharr-unofficial/"
DOWNLOAD_URL = "https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/releases/download/"

# Kodi line -> (site directory, the kodi.binary.instance.pvr major its builds import). The PVR API major is how a zip
# says which Kodi it was built for: 8.x for Kodi 21, 9.x for Kodi 22 (docs/BRANCHES.md).
KODI_LINES = {
    "21": ("kodi21", 8),
    "22": ("kodi22", 9),
}

# Asset-name suffix -> the <platform> the index gives it. The CI Linux build runs on x86_64; the CoreELEC package is
# the only ARM Linux build there is, so an ARM Linux that is not CoreELEC is offered it too.
PLATFORMS = {
    "windows-x86_64": "windows-x86_64",
    "linux": "linux-x86_64",
    "osx-arm64": "osx-arm64",
    "osx-x86_64": "osx-x86_64",
    "android-aarch64": "android-aarch64",
    "android-armv7": "android-armv7",
    "coreelec-aarch64": "linux-aarch64",
    "coreelec-armv7": "linux-armv7",
}

METADATA_POINTS = ("xbmc.addon.metadata", "kodi.addon.metadata")


class RepoError(Exception):
    pass


def tag_version(tag):
    """`0.14.0-Piers` -> `0.14.0` (the tag carries the branch, the addon version does not)."""
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[A-Za-z]+)?", tag):
        raise RepoError("%r is not a release tag (<version> or <version>-<Branch>)" % tag)
    return tag.split("-", 1)[0]


def asset_platform(name, version):
    """The index <platform> for a release asset name, or None for a plugin zip."""
    if name in PLUGIN_ZIPS:
        return None
    stem = ASSET_PREFIX + version + "-"
    if not (name.startswith(stem) and name.endswith(".zip")):
        raise RepoError("%s is not an addon zip of release %s (expected %s<platform>.zip)" % (name, version, stem))
    suffix = name[len(stem) : -len(".zip")]
    if suffix not in PLATFORMS:
        raise RepoError("%s: unknown platform %r (known: %s)" % (name, suffix, ", ".join(PLATFORMS)))
    return PLATFORMS[suffix]


def read_addon_xml(zip_path, addon_id):
    with zipfile.ZipFile(zip_path) as z:
        try:
            data = z.read(addon_id + "/addon.xml")
        except KeyError:
            raise RepoError("%s has no %s/addon.xml" % (os.path.basename(zip_path), addon_id)) from None
    root = ET.fromstring(data)
    if root.tag != "addon" or root.get("id") != addon_id:
        raise RepoError("%s: addon.xml is not %s's" % (os.path.basename(zip_path), addon_id))
    return root


def metadata_element(root):
    for ext in root.findall("extension"):
        if ext.get("point") in METADATA_POINTS:
            return ext
    raise RepoError("addon.xml of %s %s has no metadata extension" % (root.get("id"), root.get("version")))


def pvr_api_major(root):
    for imp in root.iter("import"):
        if imp.get("addon") == "kodi.binary.instance.pvr":
            return int(imp.get("version", "0").split(".")[0])
    raise RepoError("addon.xml of %s %s imports no kodi.binary.instance.pvr" % (root.get("id"), root.get("version")))


def check_platform(name, own_text, index_platform):
    """The zip's own tag may be empty or vaguer than the index's (`linux` for `linux-x86_64`), never a different one."""
    own = [p for p in re.split(r"[ ,]+", own_text or "") if p]
    family = index_platform.split("-", 1)[0]
    allowed = {index_platform, family, "windx"} if family == "windows" else {index_platform, family}
    wrong = [p for p in own if p not in allowed]
    if wrong:
        raise RepoError("%s says <platform>%s</platform>, but its name says %s" % (name, own_text, index_platform))


def index_entry(zip_path, tag, kodi):
    """The addons.xml entry for one addon zip, and the asset paths (in the zip) its metadata declares."""
    name = os.path.basename(zip_path)
    version = tag_version(tag)
    platform = asset_platform(name, version)
    root = read_addon_xml(zip_path, ADDON_ID)
    # CoreELEC appends its package revision (0.14.0.1); every other build carries the plain version.
    if not re.fullmatch(re.escape(version) + r"(\.\d+)?", root.get("version", "")):
        raise RepoError("%s holds version %s, not release %s" % (name, root.get("version"), version))
    major = pvr_api_major(root)
    if major != KODI_LINES[kodi][1]:
        raise RepoError("%s is built against PVR API %d, not Kodi %s's %d" % (name, major, kodi, KODI_LINES[kodi][1]))
    meta = metadata_element(root)
    element = meta.find("platform")
    check_platform(name, element.text if element is not None else "", platform)
    if element is None:
        element = ET.SubElement(meta, "platform")
    element.text = platform
    for old in meta.findall("path"):
        meta.remove(old)
    path = ET.Element("path")
    path.text = "%s/%s" % (tag, name)
    path.tail = meta.text
    meta.insert(0, path)
    assets = [a for a in ((child.text or "").strip() for e in meta.findall("assets") for child in e) if a]
    for asset in assets:
        if asset.startswith("/") or "\\" in asset or ".." in asset.split("/"):
            raise RepoError("%s declares asset %r, which would land outside the art directory" % (name, asset))
    return platform, root, assets


def addons_xml(entries):
    body = "".join(ET.tostring(e, encoding="unicode").rstrip() + "\n" for e in entries)
    return '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<addons>\n' + body + "</addons>\n"


def write_index(directory, entries):
    text = addons_xml(entries).encode("utf-8")
    (directory / "addons.xml").write_bytes(text)
    (directory / "addons.xml.sha256").write_text(hashlib.sha256(text).hexdigest() + "  addons.xml\n")


def replace_dir(final, build):
    """Swaps a freshly built directory in for `final`, so a failed run never leaves half an index behind."""
    old = final.with_name(final.name + ".old")
    if old.exists():
        shutil.rmtree(old)
    if final.exists():
        final.rename(old)
    build.rename(final)
    if old.exists():
        shutil.rmtree(old)


def build_index(site, kodi, tag, zips):
    site = Path(site)
    entries, seen, skipped = {}, {}, []
    for zip_path in zips:
        name = os.path.basename(zip_path)
        if asset_platform(name, tag_version(tag)) is None:
            skipped.append(name)
            continue
        platform, root, assets = index_entry(zip_path, tag, kodi)
        if platform in seen:
            raise RepoError("%s and %s are both %s builds" % (seen[platform], name, platform))
        seen[platform] = name
        entries[platform] = (zip_path, root, assets)
    if not entries:
        raise RepoError("no addon zip given")
    site.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix=".build-", dir=site))
    build.chmod(0o755)  # mkdtemp's 0700 would be what a local web server serves from
    try:
        for zip_path, _root, assets in entries.values():
            with zipfile.ZipFile(zip_path) as z:
                for asset in assets:
                    target = build / "art" / tag / asset
                    target.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        target.write_bytes(z.read(ADDON_ID + "/" + asset))
                    except KeyError:
                        raise RepoError("%s declares %s but does not contain it" % (zip_path, asset)) from None
        write_index(build, [entries[p][1] for p in sorted(entries)])
        replace_dir(site / KODI_LINES[kodi][0], build)
    except BaseException:
        shutil.rmtree(build, ignore_errors=True)
        raise
    return sorted(entries), skipped


def fill_template(text, site_url, download_url):
    for url in (site_url, download_url):
        if not re.fullmatch(r"https?://[^\s\"'<>&]+/", url):
            raise RepoError("%r is not an http(s) URL ending in /" % url)
    text = text.replace("@SITE_URL@", site_url).replace("@DOWNLOAD_URL@", download_url)
    left = re.findall(r"@[A-Z_]+@", text)
    if left:
        raise RepoError("unfilled placeholders in the repository addon.xml: %s" % ", ".join(sorted(set(left))))
    return text


def deterministic_zip(path, members, stamp):
    """Members (name -> bytes) in name order, one UTC time, no extra fields: what the release gate requires."""
    date_time = time.gmtime(stamp)[:6]
    with zipfile.ZipFile(path, "w") as z:
        for name in sorted(members):
            info = zipfile.ZipInfo(name, date_time=date_time)
            info.create_system = 3
            if name.endswith("/"):
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                z.writestr(info, members[name])


def source_time():
    """The last commit that touched the repository addon's source, so a rebuild gives the same zip."""
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "log", "-1", "--format=%ct", "--", str(REPOSITORY_SOURCE)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if not out:
        raise RepoError("the repository addon's source is not committed; pass --time")
    return int(out)


INDEX_HTML = """<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8"><title>Dispatcharr PVR Client (Unofficial) repository</title></head>
<body>
<h1>Dispatcharr PVR Client (Unofficial) repository</h1>
<p>An unofficial, community-maintained Kodi repository; not affiliated with the Dispatcharr project or with Kodi/Team
Kodi. Install the zip below in Kodi (Add-ons, Install from zip file), then install the addon from the repository.
Instructions: <a href="https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial#installing">the project's
README</a>.</p>
<pre>
<a href="{zip}">{zip}</a>
</pre>
</body>
</html>
"""


def build_repository(site, site_url, download_url, stamp):
    site = Path(site)
    template = (REPOSITORY_SOURCE / "addon.xml.in").read_text(encoding="utf-8")
    addon_xml = fill_template(template, site_url, download_url)
    root = ET.fromstring(addon_xml.encode("utf-8"))
    version = root.get("version")
    members = {REPOSITORY_ID + "/": b"", REPOSITORY_ID + "/addon.xml": addon_xml.encode("utf-8")}
    for f in sorted(REPOSITORY_SOURCE.rglob("*")):
        rel = f.relative_to(REPOSITORY_SOURCE).as_posix()
        if f.is_file() and rel != "addon.xml.in":
            members[REPOSITORY_ID + "/" + rel] = f.read_bytes()
    zip_name = "%s-%s.zip" % (REPOSITORY_ID, version)
    site.mkdir(parents=True, exist_ok=True)
    for old in site.glob(REPOSITORY_ID + "-*.zip"):
        if old.name != zip_name:
            old.unlink()
    deterministic_zip(site / zip_name, members, stamp)
    meta = metadata_element(root)
    path = ET.Element("path")
    path.text = zip_name
    meta.insert(0, path)
    (site / "repo").mkdir(exist_ok=True)
    write_index(site / "repo", [root])
    (site / "index.html").write_text(INDEX_HTML.format(zip=html.escape(zip_name)), encoding="utf-8")
    (site / ".nojekyll").write_bytes(b"")
    return zip_name


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    idx = sub.add_parser("index", help="rewrite one Kodi line's index from its latest release's zips")
    idx.add_argument("--kodi", required=True, choices=sorted(KODI_LINES))
    idx.add_argument("--tag", required=True, help="the release tag the zips are attached to, e.g. 0.14.0-Piers")
    idx.add_argument("--site", required=True)
    idx.add_argument("zips", nargs="+")
    rep = sub.add_parser("repository", help="build the repository addon zip, its own index and the landing page")
    rep.add_argument("--site", required=True)
    rep.add_argument("--site-url", default=SITE_URL)
    rep.add_argument("--download-url", default=DOWNLOAD_URL)
    rep.add_argument("--time", type=int, help="zip entry time (default: the source's last commit)")
    args = parser.parse_args(argv[1:])
    try:
        if args.command == "index":
            platforms, skipped = build_index(args.site, args.kodi, args.tag, args.zips)
            for name in skipped:
                print("skipped %s (a Dispatcharr plugin, not a Kodi addon)" % name)
            print("kodi%s: %s %s for %s" % (args.kodi, ADDON_ID, args.tag, ", ".join(platforms)))
        else:
            stamp = args.time if args.time is not None else source_time()
            print("wrote %s" % build_repository(args.site, args.site_url, args.download_url, stamp))
    except (RepoError, ET.ParseError, zipfile.BadZipFile, OSError, subprocess.CalledProcessError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
