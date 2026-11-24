#!/usr/bin/env bash
# The free-disk action's work (action.yml): when / has less free than the job needs, remove the
# preinstalled Android SDK, .NET, GHC, the CodeQL bundle and every Docker image.
#
#   NEED=<GB> .github/actions/free-disk/free-disk.sh
#
# NEED is the job's measured peak with a margin; without it the removal always runs. The removal
# takes 1-2.5 min of a job, and an ubuntu-latest runner starts with about 87 GB free of 145.
set -euo pipefail

avail_gb() { df -BG --output=avail / | tail -n 1 | tr -dc '0-9'; }

# an overflow runner is a person's machine: what is installed there is theirs (action.yml skips it too)
if [[ "${RUNNER_ENVIRONMENT:-}" == self-hosted ]]; then
    echo "free-disk: a self-hosted runner: nothing removed"
    exit 0
fi

need="${NEED:-}"
if [[ -n "$need" && ! "$need" =~ ^[0-9]+$ ]]; then
    echo "free-disk: NEED='$need' is not a whole number of GB" >&2
    exit 2
fi
avail="$(avail_gb)"
if [[ -n "$need" ]] && (( avail >= need )); then
    echo "free-disk: $avail GB free, the job needs $need: nothing removed"
    exit 0
fi

df -h /
# in parallel: each removal is a few GB of small files
for dir in /usr/local/lib/android /usr/share/dotnet /opt/ghc /usr/local/.ghcup /opt/hostedtoolcache/CodeQL; do
    sudo rm -rf "$dir" &
done
# no container runs yet, so every image is unused
docker image prune --all --force > /dev/null &
wait
df -h /
avail="$(avail_gb)"
if [[ -n "$need" ]] && (( avail < need )); then
    echo "::warning::free-disk: $avail GB free after the removal, under the $need GB the job needs"
fi
