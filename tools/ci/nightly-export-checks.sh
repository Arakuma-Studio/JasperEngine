#!/usr/bin/env bash
# The nightly's two MCP files that compare an exported game with the editor's Game view
# (test_pixel_fonts_export.py, S51 AC22; test_pixel_fx_export.py, S48 AC26). Both read a
# SMOKE_METHOD=compile smoke run made first on the same machine, which an MCP shard does not make,
# so tools/ci/plan_lanes.py leaves them out of the shards (NIGHTLY_LEFT_OUT) and this runs them:
#
#   tools/build.sh linux                     # build/linux/editor-install
#   tools/ci/nightly-export-checks.sh
#
# The smoke runs go to build/smoke (pixel-fonts) and build/pfx-opengl (pixel-fx-game, its three
# stages); the results to build/test-results/mcp.json, gated as the nightly lane by tools/ci/mcp_gate.py.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
[[ -x build/linux/editor-install/bin/jasperengine-editor ]] \
    || { echo "nightly-export-checks: no editor at build/linux/editor-install (tools/build.sh linux)" >&2; exit 2; }

echo "== smoke: tools/fixtures/pixel-fonts (compile)"
SMOKE_METHOD=compile SMOKE_DIR=build/smoke tools/smoke-test.sh tools/fixtures/pixel-fonts
echo "== smoke: tools/fixtures/pixel-fx-game (compile, its three stages)"
SMOKE_METHOD=compile SMOKE_DIR=build/pfx-opengl SMOKE_GAME_SECONDS="ready 10" \
    SMOKE_GAME_RUNS="idle PFX_STAGE=idle; set PFX_STAGE=set; covered PFX_STAGE=covered" \
    tools/smoke-test.sh tools/fixtures/pixel-fx-game

mkdir -p build/test-results
rm -f build/test-results/mcp.json
status=0
# (PFX_SMOKE_DIRS is not one of the variables tools/build.sh forwards: it is set inside)
tools/build.sh exec linux env -u JASPER_FIXTURE PFX_SMOKE_DIRS=build/pfx-opengl dbus-run-session -- \
    tools/mcp/run-tests.sh tools/mcp/tests/test_pixel_fonts_export.py tools/mcp/tests/test_pixel_fx_export.py \
    --json build/test-results/mcp.json || status=$?
echo "nightly-export-checks: run_tests.py exited $status"
python3 tools/ci/mcp_gate.py --json build/test-results/mcp.json --allowed tools/ci/mcp-allowed-skips.txt --lane nightly || exit 1
[[ $status -eq 0 ]] || { echo "nightly-export-checks: the runner failed (exit $status) though the gate passed" >&2; exit 1; }
