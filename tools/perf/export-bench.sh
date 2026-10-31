#!/usr/bin/env bash
# The C++-script game builds of docs/specs/S33-build-speed.md (AC1, AC5): S30's Compile method on
# tools/fixtures/prefab-runtime (C++ scripts), as a user without ccache runs it, inside the Linux
# build image after tools/build.sh linux:
#
#   tools/build.sh exec linux env JOBS=8 tools/perf/export-bench.sh <label>
#
# Three builds, each into a new output directory, with an empty user cache to start from:
#   first     the project's first build
#   other     a second project (the same fixture under another name) with the same kit
#   settings  the first project again after a Player setting change (its window title)
# Results: build/bench/<label>/results.jsonl (scenario "export"), the logs next to it.
set -euo pipefail

[[ $# -eq 1 && "$1" =~ ^[A-Za-z0-9_.-]+$ ]] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
LABEL="$1"
OUT="build/bench/$LABEL/export"
RESULTS="build/bench/$LABEL/results.jsonl"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
[[ -x "$EDITOR" ]] || { echo "no editor at $EDITOR: run tools/build.sh linux first" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/cache"
# a user's machine: no compiler cache, nothing built before
export CCACHE_DISABLE=1 XDG_CACHE_HOME="$PWD/$OUT/cache"
cp -r tools/fixtures/prefab-runtime "$OUT/project-a"
cp -r tools/fixtures/prefab-runtime "$OUT/project-b"
sed -i 's/^name: .*/name: PrefabRuntimeB/' "$OUT/project-b/project.yaml"

build() {  # name project
    local name="$1" project="$2" l0 t0 t1 units
    l0=$(cut -d' ' -f1 /proc/loadavg)
    t0=$(date +%s.%N)
    if ! dbus-run-session -- "$EDITOR" build --project "$project" --platform linux --out "$OUT/out-$name" \
            --method compile >"$OUT/$name.log" 2>&1; then
        tail -40 "$OUT/$name.log" >&2; exit 1
    fi
    t1=$(date +%s.%N)
    # the units the build compiled (Ninja's lines in the build's output)
    units=$(grep -cE 'Building (C|CXX|ASM_NASM) object' "$OUT/$name.log" || true)
    printf '{"scenario": "export", "phase": "%s", "dir": "%s", "seconds": %.1f, "jobs": %s, "load_before": %s, "load_after": %s, "steps": %s, "ccache_hits": null, "ccache_misses": null}\n' \
        "$name" "$OUT/out-$name" "$(awk -v a="$t0" -v b="$t1" 'BEGIN { print b - a }')" "${JOBS:-0}" "$l0" \
        "$(cut -d' ' -f1 /proc/loadavg)" "$units" | tee -a "$RESULTS"
}

build first "$OUT/project-a"
build other "$OUT/project-b"
if grep -q '^windowTitle:' "$OUT/project-a/project.yaml"; then
    sed -i 's/^windowTitle: .*/windowTitle: Prefab Runtime Changed/' "$OUT/project-a/project.yaml"
else
    echo 'windowTitle: Prefab Runtime Changed' >> "$OUT/project-a/project.yaml"
fi
build settings "$OUT/project-a"
