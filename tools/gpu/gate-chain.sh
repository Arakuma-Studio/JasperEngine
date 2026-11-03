#!/usr/bin/env bash
# The GPU campaign's landing gate on a normal build server (not the GPU host): GL WERROR build,
# the touched unit suites, ONE full MCP run (MCP_SUITE=all, slow builds) and the smoke test.
# Run detached on the server from the worktree; each step logs to build/gate/<step>.log.
set -uo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/gate
step() { local name="$1"; shift; echo "[gate] $name: $*"; "$@" > "build/gate/$name.log" 2>&1; echo "[gate] $name: exit $?"; }
step gl-werror env WERROR=1 tools/build.sh linux
step units-touched env UNIT_TEST_ARGS='--gtest_filter=AtmosphereGpu.*:ShaderZeroCost.*:*Shadow*:AsyncCreate*:ShaderCreationReflect*' tools/build.sh unit-tests
step mcp-all env MCP_SUITE=all JASPER_SLOW_BUILD=1 tools/build.sh mcp-tests -j 2
step smoke tools/smoke-test.sh
echo "[gate] done"
