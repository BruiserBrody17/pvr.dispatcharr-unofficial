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
    def run(args):
        assert args[:2] == ["diff", "--name-only"]
        if error:
            raise error
        return output

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
