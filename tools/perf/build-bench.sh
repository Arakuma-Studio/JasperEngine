#!/usr/bin/env bash
# Build-time measurements (docs/specs/S33-build-speed.md): clean, warm-cache and incremental
# builds of one build tree, run inside its build image so the numbers are those of
# tools/build.sh:
#
#   tools/build.sh exec linux env JOBS=8 tools/perf/build-bench.sh <label> <scenario> [phase...]
#   tools/build.sh exec web env JOBS=8 tools/perf/build-bench.sh <label> web [phase...]
#   tools/build.sh exec android env JOBS=8 tools/perf/build-bench.sh <label> android [phase...]
#
# scenario: editor (jasperengine-editor, linux-release), engine (jasperengine-project,
# engine-linux-release), core-tests (engine-linux-tests), editor-tests (linux-ci), web
# (engine-web-debug), web-threads (engine-web-threads), android (the Gradle debug APK, every ABI
# unless ANDROID_ABIS). EXTRA_CMAKE_ARGS goes to every configure (e.g. -DJASPERENGINE_UNITY_BUILD=ON).
#
# phase (default: cold warm noop edit-cpp edit-header):
#   cold         a new build directory, ccache recaching (CCACHE_RECACHE: every unit compiles,
#                and is stored), i.e. a build no cache can help
#   warm         another new build directory with ccache on: a new worktree or a CI job whose
#                cache holds the same sources
#   noop         Ninja with nothing to do (ninja -d stats)
#   edit-cpp     EDIT_CPP (one source) gets a new mtime and is rebuilt with ccache off: a real edit
#   edit-header  each of EDIT_HEADERS the same way, one after the other
# Every phase but cold runs in the warm tree. Results: build/bench/<label>/results.jsonl (one JSON
# object per phase), the trees' .ninja_log and compile_commands.json next to it;
# tools/perf/build_stats.py reads them. The load average before and after each phase is recorded,
# since a shared host's other jobs skew the wall times.
set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
LABEL="$1"; SCENARIO="$2"; shift 2
PHASES=("$@")
[[ ${#PHASES[@]} -gt 0 ]] || PHASES=(cold warm noop edit-cpp edit-header)
[[ "$LABEL" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "label: letters, digits, . _ - only" >&2; exit 2; }
JOBS="${JOBS:-$(nproc)}"
OUT="build/bench/$LABEL"
RESULTS="$OUT/results.jsonl"
mkdir -p "$OUT"

TREE=""        # -S of the configure (empty: the editor tree)
PRESET=""
TARGET=""
case "$SCENARIO" in
    editor)       PRESET=linux-release;        TARGET=jasperengine-editor ;;
    engine)       PRESET=engine-linux-release; TARGET=jasperengine-project; TREE=engine ;;
    core-tests)   PRESET=engine-linux-tests;   TARGET=jasperengine-core-tests; TREE=engine ;;
    editor-tests) PRESET=linux-ci;             TARGET=jasperengine-editor-tests ;;
    web)          PRESET=engine-web-debug;     TARGET=jasperengine-project; TREE=engine ;;
    web-threads)  PRESET=engine-web-threads;   TARGET=jasperengine-project; TREE=engine ;;
    android) ;;
    *) echo "unknown scenario $SCENARIO" >&2; exit 2 ;;
esac
# what a typical edit touches: a mid-sized engine source, or an editor one in the editor-only targets
case "$SCENARIO" in
    editor|editor-tests)
        EDIT_CPP="${EDIT_CPP:-editor/window/InspectorWindow.cpp}"
        EDIT_HEADERS="${EDIT_HEADERS:-engine/core/Scene.h editor/App.h engine/core/math/Vector3.h}" ;;
    *)
        EDIT_CPP="${EDIT_CPP:-engine/core/subsystem/MeshSystem.cpp}"
        EDIT_HEADERS="${EDIT_HEADERS:-engine/core/Scene.h engine/core/math/Vector3.h}" ;;
esac

now() { date +%s.%N; }
load() { cut -d' ' -f1 /proc/loadavg; }
elapsed() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f", b - a }'; }

# ccache's verdicts of one build, from its log (ccache 4.5 writes one "Result:" line per call)
ccache_counts() {
    local log="$1" hits misses
    [[ -f "$log" ]] || { echo '"ccache_hits": null, "ccache_misses": null'; return; }
    hits=$(grep -c 'Result: .*cache hit' "$log" || true)
    misses=$(grep -c 'Result: cache miss\|Result: .*recache' "$log" || true)
    echo "\"ccache_hits\": $hits, \"ccache_misses\": $misses"
}

record() {  # phase dir seconds load_before load_after steps ccache_json [extra_json]
    printf '{"scenario": "%s", "phase": "%s", "dir": "%s", "seconds": %.1f, "jobs": %s, "load_before": %s, "load_after": %s, "steps": %s, %s%s}\n' \
        "$SCENARIO" "$1" "$2" "$3" "$JOBS" "$4" "$5" "$6" "$7" "${8:+, $8}" | tee -a "$RESULTS"
}

configure() {  # dir
    local src=()
    [[ -n "$TREE" ]] && src=(-S "$TREE")
    # (EXTRA_CMAKE_ARGS unquoted on purpose: a list of -D arguments)
    # shellcheck disable=SC2086
    cmake --preset "$PRESET" "${src[@]}" -B "$1" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache ${EXTRA_CMAKE_ARGS:-} >"$1.configure.log"
}

# one Ninja run of the scenario's target; prints the number of steps it ran
build() {  # dir [ninja args...]
    local dir="$1"; shift
    local log="$dir.ninja.out"
    if ! ninja -C "$dir" -j "$JOBS" "$@" "$TARGET" >"$log" 2>&1; then
        tail -50 "$log" >&2; return 1
    fi
    grep -oE '^\[[0-9]+/[0-9]+\]' "$log" | tail -1 | tr -d '[]' | cut -d/ -f2 || echo 0
}

phase_tree() {  # cold|warm: a new tree, configured and built
    local kind="$1" dir="$OUT/$SCENARIO-$1"
    rm -rf "$dir" "$dir".*
    mkdir -p "$dir"
    local l0 t0 t1 t2 steps
    l0=$(load); t0=$(now)
    configure "$dir"
    t1=$(now)
    rm -f "$dir.ccache.log"
    if [[ "$kind" == cold ]]; then
        steps=$(CCACHE_RECACHE=1 CCACHE_LOGFILE="$dir.ccache.log" build "$dir")
    else
        steps=$(CCACHE_LOGFILE="$dir.ccache.log" build "$dir")
    fi
    t2=$(now)
    record "$kind" "$dir" "$(elapsed "$t0" "$t2")" "$l0" "$(load)" "${steps:-0}" "$(ccache_counts "$dir.ccache.log")" \
        "\"configure_seconds\": $(elapsed "$t0" "$t1")"
    cp "$dir/.ninja_log" "$dir.ninja_log"
    cp "$dir/compile_commands.json" "$dir.compile_commands.json" 2>/dev/null || true
    rm -f "$dir.ccache.log"
    # the cold tree's objects are not needed after its log is kept (disk)
    [[ "$kind" == cold ]] && rm -rf "$dir"
    return 0
}

phase_edit() {  # name file...
    local name="$1"; shift
    local dir="$OUT/$SCENARIO-warm"
    [[ -f "$dir/build.ninja" ]] || { echo "$name needs the warm phase first" >&2; exit 1; }
    for file in "$@"; do
        [[ -f "$file" ]] || { echo "no such file $file" >&2; exit 1; }
        touch "$file"
        local l0 t0 t1 steps
        l0=$(load); t0=$(now)
        steps=$(CCACHE_DISABLE=1 build "$dir")
        t1=$(now)
        record "$name" "$dir" "$(elapsed "$t0" "$t1")" "$l0" "$(load)" "${steps:-0}" '"ccache_hits": null, "ccache_misses": null' \
            "\"file\": \"$file\""
        cp "$dir/.ninja_log" "$dir.$name.$(basename "$file").ninja_log"
    done
}

phase_noop() {
    local dir="$OUT/$SCENARIO-warm"
    [[ -f "$dir/build.ninja" ]] || { echo "noop needs the warm phase first" >&2; exit 1; }
    local l0 t0 t1
    l0=$(load); t0=$(now)
    ninja -C "$dir" -d stats "$TARGET" >"$dir.noop.out" 2>&1
    t1=$(now)
    record noop "$dir" "$(elapsed "$t0" "$t1")" "$l0" "$(load)" 0 '"ccache_hits": null, "ccache_misses": null'
    cat "$dir.noop.out"
}

# Android: Gradle's externalNativeBuild drives one CMake tree per ABI under app/.cxx
android_phase() {
    local kind="$1"
    local app=engine/workspaces/androidstudio/app
    local l0 t0 t1 env_args=(CMAKE_CXX_COMPILER_LAUNCHER=ccache)
    [[ -n "${ANDROID_ABIS:-}" ]] && env_args+=(JASPERENGINE_ANDROID_ABIS="$ANDROID_ABIS")
    case "$kind" in
        cold|warm) rm -rf "$app/.cxx" "$app/build" ;;
        edit-cpp) touch "$EDIT_CPP" ;;
        noop) ;;
        *) echo "android: phases cold warm noop edit-cpp" >&2; exit 2 ;;
    esac
    [[ "$kind" == cold ]] && env_args+=(CCACHE_RECACHE=1)
    [[ "$kind" == edit-cpp ]] && env_args+=(CCACHE_DISABLE=1)
    rm -f "$OUT/android.ccache.log"
    l0=$(load); t0=$(now)
    (cd engine/workspaces/androidstudio && env "${env_args[@]}" CCACHE_LOGFILE="/src/$OUT/android.ccache.log" \
        ./gradlew --no-daemon assembleDebug >"/src/$OUT/android-$kind.gradle.out" 2>&1) \
        || { tail -50 "$OUT/android-$kind.gradle.out" >&2; exit 1; }
    t1=$(now)
    record "$kind" "$app/.cxx" "$(elapsed "$t0" "$t1")" "$l0" "$(load)" 0 "$(ccache_counts "$OUT/android.ccache.log")"
    rm -f "$OUT/android.ccache.log"
    local n=0
    while IFS= read -r -d '' log; do
        n=$((n + 1)); cp "$log" "$OUT/android-$kind.$(basename "$(dirname "$log")").ninja_log"
    done < <(find "$app/.cxx" -name .ninja_log -print0)
}

for phase in "${PHASES[@]}"; do
    echo "== $SCENARIO $phase (jobs $JOBS, load $(load))" >&2
    if [[ "$SCENARIO" == android ]]; then android_phase "$phase"; continue; fi
    case "$phase" in
        cold|warm) phase_tree "$phase" ;;
        noop) phase_noop ;;
        edit-cpp) phase_edit edit-cpp "$EDIT_CPP" ;;
        edit-header) read -r -a headers <<< "$EDIT_HEADERS"; phase_edit edit-header "${headers[@]}" ;;
        *) echo "unknown phase $phase" >&2; exit 2 ;;
    esac
done
