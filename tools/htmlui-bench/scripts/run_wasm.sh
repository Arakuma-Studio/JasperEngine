#!/usr/bin/env bash
# WebAssembly benchmark run: builds svgbench and the size probes with emscripten (inside the
# jasperengine/web-build image unless em++ is on PATH), runs the selftest, the timings on the
# icons and illustrations sets with node, a render of the icons at 48 px (checked against the
# native renders) and the .wasm size probes.
#
#   tools/htmlui-bench/scripts/run_wasm.sh [phase ...]     phases: build selftest time render size
#
# Environment: SHARDS (parallel node processes, default 4), CORES (default "8 9 10 11"),
# IMAGE (default jasperengine/web-build). ThorVG is built without threads and without AVX.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$BENCH/../.." && pwd)"
B="$ROOT/build/htmlui-bench"
C="$B/corpus"
RAW="$B/raw"
IMAGE="${IMAGE:-jasperengine/web-build}"
SHARDS="${SHARDS:-4}"
CORES="${CORES:-8 9 10 11}"
LUNASVG_DIR="${LUNASVG_DIR:-/tmp/htmlui-src/lunasvg}"
THORVG_DIR="${THORVG_DIR:-/tmp/htmlui-src/thorvg}"
PHASES="${*:-build selftest time render size}"
mkdir -p "$RAW"

if ! command -v em++ > /dev/null && [ -z "${IN_WASM_CONTAINER:-}" ]; then
    # Re-run this script inside the web image, same paths, then give the files back to the user.
    exec docker run --rm -e IN_WASM_CONTAINER=1 -e SHARDS="$SHARDS" -e CORES="$CORES" \
        -e LUNASVG_DIR="$LUNASVG_DIR" -e THORVG_DIR="$THORVG_DIR" \
        -v "$ROOT:$ROOT" -v "$LUNASVG_DIR:$LUNASVG_DIR:ro" -v "$THORVG_DIR:$THORVG_DIR:ro" \
        -w "$ROOT" "$IMAGE" bash -c "'$HERE/run_wasm.sh' $PHASES; rc=\$?; chown -R $(id -u):$(id -g) '$B'; exit \$rc"
fi

want() { case " $PHASES " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
COMMON=(-DCMAKE_BUILD_TYPE=Release -DTHORVG_THREADS=OFF -DTHORVG_AVX=OFF -DLUNASVG_DIR="$LUNASVG_DIR" -DTHORVG_DIR="$THORVG_DIR")
SVGBENCH=(node "$B/wasm/svgbench.js")

if want build; then
    emcmake cmake -S "$BENCH" -B "$B/wasm" "${COMMON[@]}" > /dev/null
    cmake --build "$B/wasm" -j 3 --target svgbench
    emcmake cmake -S "$BENCH" -B "$B/wasm-size" "${COMMON[@]}" -DBENCH_SIZE=ON > /dev/null
    cmake --build "$B/wasm-size" -j 3
fi

if want selftest; then
    "${SVGBENCH[@]}" --selftest --render-dir "$B/selftest-wasm" | tee "$RAW/selftest-wasm.txt"
fi

if want time; then
    {
        echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "emscripten: $(emcc --version | head -1)"
        echo "node: $(node --version)"
        echo "load before time: $(cut -d' ' -f1-3 /proc/loadavg)"
    } >> "$RAW/env-wasm.txt"
    read -r -a cores <<< "$CORES"
    for set in icons illustrations; do
        echo "== wasm time $set"
        pids=()
        for ((k = 0; k < SHARDS; k++)); do
            taskset -c "${cores[$((k % ${#cores[@]}))]}" "${SVGBENCH[@]}" --lib luna,thorvg,nanosvg --list "$C/$set.txt" \
                --root "$C/sets/$set" --set "$set" --sizes 24,48,128,512,1024 --cold-sizes 24,48 --mode time \
                --min-time-ms 200 --raster-min-time-ms 100 --shard "$k/$SHARDS" --out "$RAW/wasm-time-$set-$k.csv" &
            pids+=($!)
        done
        for p in "${pids[@]}"; do wait "$p"; done
    done
fi

if want render; then
    "${SVGBENCH[@]}" --lib luna,thorvg,nanosvg --list "$C/icons.txt" --root "$C/sets/icons" --set icons --sizes 48 \
        --mode render --render-dir "$B/renders-wasm/icons" --out "$RAW/wasm-render-icons.csv"
fi

if want size; then
    out="$RAW/binary-size-wasm.csv"
    echo "program,bytes" > "$out"
    for p in size_baseline size_lunasvg size_nanosvg size_thorvg_svg_probe size_thorvg_svgfull_probe size_thorvg_svglottie_probe; do
        node "$B/wasm-size/$p.js" "$C/sets/icons/activity.svg" /dev/null
        echo "$p,$(stat -c %s "$B/wasm-size/$p.wasm")" >> "$out"
    done
    cat "$out"
fi
