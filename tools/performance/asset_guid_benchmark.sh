#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${ASSET_GUID_BENCH_OUT:-$ROOT/build/performance/asset-guid}"
SOURCE="${1:-$ROOT/editor/util/AssetDatabase.cpp}"
mkdir -p "$OUT"
"${CXX:-c++}" -std=c++17 -O2 -DNDEBUG -ffunction-sections -fdata-sections \
    -I"$ROOT/editor" -I"$ROOT/editor/util" -I"$ROOT/engine/core" -I"$ROOT/engine/core/script" \
    -I"$ROOT/engine/libs/lua" -I"$ROOT/engine/libs/luabridge3" -I"$ROOT/engine/renders" \
    -I"$ROOT/libs/imgui" -I"$ROOT/libs/yaml-cpp/include" -I"$ROOT/engine/libs/sokol" \
    "$SOURCE" "$ROOT/tools/performance/asset_guid_benchmark.cpp" \
    -Wl,--gc-sections -o "$OUT/asset-guid-benchmark"
"$OUT/asset-guid-benchmark" "$OUT/fixtures"
