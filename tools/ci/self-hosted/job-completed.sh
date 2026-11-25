#!/usr/bin/env bash
# An overflow runner's job-completed hook (ACTIONS_RUNNER_HOOK_JOB_COMPLETED; docs/testing.md
# "Overflow runners"): deletes the job's workspace (only a folder under this runner's _work), so the
# build trees and restored caches never pile up; on Linux removes the Docker images CI pulled
# (.github/actions/image notes them in the tool cache) that no job used for KEEP_DAYS, or every one but
# the newest of each when the disk is short, and cuts the CI image builder's cache; on macOS empties
# the runner's own home's caches (Xcode's DerivedData among them) past HOME_CAP_GB. Never fails a job.
set -uo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
KEEP_DAYS=3
SHORT_GB=100
HOME_CAP_GB=20
free_gb() { df -Pk "$DIR" | awk 'NR == 2 {print int($4 / 1048576)}'; }

ws="${GITHUB_WORKSPACE:-}"
case "$ws" in
    "$DIR/_work/"?*) ;;
    *) echo "job-completed: workspace '$ws' is not under $DIR/_work: left alone"; ws="" ;;
esac
ledger="$DIR/_work/_tool/je-ci-images.txt"
if [[ -n "$ws" && -d "$ws" ]]; then
    find "$ws" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2> /dev/null
    if [[ -n "$(ls -A "$ws" 2> /dev/null)" && -f "$ledger" ]] && command -v docker > /dev/null; then
        # files a container left as another user: removed from inside one of CI's own images
        image="$(tail -n 1 "$ledger" | cut -d' ' -f1)"
        docker run --rm -v "$ws:/w" "$image" find /w -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2> /dev/null
    fi
    echo "job-completed: emptied $ws"
fi

if [[ "$(uname -s)" == Linux && -f "$ledger" ]] && command -v docker > /dev/null; then
    now="$(date +%s)"
    max_age=$((KEEP_DAYS * 86400))
    (( $(free_gb) < SHORT_GB )) && max_age=0
    # each image's last use, newest first; the newest of each repository stays
    declare -A seen=()
    kept=()
    while read -r when ref; do
        repo="${ref%:*}"
        if [[ -z "${seen[$repo]:-}" ]] || (( now - when <= max_age )) || ! docker image rm "$ref" > /dev/null 2>&1; then
            kept+=("$ref $when")
        else
            echo "job-completed: removed $ref (last used $(( (now - when) / 3600 )) h ago)"
        fi
        seen[$repo]=1
    done < <(awk '{last[$1] = $2} END {for (r in last) print last[r], r}' "$ledger" | sort -rn)
    printf '%s\n' "${kept[@]}" > "$ledger.new" && mv "$ledger.new" "$ledger"
    if docker buildx inspect jasper-ci > /dev/null 2>&1; then
        docker buildx prune --builder jasper-ci --force --keep-storage 10gb > /dev/null 2>&1
    fi
fi

if [[ "$(uname -s)" == Darwin && -d "$DIR/home" ]]; then
    used_gb=$(( $(du -sk "$DIR/home" | cut -f1) / 1048576 ))
    if (( used_gb > HOME_CAP_GB )); then
        rm -rf "$DIR/home/Library/Caches" "$DIR/home/Library/Developer/Xcode/DerivedData" "$DIR/home/.cache"
        echo "job-completed: the runner's home held $used_gb GB: its caches emptied"
    fi
fi
echo "job-completed: $(free_gb) GB free"
exit 0
