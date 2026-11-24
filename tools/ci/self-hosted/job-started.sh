#!/usr/bin/env bash
# An overflow runner's job-started hook (ACTIONS_RUNNER_HOOK_JOB_STARTED; docs/testing.md "Overflow
# runners"), run before any step of a job: it fails the job unless it is this repository's push, merge
# queue, dispatch or pull request from one of its own branches. The workflow file a pull request runs
# is the pull request's, so this check, not ci.yml's routing, is what keeps a fork's code off the
# machine. On macOS it also keeps the machine awake until the job ends.
set -euo pipefail
REPO=Arakuma-Studio/JasperEngine
refuse() { echo "::error::overflow runner $(hostname): $1: not run here"; exit 1; }

[[ "${GITHUB_REPOSITORY:-}" == "$REPO" ]] || refuse "repository '${GITHUB_REPOSITORY:-}'"
case "${GITHUB_EVENT_NAME:-}" in
    push|merge_group|workflow_dispatch) ;;
    pull_request)
        # (a deleted fork has no head repository: the lookup fails, and so does the job)
        head="$(python3 -I -c 'import json, os, sys
e = json.load(open(os.environ["GITHUB_EVENT_PATH"]))
print(e["pull_request"]["head"]["repo"]["full_name"])')" || refuse "a pull request without a head repository"
        [[ "$head" == "$REPO" ]] || refuse "a pull request from $head"
        ;;
    *) refuse "event '${GITHUB_EVENT_NAME:-}'" ;;
esac
echo "overflow runner $(hostname): $GITHUB_EVENT_NAME of $GITHUB_REPOSITORY, $(df -Pk "$(dirname "$0")" | awk 'NR == 2 {printf "%d GB free", $4 / 1048576}')"

if [[ "$(uname -s)" == Darwin ]]; then
    # idle and system sleep (on AC power) held off while the job's worker runs; a closed lid still sleeps
    worker="$(pgrep -n -f Runner.Worker || true)"
    [[ -n "$worker" ]] && nohup caffeinate -i -s -w "$worker" > /dev/null 2>&1 &
fi
exit 0
