#!/usr/bin/env python3
"""Project-specific validation of .github/workflows/*.yml.

These are checks that `actionlint` cannot know about — they tie the workflow to
*this* codebase:

  1. Every `cmake --build ... --target <name>` in a workflow refers to a target
     actually defined in CMakeLists.txt (catches a typo like `gridtv_devtest`
     that would silently break a CI build step).
  2. Every third-party `uses:` action is pinned to a version tag or commit SHA,
     never a moving branch like @main / @master.
  3. Each workflow file parses as YAML (when pyyaml is available).

Run via `scripts/check-ci.sh`, or directly:
    python3 scripts/_check_workflow.py
Exit codes: 0 = clean, 1 = problems found, 2 = environment error.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
WF_DIR = ROOT / ".github" / "workflows"
CMAKE = ROOT / "CMakeLists.txt"

# Valid `--target` names that are not add_executable/add_library in CMakeLists.
BUILT_IN_TARGETS = {"all", "help", "clean", "install", "test", "rebuild_cache"}
# Branch refs an action must never be pinned to.
MOVING_REFS = {"main", "master", "develop", "HEAD", "latest"}


def cmake_targets():
    """Names from add_executable(...) / add_library(...) in CMakeLists.txt."""
    return set(
        re.findall(
            r"add_(?:executable|library)\(\s*([A-Za-z0-9_]+)",
            CMAKE.read_text(),
        )
    )


def workflow_files():
    return sorted(WF_DIR.glob("*.yml")) + sorted(WF_DIR.glob("*.yaml"))


def check_targets(text, targets, name):
    """Every CMake target named in a `--target ...` step must be defined."""
    errs = []
    for line in text.splitlines():
        for grp in re.findall(r"--target\s+([A-Za-z0-9_ ]+)", line):
            for tok in grp.split():
                if tok not in BUILT_IN_TARGETS and tok not in targets:
                    errs.append(
                        "%s: references CMake target '%s', "
                        "not defined in CMakeLists.txt" % (name, tok)
                    )
    return errs


def check_pins(text, name):
    """No `uses:` action pinned to a moving branch."""
    errs = []
    for ref in re.findall(r"uses:\s*[A-Za-z0-9_./-]+@([A-Za-z0-9_./-]+)", text):
        if ref in MOVING_REFS:
            errs.append(
                "%s: action pinned to moving ref '@%s'; "
                "use a tag (e.g. @v4) or commit SHA" % (name, ref)
            )
    return errs


def main():
    if not CMAKE.exists():
        print("  \u2717 CMakeLists.txt not found at %s" % CMAKE, file=sys.stderr)
        return 2
    if not WF_DIR.exists():
        print("  \u2717 no .github/workflows directory", file=sys.stderr)
        return 2

    try:
        import yaml
    except ImportError:
        yaml = None

    targets = cmake_targets()
    errors = []
    n_files = 0

    for f in workflow_files():
        n_files += 1
        text = f.read_text()

        if yaml is not None:
            try:
                yaml.safe_load(text)
            except Exception as exc:  # noqa: BLE001
                errors.append("%s: not valid YAML: %s" % (f.name, exc))
        else:
            print("  (pyyaml not installed; skipping YAML-parse check)")

        errors += check_targets(text, targets, f.name)
        errors += check_pins(text, f.name)

    if errors:
        for e in errors:
            print("  \u2717 " + e)
        print("\n  %d problem(s) in %d workflow file(s)." % (len(errors), n_files))
        return 1

    print(
        "  \u2714 %d workflow file(s) OK; cross-checked against %d CMake target(s)."
        % (n_files, len(targets))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
