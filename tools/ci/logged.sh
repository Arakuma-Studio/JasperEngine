#!/usr/bin/env bash
# Runs one lane command of .github/workflows/ci.yml with its output also in build/test-results/<name>.log,
# whose first line is `$ <command>`: tools/ci/report.py gives that command as the repro of the failures
# it finds in the log. The command is a shell command line, as a developer types it.
#
#   tools/ci/logged.sh <name> '<command>'
#
# Exits with the command's status.
set -uo pipefail

[[ $# -eq 2 && "$1" =~ ^[A-Za-z0-9_.-]+$ ]] || { sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2
mkdir -p build/test-results
LOG="build/test-results/$1.log"
printf '$ %s\n' "$2" | tee "$LOG"
bash -c "$2" 2>&1 | tee -a "$LOG"
exit "${PIPESTATUS[0]}"
