#!/usr/bin/env bash
#
# check-ci.sh — lint and validate the GitHub Actions workflow(s).
#
# Two layers:
#   1. actionlint   — the canonical static checker for GitHub Actions
#                     (syntax, expression contexts, job deps, matrix, ...).
#   2. _check_workflow.py — project-specific checks this repo cares about
#                     (every CMake --target in the workflow really exists;
#                     actions are version-pinned, not on @main).
#
# Run locally:    scripts/check-ci.sh
# In CI:          the 'lint' job in .github/workflows/build.yml.
# Exit non-zero if any check fails.
set -euo pipefail
cd "$(dirname "$0")/.."

status=0

echo "==> 1/2  actionlint (GitHub Actions static checker)"
if command -v actionlint >/dev/null 2>&1; then
    # Pass workflow paths explicitly: with no arguments actionlint tries to
    # auto-discover via a git repo, which doesn't exist before `git init`.
    shopt -s nullglob
    wfs=(.github/workflows/*.yml .github/workflows/*.yaml)
    shopt -u nullglob
    if [ "${#wfs[@]}" -gt 0 ]; then
        actionlint -color "${wfs[@]}" || status=1
    else
        echo "  no workflow files found in .github/workflows"
    fi
else
    echo "  actionlint not installed — skipping."
    echo "  Install:  brew install actionlint   (macOS)"
    echo "            bash <(curl -s https://raw.githubusercontent.com/rhysd/actionlint/main/scripts/download-actionlint.bash)"
fi

echo "==> 2/2  project-specific workflow checks"
python3 scripts/_check_workflow.py || status=1

if [ "$status" -eq 0 ]; then
    echo "==> all CI checks passed"
else
    echo "==> CI checks FAILED" >&2
fi
exit "$status"
