#!/usr/bin/env bash
# Native x86-64 benchmark run: builds, selftest, timings, mutation, memory, binary size,
# renders for correctness, comparison against the references, summary CSVs.
#
#   tools/htmlui-bench/scripts/run_native.sh [phase ...]
#
# Phases (default: all, in this order): build selftest time noavx threads cached patched mutate memory
# size render compare report. Environment: SHARDS (parallel time processes, default 4), CORES
# (CPUs to pin them to, default "8 9 10 11"), PY (python with numpy/pillow, default the venv
# /tmp/htmlui-src-bench/venv). Needs the corpus (fetch_corpus.sh) and, for compare, the
# references (references.py).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$BENCH/../.." && pwd)"
B="$ROOT/build/htmlui-bench"
C="$B/corpus"
RAW="$B/raw"
RENDERS="$B/renders"
REFS="$B/refs"
PY="${PY:-/tmp/htmlui-src-bench/venv/bin/python}"
SHARDS="${SHARDS:-4}"
read -r -a CORES <<< "${CORES:-8 9 10 11}"
JOBS="${JOBS:-3}"
PHASES=("$@")
[ ${#PHASES[@]} -eq 0 ] && PHASES=(build selftest time noavx threads cached patched mutate memory size render compare report)
mkdir -p "$RAW" "$RENDERS"

SIZES_ALL=24,48,128,512,1024
declare -A SET_SIZES=([icons]=$SIZES_ALL [multicolor]=$SIZES_ALL [illustrations]=$SIZES_ALL [huge]=512,1024)
declare -A SET_COLD=([icons]=24,48 [multicolor]=24,48 [illustrations]=24,48 [huge]=512)

want() { local p; for p in "${PHASES[@]}"; do [ "$p" = "$1" ] && return 0; done; return 1; }

# sharded <exe> <out-prefix> <svgbench args...>: SHARDS pinned processes over one list
sharded() {
    local exe="$1" prefix="$2"; shift 2
    local pids=() k
    for ((k = 0; k < SHARDS; k++)); do
        taskset -c "${CORES[$((k % ${#CORES[@]}))]}" "$exe" "$@" --shard "$k/$SHARDS" --out "$prefix-$k.csv" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
}

record_env() {
    {
        echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "cpu: $(lscpu | sed -n 's/^Model name: *//p'), $(nproc) logical CPUs"
        echo "kernel: $(uname -r)"
        echo "compiler: $(g++ --version | head -1)"
        echo "cmake: $(cmake --version | head -1)"
        echo "load before $1: $(cut -d' ' -f1-3 /proc/loadavg)"
    } >> "$RAW/env-native.txt"
}

if want build; then
    cmake -S "$BENCH" -B "$B/native" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$B/native" -j "$JOBS" --target svgbench
    cmake -S "$BENCH" -B "$B/native-noavx" -DCMAKE_BUILD_TYPE=Release -DTHORVG_AVX=OFF -DTHORVG_BENCH_NAME=thorvg-noavx > /dev/null
    cmake --build "$B/native-noavx" -j "$JOBS" --target svgbench
    cmake -S "$BENCH" -B "$B/native-cached" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DTHORVG_BENCH_CACHED_REDRAW \
        -DTHORVG_BENCH_NAME=thorvg-cached > /dev/null
    cmake --build "$B/native-cached" -j "$JOBS" --target svgbench
    # ThorVG with thorvg-skip-unsupported-filters.patch (correctness renders only)
    rm -rf "$B/thorvg-patched"
    cp -r "${THORVG_DIR:-/tmp/htmlui-src/thorvg}" "$B/thorvg-patched"
    rm -rf "$B/thorvg-patched/.git"
    patch -s -d "$B/thorvg-patched" -p1 < "$BENCH/thorvg-skip-unsupported-filters.patch"
    cmake -S "$BENCH" -B "$B/native-patched" -DCMAKE_BUILD_TYPE=Release -DTHORVG_DIR="$B/thorvg-patched" \
        -DTHORVG_BENCH_NAME=thorvg-patched > /dev/null
    cmake --build "$B/native-patched" -j "$JOBS" --target svgbench
    cmake -S "$BENCH" -B "$B/size" -DCMAKE_BUILD_TYPE=Release -DBENCH_SIZE=ON > /dev/null
    cmake --build "$B/size" -j "$JOBS"
fi

if want selftest; then
    "$B/native/svgbench" --selftest --render-dir "$B/selftest" | tee "$RAW/selftest-native.txt"
fi

if want time; then
    record_env time
    for set in icons multicolor illustrations huge; do
        echo "== time $set"
        sharded "$B/native/svgbench" "$RAW/time-$set" --lib luna,thorvg,nanosvg --list "$C/$set.txt" \
            --root "$C/sets/$set" --set "$set" --sizes "${SET_SIZES[$set]}" --cold-sizes "${SET_COLD[$set]}" \
            --mode time --min-time-ms 200 --raster-min-time-ms 100
    done
fi

if want noavx; then
    record_env noavx
    for set in icons multicolor illustrations; do
        echo "== time $set (ThorVG without AVX2)"
        sharded "$B/native-noavx/svgbench" "$RAW/noavx-$set" --lib thorvg --list "$C/$set.txt" \
            --root "$C/sets/$set" --set "$set" --sizes "${SET_SIZES[$set]}" --cold-sizes "${SET_COLD[$set]}" \
            --mode time --min-time-ms 200 --raster-min-time-ms 100
    done
fi

if want threads; then
    record_env threads
    # ThorVG with its task scheduler (4 workers); one process at a time so the workers have
    # free cores (CORES are given to it).
    for set in icons multicolor illustrations huge; do
        echo "== time $set (ThorVG, 4 threads)"
        sizes="${SET_SIZES[$set]}"
        [ "$set" = icons ] && sizes=24,48
        taskset -c "$(IFS=,; echo "${CORES[*]}")" "$B/native/svgbench" --lib thorvg --threads 4 --list "$C/$set.txt" \
            --root "$C/sets/$set" --set "$set" --sizes "$sizes" --cold-sizes "${SET_COLD[$set]}" \
            --mode time --min-time-ms 50 --raster-min-time-ms 50 --out "$RAW/threads-$set-0.csv"
    done
fi

if want cached; then
    record_env cached
    # ThorVG redrawing an unchanged scene (what the forced redraw of the main run avoids).
    for set in icons multicolor; do
        echo "== time $set (ThorVG cached redraw)"
        sharded "$B/native-cached/svgbench" "$RAW/cached-$set" --lib thorvg --list "$C/$set.txt" \
            --root "$C/sets/$set" --set "$set" --sizes 24,48 --cold-sizes "" \
            --mode time --min-time-ms 20 --raster-min-time-ms 50
    done
fi

if want patched; then
    record_env patched
    # LunaSVG vs ThorVG with the filter patch, interleaved in one process (the patched binary
    # links the same LunaSVG): separates ThorVG's raster speed from the cost of the blur
    # filters it executes and LunaSVG ignores.
    for set in multicolor illustrations; do
        echo "== time $set (ThorVG with the filter patch)"
        sharded "$B/native-patched/svgbench" "$RAW/patched-$set" --lib luna,thorvg --list "$C/$set.txt" \
            --root "$C/sets/$set" --set "$set" --sizes "${SET_SIZES[$set]}" --cold-sizes "${SET_COLD[$set]}" \
            --mode time --min-time-ms 200 --raster-min-time-ms 100
    done
fi

if want mutate; then
    record_env mutate
    for set in icons multicolor illustrations; do
        echo "== mutate $set"
        sharded "$B/native/svgbench" "$RAW/mutate-$set" --lib luna,thorvg,nanosvg --list "$C/$set.txt" \
            --root "$C/sets/$set" --set "$set" --sizes 48 --mode mutate --raster-min-time-ms 50 \
            --render-dir "$RENDERS/mutate-$set"
    done
fi

if want memory; then
    for set in icons multicolor illustrations huge; do
        for lib in luna thorvg nanosvg; do
            echo "== memory $set $lib"
            "$B/native/svgbench" --lib "$lib" --list "$C/$set.txt" --root "$C/sets/$set" --set "$set" \
                --sizes 512 --mode memory --out "$RAW/memory-$set-$lib.csv"
        done
    done
fi

if want size; then
    out="$RAW/binary-size-native.csv"
    echo "program,bytes" > "$out"
    for p in size_baseline size_lunasvg size_nanosvg size_thorvg_svg_probe size_thorvg_svgfull_probe size_thorvg_svglottie_probe; do
        "$B/size/$p" "$C/sets/icons/activity.svg" /dev/null
        echo "$p,$(stat -c %s "$B/size/$p")" >> "$out"
    done
    cat "$out"
fi

if want render; then
    tests="$C/src/resvg/crates/resvg/tests"
    for exe in native native-patched; do
        libs=luna,thorvg,nanosvg
        [ "$exe" = native-patched ] && libs=thorvg
        for set in icons multicolor; do
            "$B/$exe/svgbench" --lib "$libs" --list "$C/$set.txt" --root "$C/sets/$set" --set "$set" \
                --sizes 24,48,128 --mode render --render-dir "$RENDERS/$set" --out "$RAW/render-$set-$exe.csv"
        done
        "$B/$exe/svgbench" --lib "$libs" --list "$C/illustrations.txt" --root "$C/sets/illustrations" \
            --set illustrations --sizes 128,512 --mode render --render-dir "$RENDERS/illustrations" \
            --out "$RAW/render-illustrations-$exe.csv"
        "$B/$exe/svgbench" --lib "$libs" --list "$C/conformance.txt" --root "$tests/tests" --set conformance \
            --sizes 300 --mode render --render-dir "$RENDERS/conformance" --font-dir "$tests/fonts" \
            --out "$RAW/render-conformance-$exe.csv"
    done
fi

if want compare; then
    out="$RAW/compare.csv"
    rm -f "$out"
    libs() {
        echo "luna=$RENDERS/$1/luna/$2" "thorvg=$RENDERS/$1/thorvg/$2" "nanosvg=$RENDERS/$1/nanosvg/$2" \
            "thorvg-patched=$RENDERS/$1/thorvg-patched/$2"
    }
    for set in icons multicolor; do
        for size in 24 48 128; do
            # shellcheck disable=SC2046
            "$PY" "$HERE/compare.py" --set "$set" --size "$size" --list "$C/$set.txt" --ref "resvg=$REFS/resvg/$set/$size" \
                --out "$out" $(libs "$set" "$size")
        done
        # Chrome cross-check at 48 px: every library and resvg itself against Chrome
        # shellcheck disable=SC2046
        "$PY" "$HERE/compare.py" --set "$set" --size 48 --list "$C/$set.txt" --ref "chrome=$REFS/chrome/$set/48" \
            --out "$out" $(libs "$set" 48) "resvg=$REFS/resvg/$set/48"
    done
    for size in 128 512; do
        # shellcheck disable=SC2046
        "$PY" "$HERE/compare.py" --set illustrations --size "$size" --list "$C/illustrations.txt" \
            --ref "resvg=$REFS/resvg/illustrations/$size" --out "$out" $(libs illustrations "$size")
    done
    # shellcheck disable=SC2046
    "$PY" "$HERE/compare.py" --set conformance --size 300 --list "$C/conformance.txt" \
        --ref "resvg-suite=$C/src/resvg/crates/resvg/tests/tests" --ref-straight --out "$out" $(libs conformance 300)
fi

if want report; then
    "$PY" "$HERE/report.py" --raw "$RAW" --results "$BENCH/results"
fi
