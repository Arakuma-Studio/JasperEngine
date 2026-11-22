#!/usr/bin/env bash
# Runs a download command of CI (curl, wget, brew, choco, sdkmanager, gh release download) and runs it
# again when it fails, so a one-off network failure does not fail the job. Never wrap a build or a test
# in it: a flaky test gets DISABLED_ and an issue (AGENTS.md), not a retry.
#
#   tools/ci/retry.sh <command> [<arg>...]
#
# RETRY_ATTEMPTS tries in all (default 5); the pause after try k is k * RETRY_DELAY seconds (default 5).
# Each retry is logged on stderr (a warning annotation on GitHub Actions). Exits 0 on the first success,
# else with the last try's status.
set -uo pipefail

ATTEMPTS="${RETRY_ATTEMPTS:-5}"
DELAY="${RETRY_DELAY:-5}"
if [[ $# -eq 0 || ! "$ATTEMPTS" =~ ^[1-9][0-9]*$ || ! "$DELAY" =~ ^[0-9]+$ ]]; then
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi

PREFIX=""
[[ "${GITHUB_ACTIONS:-}" == true ]] && PREFIX="::warning::"
COMMAND="$*"

try=1
while true; do
    "$@"
    status=$?
    [[ $status == 0 ]] && exit 0
    if [[ $try -ge $ATTEMPTS ]]; then
        echo "${PREFIX}retry: $COMMAND failed (exit $status); gave up after $ATTEMPTS tries" >&2
        exit "$status"
    fi
    pause=$((try * DELAY))
    echo "${PREFIX}retry: $COMMAND failed (exit $status), try $try of $ATTEMPTS; retrying in $pause s" >&2
    sleep "$pause"
    try=$((try + 1))
done
