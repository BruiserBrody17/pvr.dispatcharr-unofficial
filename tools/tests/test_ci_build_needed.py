"""tools/ci_build_needed.py: which pushes the compile-and-package CI jobs run for."""

import importlib.util
import re
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("ci_build_needed", REPO_ROOT / "tools" / "ci_build_needed.py")
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)


@pytest.mark.parametrize(
    "path",
    [
        "src/DispatcharrClient.cpp",
        "tests/test_staleness.cpp",
        "tests/glue/ci.sh",
        "CMakeLists.txt",
        "pvr.dispatcharr-unofficial/addon.xml.in",
        "pvr.dispatcharr-unofficial/resources/settings.xml",
        "dispatcharr-plugin/timeshift_buffer/plugin.py",
        ".github/workflows/build.yml",
        "tools/normalize_zip.py",
        "tools/check_release_zip.py",
        "tools/ci_build_needed.py",
    ],
)
def test_a_path_the_build_reads_triggers_it(path):
    assert gate.needs_build(["docs/BUILDING.md", path])


@pytest.mark.parametrize(
    "path",
    [
        "docs/OPEN_ITEMS.md",
        "README.md",
        "CHANGELOG.md",
        "CLAUDE.md",
        "packaging/coreelec/pvr.dispatcharr-unofficial/package.mk",
        "tools/scrub_zip_paths.py",
        "tools/tests/test_scrub_zip_paths.py",
        "tools/kodi_smoke_test.py",
        "srcs/readme.txt",  # a directory that only starts like one the build reads
        "CMakeLists.txt.bak",
    ],
)
def test_a_path_no_build_step_reads_does_not(path):
    assert not gate.needs_build([path])


def test_an_empty_diff_builds_nothing():
    assert not gate.needs_build([])


def test_every_tool_the_build_jobs_run_is_on_the_trigger_list():
    workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
    # the two zip tools are the ones the compile jobs call; a new one added there must be added to BUILD_RELEVANT
    used = set(re.findall(r"tools/(normalize_zip|check_release_zip)\.py", workflow))
    assert used == {"normalize_zip", "check_release_zip"}
    for name in used:
        assert gate.needs_build(["tools/%s.py" % name])


def _fake_git(output=None, error=None):
    """A git whose diff lists the names of `output` (one per line, written NUL-separated as `-z` makes git print
    them)."""

    def run(args):
        assert args[:5] == ["-c", "core.quotepath=off", "diff", "-z", "--name-only"]
        if error:
            raise error
        return output.replace("\n", "\0")

    return run


def test_a_docs_only_push_skips_the_build():
    assert not gate.decide(
        "push", "refs/heads/Omega", "a" * 40, "b" * 40, _fake_git("docs/BUILDING.md\nCHANGELOG.md\n")
    )


def test_a_push_touching_source_builds():
    assert gate.decide("push", "refs/heads/Omega", "a" * 40, "b" * 40, _fake_git("docs/x.md\nsrc/PVRDispatcharr.cpp\n"))


def test_a_pull_request_is_judged_the_same_way():
    assert not gate.decide("pull_request", "refs/merge/3", "a" * 40, "b" * 40, _fake_git("README.md\n"))
    assert gate.decide("pull_request", "refs/merge/3", "a" * 40, "b" * 40, _fake_git("src/XmlTvParser.cpp\n"))


@pytest.mark.parametrize(
    "event,ref,base,head",
    [
        ("push", "refs/tags/0.12.0", "a" * 40, "b" * 40),  # a release builds everything
        ("workflow_dispatch", "refs/heads/Omega", "", ""),
        ("push", "refs/heads/Omega", "0" * 40, "b" * 40),  # a new branch has no base
        ("push", "refs/heads/Omega", "", "b" * 40),
    ],
)
def test_anything_unclear_builds(event, ref, base, head):
    assert gate.decide(event, ref, base, head, _fake_git("docs/x.md\n"))


@pytest.mark.parametrize("error", [subprocess.CalledProcessError(128, "git"), FileNotFoundError("git")])
def test_a_git_failure_builds(error):
    assert gate.decide("push", "refs/heads/Omega", "a" * 40, "b" * 40, _fake_git(error=error))


def test_a_forced_push_over_an_unknown_base_builds_through_the_real_git(tmp_path):
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    result = subprocess.run(
        ["python3", str(REPO_ROOT / "tools" / "ci_build_needed.py"), "push", "refs/heads/Omega", "a" * 40, "b" * 40],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    assert result.stdout.split() == ["build=true", "plugins=true"]


def _git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


@pytest.mark.parametrize("target", ["docs/Staleness.h", "tools/old_ci_build_needed.py"])
def test_a_file_renamed_out_of_a_build_path_builds_through_the_real_git(tmp_path, target):
    # Rename detection would list only the new path, which no build step reads.
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "config", "user.email", "t@example.invalid")
    _git(tmp_path, "config", "user.name", "t")
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "Staleness.h").write_text("#pragma once\n" + "// a line\n" * 40)
    _git(tmp_path, "add", ".")
    _git(tmp_path, "commit", "-q", "-m", "base")
    base = _git(tmp_path, "rev-parse", "HEAD")
    (tmp_path / target).parent.mkdir(exist_ok=True)
    _git(tmp_path, "mv", "src/Staleness.h", target)
    _git(tmp_path, "commit", "-q", "-m", "move")
    head = _git(tmp_path, "rev-parse", "HEAD")
    result = subprocess.run(
        ["python3", str(REPO_ROOT / "tools" / "ci_build_needed.py"), "push", "refs/heads/Piers", base, head],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    assert "build=true" in result.stdout.split()


@pytest.mark.parametrize(
    "name",
    [
        "src/Ünïcode.cpp",
        'src/a"quote.h',
        "src/back\\slash.h",
        "src/tab\there.h",
        "dispatcharr-plugin/recording_edl/é.py",
    ],
)
def test_a_name_git_would_quote_still_builds_through_the_real_git(tmp_path, name):
    # The 2026-11-10 sweep's finding: git C-quoted such a name with a leading `"`, which no start-anchored pattern
    # matched, so a push changing only that file skipped the build.
    base, head = _commit_one_file(tmp_path, name)
    assert _git(tmp_path, "diff", "--name-only", base, head).startswith('"')  # the case this pins
    assert gate.changed_files("push", "refs/heads/Piers", base, head, lambda args: _git_raw(tmp_path, args)) == [name]
    expected_plugins = "true" if name.startswith("dispatcharr-plugin/") else "false"
    assert _run_script(tmp_path, base, head) == ["build=true", "plugins=" + expected_plugins]


def test_a_name_that_is_not_utf8_builds_rather_than_crashing(tmp_path):
    # Quoted (`"src/\377.cpp"`) before the fix; now that git hands over raw names, a strict UTF-8 decode would raise
    # on it instead, and the step's pipefail would fail the run.
    base, head = _commit_one_file(tmp_path, "src/\udcff.cpp")  # surrogateescape: the file name is the byte 0xff
    assert _run_script(tmp_path, base, head) == ["build=true", "plugins=false"]


def _commit_one_file(repo, name):
    """(base, head) of a temporary repository where head adds `name`, with core.quotepath on (git's default, and what
    a CI runner's global config may set: the tool must not depend on it)."""
    _git(repo, "init", "-q")
    _git(repo, "config", "user.email", "t@example.invalid")
    _git(repo, "config", "user.name", "t")
    _git(repo, "config", "core.quotepath", "on")
    (repo / "docs").mkdir()
    (repo / "docs" / "x.md").write_text("x\n")
    _git(repo, "add", ".")
    _git(repo, "commit", "-q", "-m", "base")
    base = _git(repo, "rev-parse", "HEAD")
    (repo / name).parent.mkdir(parents=True, exist_ok=True)
    (repo / name).write_text("x\n")
    _git(repo, "add", ".")
    _git(repo, "commit", "-q", "-m", "add")
    return base, _git(repo, "rev-parse", "HEAD")


def _run_script(repo, base, head):
    script = str(REPO_ROOT / "tools" / "ci_build_needed.py")
    result = subprocess.run(
        ["python3", script, "push", "refs/heads/Piers", base, head], cwd=repo, capture_output=True, text=True
    )
    return result.stdout.split()


def _git_raw(cwd, args):
    output = subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True).stdout
    return output.decode("utf-8", errors="surrogateescape")


def test_the_decide_step_runs_with_pipefail_and_the_jobs_build_on_an_empty_answer():
    # A crash of the script piped through tee must fail the step, not leave empty outputs that skip every build job.
    workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
    step = workflow[workflow.index("id: decide") :]
    step = step[: step.index("\n\n")]
    assert re.search(r"^\s+shell: bash$", step, re.M)
    assert "outputs.build == 'true'" not in workflow and "outputs.plugins == 'true'" not in workflow
    assert workflow.count("needs.changes.outputs.build != 'false'") == 3
    assert workflow.count("needs.changes.outputs.plugins != 'false'") == 1


@pytest.mark.parametrize(
    "path",
    [
        "dispatcharr-plugin/timeshift_buffer/plugin.py",
        "dispatcharr-plugin/recording_edl/plugin.json",
        ".github/workflows/build.yml",
        "tools/normalize_zip.py",
        "tools/check_release_zip.py",
        "tools/ci_build_needed.py",
    ],
)
def test_a_path_the_plugin_packaging_reads_triggers_it(path):
    assert gate.needs_plugins(["docs/BUILDING.md", path])


@pytest.mark.parametrize(
    "path",
    [
        "src/DispatcharrClient.cpp",
        "tests/glue/ci.sh",  # builds the addon; the plugin zips are made from dispatcharr-plugin/ alone
        "CMakeLists.txt",
        "pvr.dispatcharr-unofficial/addon.xml.in",
        "docs/TIMESHIFT.md",
        "packaging/coreelec/pvr.dispatcharr-unofficial/package.mk",
        "tools/scrub_zip_paths.py",
        "dispatcharr-plugins/x.py",  # only starts like the real directory
    ],
)
def test_a_path_the_plugin_packaging_does_not_read_does_not(path):
    assert not gate.needs_plugins([path])


def test_the_two_job_groups_are_decided_separately():
    run = _fake_git("src/XmlTvParser.cpp\n")
    assert gate.decide("push", "refs/heads/Omega", "a" * 40, "b" * 40, run, gate.needs_build)
    assert not gate.decide("push", "refs/heads/Omega", "a" * 40, "b" * 40, run, gate.needs_plugins)
    run = _fake_git("dispatcharr-plugin/timeshift_buffer/plugin.py\n")
    assert gate.decide("push", "refs/heads/Omega", "a" * 40, "b" * 40, run, gate.needs_plugins)


def test_everything_the_workflow_zips_is_on_the_plugin_list():
    workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
    # the job zips each plugin's directory under dispatcharr-plugin/ and runs these two tools over the result
    assert "cd dispatcharr-plugin" in workflow
    for name in ("normalize_zip", "check_release_zip"):
        assert gate.needs_plugins(["tools/%s.py" % name])


@pytest.mark.parametrize(
    "args,expected",
    [
        (["push", "refs/tags/0.12.0", "a" * 40, "b" * 40], "build=true\nplugins=true"),
        (["push", "refs/heads/Omega", "0" * 40, "b" * 40], "build=true\nplugins=true"),
    ],
)
def test_main_prints_both_outputs_and_builds_everything_when_unclear(args, expected, capsys):
    assert gate.main(["ci_build_needed.py", *args]) == 0
    assert capsys.readouterr().out.strip() == expected
