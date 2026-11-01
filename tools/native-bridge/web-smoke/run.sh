#!/bin/bash
# Builds the web native bridge smoke test with emsdk and runs it under node.
#   tools/native-bridge/web-smoke/run.sh   (from the repository root; emsdk at $EMSDK or /opt/emsdk)
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
source "${EMSDK:-/opt/emsdk}/emsdk_env.sh" >/dev/null 2>&1
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
em++ -std=c++17 -O1 -Wall -Wextra \
    -I"$root/engine/core" -I"$root/engine/core/script" -isystem "$root/engine/libs/json" -isystem "$root/engine/libs/lua" \
    -isystem "$root/engine/libs/luabridge3" \
    "$root/engine/platform/emscripten/NativePlatformWeb.cpp" "$root/tools/native-bridge/web-smoke/harness.cpp" \
    -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sEXPORTED_FUNCTIONS=_main,_malloc,_free -o "$out/smoke.js"
node "$out/smoke.js"
