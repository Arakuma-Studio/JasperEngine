#!/usr/bin/env bash
# ccache-save's work (action.yml): prints the run's ccache statistics, evicts every entry the run neither
# used nor wrote (a hit refreshes an entry's mtime), so what remains is the lane's working set, and decides
# whether to save it: on push to main or a schedule, when the run wrote something, after cutting it to the
# lane's cap, recompressing it smaller and zeroing the statistics (the next run's are then its own). The
# build itself runs without a cap (ccache-restore): a cleanup in the middle of a build evicts the restored
# entries it has not reached yet, and every miss after that evicts more.
#
#   LANE=<lane> EVENT=<event> REF=<ref> .github/actions/ccache-save/trim.sh
#
# Run from the repository root after ccache-restore. Writes save=true|false to $GITHUB_OUTPUT.
set -euo pipefail

DIR=build/.cache/ccache
# the level a saved entry is recompressed to (below)
RECOMPRESS_LEVEL=9
out() { echo "save=$1" >> "${GITHUB_OUTPUT:-/dev/stdout}"; }

if [[ -z "${JASPER_CCACHE_LANE:-}" ]]; then
    echo "ccache-save: ccache-restore did not run"; out false; exit 0
fi
if [[ "$JASPER_CCACHE_LANE" != "$LANE" ]]; then
    echo "::error::ccache-save: lane $LANE, but ccache-restore ran for '$JASPER_CCACHE_LANE'"; exit 1
fi

# an overflow runner's own cache (ccache-restore): capped by CCACHE_MAXSIZE as it builds, kept whole,
# never saved (the run-start eviction below would empty it of every other job's entries)
if [[ "${JASPER_CCACHE_LOCAL:-}" == 1 ]]; then
    ccache --show-stats | grep -E 'Hits|Misses|Cache size' || true
    echo "ccache-save: the runner's own cache at $CCACHE_DIR: kept, not saved"; out false; exit 0
fi

# ccache of the lane: the runner's on Windows and macOS (ccache-restore installs it), the lane's build
# image's on Linux, whose runner has none (and whose ccache wrote the entries); CAP is its max size
CAP=0
run_ccache() {
    if [[ "$RUNNER_OS" == Linux ]]; then
        [[ -n "${JASPER_DOCKER_IMAGE:-}" ]] || { echo "ccache-save: no build image to run ccache in"; return 1; }
        docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/$DIR:/ccache" -e CCACHE_DIR=/ccache \
            -e CCACHE_MAXSIZE="$CAP" "$JASPER_DOCKER_IMAGE" ccache "$@"
    else
        CCACHE_MAXSIZE="$CAP" ccache "$@"
    fi
}

save=false
if [[ "$EVENT" == schedule || ( "$EVENT" == push && "$REF" == refs/heads/main ) ]]; then
    # an object or manifest written since the restore (ccache rewrites its stats files on every hit)
    new="$(find "$DIR" -type f -newer "$JASPER_CCACHE_MARKER" ! -name stats ! -name '*.lock' \
        ! -name ccache.conf ! -path '*/tmp/*' -print -quit 2> /dev/null || true)"
    if [[ -n "$new" ]]; then save=true; else echo "ccache-save: nothing new since the restore"; fi
else
    echo "ccache-save: $EVENT runs restore only"
fi

echo "ccache-save: restored $JASPER_CCACHE_RESTORED_SIZE; this run's statistics:"
run_ccache --show-stats --verbose || { echo "ccache-save: ccache did not run; nothing saved"; out false; exit 0; }
du -sh "$DIR"

# (one second more: an entry touched in the restore's second stays)
age=$(( $(date +%s) - JASPER_CCACHE_START + 1 ))
run_ccache --evict-older-than "${age}s" > /dev/null || { echo "ccache-save: eviction failed; nothing saved"; out false; exit 0; }
echo "ccache-save: the run's working set (entries used or written), cap $JASPER_CCACHE_CAP:"
du -sh "$DIR"

if [[ "$save" == true ]]; then
    # only a working set over the cap is cut: ccache's cleanup holds each of its 16 subdirectories to a
    # 16th of the cap and cuts one over that to 80 % of it, so under the cap it still evicted the
    # fullest subdirectories' entries, which the next run missed (linux, 0.85 GB under a 0.95G cap: 240
    # of 5,312 objects went, and the next build of the same commit missed 238 of its 5,284 compiles)
    used="$(run_ccache --print-stats | awk '$1 == "cache_size_kibibyte" { printf "%.0f\n", $2 * 1024 }')"
    cap="$(awk -v s="$JASPER_CCACHE_CAP" 'BEGIN {
        if (!match(s, /^[0-9.]+/)) exit
        n = substr(s, 1, RLENGTH); u = substr(s, RLENGTH + 1)
        m["k"] = 1e3; m["M"] = 1e6; m["G"] = 1e9; m["T"] = 1e12; m[""] = 1e9
        m["Ki"] = 2^10; m["Mi"] = 2^20; m["Gi"] = 2^30; m["Ti"] = 2^40
        if (u in m) printf "%.0f\n", n * m[u] }')"
    CAP="$JASPER_CCACHE_CAP"
    if [[ -n "$used" && -n "$cap" ]] && (( used <= cap )); then
        echo "ccache-save: the working set is within the cap; nothing cut"
    else
        # the oldest-used entries go first
        run_ccache --cleanup > /dev/null \
            || { echo "ccache-save: the cut to $JASPER_CCACHE_CAP failed; nothing saved"; out false; exit 0; }
    fi
    # zstd level 9 instead of the level 1 a build writes: 14 % smaller (a linux entry's 296 MB: 255 MB,
    # 23 s on 2 CPUs; level 19, 22 % in 277 s), room in the repository's budget for every lane's entry
    # (docs/testing.md "CI"). After the cut: ccache 4.5 gives each rewritten file a new mtime,
    # which would lose the order the cut evicts by. A hit decompresses at the same speed
    run_ccache --recompress "$RECOMPRESS_LEVEL" > /dev/null \
        || { echo "ccache-save: recompressing failed; nothing saved"; out false; exit 0; }
    run_ccache --zero-stats > /dev/null \
        || { echo "ccache-save: zeroing the statistics failed; nothing saved"; out false; exit 0; }
    echo "ccache-save: saving"
    du -sh "$DIR"
fi
out "$save"
