#!/usr/bin/env bash
# je-bench-anim as WebAssembly (S77 R11, docs/perf/animator.md): built in the web image's Emscripten
# for release, run under its node, one thread, once with the pose kernels on -msimd128 and once
# scalar. Results in <out>/simd128.json and <out>/scalar.json; check_bench.py gates them as
# je-bench-anim-wasm (the owner's Web30 and clip memory budgets).
#
#   tools/perf/bench-anim-wasm.sh [<out dir under the repository, default build/test-results/anim-wasm>] [je-bench-anim args...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
OUT="${1:-build/test-results/anim-wasm}"
[[ $# -gt 0 ]] && shift
ARGS="$*"
mkdir -p "$OUT"
for variant in simd128 scalar; do
    [[ "$variant" == simd128 ]] && SIMD=ON || SIMD=OFF
    TREE="build/web-anim-bench-$variant"
    tools/build.sh exec web bash -euo pipefail -c "
        cmake --preset engine-web-release -S engine -B $TREE -DJASPERENGINE_WASM_BENCH=ON -DJASPERENGINE_WASM_SIMD=$SIMD \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        cmake --build $TREE --target je-bench-anim -j${JOBS:-6}
        node --version
        node $TREE/je-bench-anim.js --workers 0 $ARGS --json $OUT/$variant.json
    "
done
