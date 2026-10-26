"""Cross-file check that third-party code this project pulls in is pinned to an
immutable reference, not a tag its publisher can repoint (docs/CLOSED_ITEMS.md,
"CI release-upload actions pinned to mutable tags" / "FetchContent GIT_TAG pins
are mutable tags" / "Prebuilt Windows dependency archives have no integrity
check", all closed 2026-10-02):

- every `uses:` in .github/workflows is `@<40-hex commit SHA>` (a trailing
  `# vX.Y.Z` comment says which release it is; both are moved together by hand);
- every FetchContent `GIT_TAG` in the two CMakeLists.txt is a 40-hex commit SHA;
- the Kodi tree the build jobs compile against is a full commit SHA (`KODI_COMMIT`), and no job clones it by branch
  (docs/OPEN_ITEMS.md, "Kodi is cloned from a moving branch in the release builds", closed 2026-10-06);
- the Windows prebuilt archives are fetched with a SHA256 pinned next to each
  name, and docs/BUILDING.md's copy of that step pins the same values (the two
  drifting apart is the usual way such a copy goes stale);
- every `pip install` in a workflow reads a requirements file, and every line of those files is `name==version`
  (2026-10-25: the lint tools were pinned inline in build.yml, and a dev workspace kept its own copy by hand).

Reads this repo's real files, deliberately -- the real files are what is checked.
"""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = sorted((REPO_ROOT / ".github" / "workflows").glob("*.yml"))
CMAKE_FILES = [REPO_ROOT / "CMakeLists.txt", REPO_ROOT / "tests" / "CMakeLists.txt"]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
BUILDING_MD = REPO_ROOT / "docs" / "BUILDING.md"

SHA1 = re.compile(r"^[0-9a-f]{40}$")
ARCHIVE_ROW = re.compile(r'file\s*=\s*"([^"]+\.7z)";\s*sha256\s*=\s*"([0-9a-f]{64})"')


def test_workflows_exist():
    assert WORKFLOWS, "no workflow files found -- the pin checks below would pass vacuously"


def test_every_workflow_action_is_pinned_to_a_commit_sha():
    unpinned = []
    for workflow in WORKFLOWS:
        for lineno, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), 1):
            match = re.match(r"\s*-?\s*uses:\s*(\S+)", line)
            if not match:
                continue
            ref = match.group(1)
            if ref.startswith("./"):  # a local action lives in this repo
                continue
            _, _, version = ref.partition("@")
            if not SHA1.match(version):
                unpinned.append(f"{workflow.name}:{lineno}: {ref}")
    assert not unpinned, "actions not pinned to a commit SHA:\n" + "\n".join(unpinned)


def test_every_pinned_action_says_which_release_it_is():
    missing = []
    for workflow in WORKFLOWS:
        for lineno, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), 1):
            if re.match(r"\s*-?\s*uses:\s*\S+@[0-9a-f]{40}", line) and not re.search(r"#\s*v\d", line):
                missing.append(f"{workflow.name}:{lineno}")
    assert not missing, "pinned actions without a `# vX.Y.Z` comment:\n" + "\n".join(missing)


def test_every_fetchcontent_git_tag_is_a_commit_sha():
    unpinned = []
    seen = 0
    for cmake_file in CMAKE_FILES:
        for lineno, line in enumerate(cmake_file.read_text(encoding="utf-8").splitlines(), 1):
            match = re.match(r"\s*GIT_TAG\s+(\S+)", line)
            if not match:
                continue
            seen += 1
            if not SHA1.match(match.group(1)):
                unpinned.append(f"{cmake_file.relative_to(REPO_ROOT)}:{lineno}: {match.group(1)}")
    assert seen, "found no GIT_TAG lines -- the check would pass vacuously"
    assert not unpinned, "GIT_TAG not a commit SHA:\n" + "\n".join(unpinned)


def test_windows_archives_are_hash_pinned_and_buildmd_matches():
    workflow_rows = ARCHIVE_ROW.findall(BUILD_WORKFLOW.read_text(encoding="utf-8"))
    doc_rows = ARCHIVE_ROW.findall(BUILDING_MD.read_text(encoding="utf-8"))
    assert len(workflow_rows) == 3, "expected curl, openssl and zlib pinned in the Windows job"
    assert sorted(workflow_rows) == sorted(doc_rows), "docs/BUILDING.md pins differ from the workflow's"


def test_windows_job_no_longer_registers_a_remote_url_with_kodis_add_internal():
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    assert "mirrors.kodi.tv/build-deps/win32/curl" not in text.replace(
        "https://mirrors.kodi.tv/build-deps/win32/$($a.file)", ""
    ), "an archive is registered by remote URL again, bypassing the SHA256 check"


def test_kodi_is_built_from_a_pinned_commit_not_a_moving_branch():
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    match = re.search(r"^\s*KODI_COMMIT:\s*(\S+)", text, re.MULTILINE)
    assert match, "build.yml has no KODI_COMMIT"
    assert SHA1.match(match.group(1)), f"KODI_COMMIT is not a full commit SHA: {match.group(1)}"
    # Every job that fetches Kodi must use it; a clone by branch (or tag) would bypass the pin.
    clones = [line.strip() for line in text.splitlines() if re.search(r"git\s+clone\b.*xbmc", line)]
    assert not clones, "a job clones Kodi by branch instead of using KODI_COMMIT:\n" + "\n".join(clones)
    fetches = re.findall(r"git -C kodi-source fetch\b[^\n]*", text)
    # the Unix and Windows build jobs, and the glue-harness job (headers only)
    assert len(fetches) == 3, f"expected three jobs to fetch KODI_COMMIT, found {len(fetches)}"
    assert all("KODI_COMMIT" in line for line in fetches), fetches


REQUIREMENT_FILES = [
    REPO_ROOT / "tools" / "requirements-lint.txt",
    REPO_ROOT / "dispatcharr-plugin" / "requirements-dev.txt",
]


def test_every_pip_install_in_a_workflow_reads_a_requirements_file():
    # A version written inline in a workflow is a second copy of the requirements file's pin, and the copies drift: a
    # dev workspace installs the file (tools/requirements-lint.txt says so), CI would run the inline one.
    inline = []
    for workflow in WORKFLOWS:
        for lineno, line in enumerate(workflow.read_text(encoding="utf-8").splitlines(), 1):
            if re.search(r"\bpip3?\s+install\b", line) and not re.search(r"\s-r\s+\S+\.txt\b", line):
                inline.append(f"{workflow.name}:{lineno}: {line.strip()}")
    assert not inline, "pip install without -r <requirements file>:\n" + "\n".join(inline)


def test_every_requirements_file_a_workflow_installs_exists_and_is_listed_here():
    installed = set()
    for workflow in WORKFLOWS:
        installed.update(re.findall(r"\bpip3?\s+install\b.*?\s-r\s+(\S+\.txt)\b", workflow.read_text(encoding="utf-8")))
    assert installed, "no workflow installs a requirements file -- the pin checks below would pass vacuously"
    assert installed == {str(path.relative_to(REPO_ROOT)) for path in REQUIREMENT_FILES}


def test_every_requirement_is_pinned_to_an_exact_version():
    loose = []
    for path in REQUIREMENT_FILES:
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = line.split("#", 1)[0].strip()
            if line and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*==[0-9][0-9A-Za-z.]*", line):
                loose.append(f"{path.relative_to(REPO_ROOT)}:{lineno}: {line}")
    assert not loose, "requirements not pinned with ==:\n" + "\n".join(loose)
