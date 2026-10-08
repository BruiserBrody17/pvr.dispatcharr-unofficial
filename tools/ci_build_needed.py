"""Decides whether CI's compile-and-package jobs have anything to do: `build` for build-unix, build-windows and
glue-harness, `plugins` for package-dispatcharr-plugins.

A push that touches only docs, packaging, the changelog or tools that no build step runs cannot change what those jobs
produce, and the Windows leg is the slowest. Lint and every unit-test job still run on every push: the
cross-file checks under tools/tests (version sync, doc citations, pins) catch a "docs only" commit that was not.

Prints `build=<bool>` and `plugins=<bool>` (the form $GITHUB_OUTPUT wants). When in doubt it says true: a tag, a manual
run, a base commit it cannot see (a force-push, a new branch) or a failing `git diff` all build.

usage: ci_build_needed.py <event> <ref> <base-sha> <head-sha>
"""

import re
import subprocess
import sys

# What the build jobs read: the addon and its metadata, the tests and the glue harness (which drives the plugin too),
# the top-level CMakeLists.txt, the workflow itself, and the two tools the zip steps call.
BUILD_RELEVANT = re.compile(
    r"^(src/|tests/|pvr\.dispatcharr-unofficial/|dispatcharr-plugin/|\.github/workflows/"
    r"|CMakeLists\.txt$"
    r"|tools/(normalize_zip|check_release_zip|ci_build_needed)\.py$)"
)

# What package-dispatcharr-plugins reads: the plugin directories it zips, the workflow, and the same two zip tools.
PLUGINS_RELEVANT = re.compile(
    r"^(dispatcharr-plugin/|\.github/workflows/|tools/(normalize_zip|check_release_zip|ci_build_needed)\.py$)"
)


def needs_build(changed_files):
    return any(BUILD_RELEVANT.match(path) for path in changed_files)


def needs_plugins(changed_files):
    return any(PLUGINS_RELEVANT.match(path) for path in changed_files)


def changed_files(event, ref, base, head, run_git=None):
    """The files the push changed, or None when that cannot be told (every job then runs)."""
    if event not in ("push", "pull_request") or ref.startswith("refs/tags/"):
        return None
    if not base or not head or set(base) == {"0"}:
        return None
    run_git = run_git or _git
    try:
        return run_git(["diff", "--name-only", base, head]).splitlines()
    except (subprocess.CalledProcessError, OSError):
        return None


def decide(event, ref, base, head, run_git=None, relevant=needs_build):
    """True when the jobs `relevant` stands for should run."""
    files = changed_files(event, ref, base, head, run_git)
    return True if files is None else relevant(files)


def _git(args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def main(argv):
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    event, ref, base, head = argv[1:]
    files = changed_files(event, ref, base, head)
    for name, relevant in (("build", needs_build), ("plugins", needs_plugins)):
        print("%s=%s" % (name, "true" if files is None or relevant(files) else "false"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
