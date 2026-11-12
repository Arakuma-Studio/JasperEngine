#!/usr/bin/env bash
# tools/ci/mcp_shard.sh <shard-id> <plan-dir>: one CI MCP shard (contract C2; CI design §8.2, §9).
#
# Runs <plan-dir>/mcp-plan/plan-<shard-id>.json (tools/ci/plan_lanes.py, tools/mcp/shard.py plan) in
# the Linux image against build/linux/editor-install, -j 2, without the uncommitted default fixture
# (the CI suite needs none: run_tests.py's ci_files guard), with the MCP suite's slow opt-ins on (the
# real player builds), then gates the results with tools/ci/mcp_gate.py. Writes
# build/test-results/mcp.json and build/test-results/mcp-test-seconds.json.
#
# tools/build.sh mcp-tests cannot run a plan without the fixture: MCP_SUITE=ci passes --suite ci,
# which run_tests.py refuses with --plan, and MCP_SUITE=all requires the fixture. So this runs the
# same command in the image through `tools/build.sh exec linux` (the same mounts and forwarded
# variables, no fixture mount).
#
# MCP_WEB_BUILD=1 (the nightly): when the shard holds test_mesh_raycast.py, build its web project
# first (WEB_PROJECT=mesh-raycast tools/build.sh web) and gate as the lane "nightly", where the
# allow list's "runs in nightly" skips must run. JASPER_CI_LANE names the lane otherwise (default mcp).
# Exit 1 when the shard fails its gate (or has no plan), 2 on usage.
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: tools/ci/mcp_shard.sh <shard-id> <plan-dir>" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
SHARD="$1"
[[ "$SHARD" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "mcp_shard: a shard id is [A-Za-z0-9_-]+, not '$SHARD'" >&2; exit 2; }
PLAN_SRC="$2/mcp-plan/plan-$SHARD.json"
if [[ ! -f "$PLAN_SRC" ]]; then
    echo "mcp_shard: no plan for shard $SHARD at $PLAN_SRC: the changes job could not plan the MCP shards" \
         "(see the fallbacks in $2/lanes.json)" >&2
    exit 1
fi
[[ -x build/linux/editor-install/bin/jasperengine-editor ]] \
    || { echo "mcp_shard: no editor at build/linux/editor-install (tools/build.sh linux, or the editor-install artifact)" >&2; exit 1; }
# (the image sees the checkout at /src: the plan goes inside it)
mkdir -p build/ci-shard build/test-results
PLAN="build/ci-shard/plan-$SHARD.json"
cp "$PLAN_SRC" "$PLAN"
LANE="${JASPER_CI_LANE:-mcp}"
if [[ "${MCP_WEB_BUILD:-0}" == 1 ]]; then
    LANE=nightly
    if grep -q '"file": "test_mesh_raycast.py"' "$PLAN"; then
        env -u JASPER_DOCKER_IMAGE WEB_PROJECT=mesh-raycast tools/build.sh web
    fi
fi
rm -f build/test-results/mcp.json
status=0
JASPER_SLOW_BUILD=1 JASPER_SLOW_TESTS=1 tools/build.sh exec linux env -u JASPER_FIXTURE \
    dbus-run-session -- tools/mcp/run-tests.sh --plan "$PLAN" -j 2 --json build/test-results/mcp.json || status=$?
echo "mcp_shard: run_tests.py exited $status"
python3 tools/ci/mcp_gate.py --json build/test-results/mcp.json --allowed tools/ci/mcp-allowed-skips.txt \
    --plan "$PLAN" --lane "$LANE" || exit 1
# (a failed test or a crash makes the gate fail above; any other exit of the runner is a failure too)
[[ $status -eq 0 ]] || { echo "mcp_shard: the runner failed (exit $status) though the gate passed" >&2; exit 1; }
