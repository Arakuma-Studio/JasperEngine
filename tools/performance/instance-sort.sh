#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
"${CXX:-c++}" -std=c++17 -O3 -DNDEBUG -ffunction-sections -fdata-sections \
    -I"$repo/engine/core/script" -I"$repo/engine/libs/lua" -I"$repo/engine/libs/luabridge3" \
    -I"$repo/engine/core" -I"$repo/engine/renders" -I"$repo/engine/libs/sokol" \
    "$repo/tools/performance/instance-sort.cpp" \
    "$repo/engine/core/math/Matrix4.cpp" "$repo/engine/core/math/Vector3.cpp" \
    "$repo/engine/core/math/Vector4.cpp" "$repo/engine/core/math/Rect.cpp" \
    -Wl,--gc-sections -o "$output/instance-sort"
"$output/instance-sort"
