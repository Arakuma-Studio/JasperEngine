#!/bin/bash
# S19 scratch (not committed): editor build+install, editor tests, TOOLS.md, MCP tests
cd "$(dirname "$0")/.."
export JOBS=4
WERROR=1 tools/build.sh linux > build/s19-linux.log 2>&1; echo "LINUX_RC=$?"
grep -E -A5 "error:" build/s19-linux.log | head -60
if [ "${EDITOR_TESTS:-1}" = 1 ]; then
  UNIT_SUITE=editor UNIT_TEST_ARGS="--gtest_filter=Navigation*:NavGridBrush*:StreamComponent*:Settings*:ShortcutDispatcher*:CommandCatalog*" tools/build.sh unit-tests > build/s19-etests.log 2>&1; echo "ETESTS_RC=$?"
  grep -E "^\[  FAILED|PASSED|Failure|error:" build/s19-etests.log | head -40
fi
tools/build.sh exec linux bash -c 'UV="$(command -v uv || echo $HOME/.local/bin/uv)"; xvfb-run -a dbus-run-session -- "$UV" run --quiet --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/gen_tool_docs.py; echo TOOLS_RC=$?; "$UV" run --quiet --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/tool_budget.py --check tools/mcp/budget.json 2>&1 | tail -8; echo BUDGET_RC=$?' > build/s19-tools.log 2>&1
tail -14 build/s19-tools.log
tools/build.sh mcp-tests ${MCP_FILES:-tools/mcp/tests/test_navigation.py tools/mcp/tests/test_ecs.py tools/mcp/tests/test_tool_budget.py tools/mcp/tests/test_docs.py tools/mcp/tests/test_autosync.py} > build/s19-mcp.log 2>&1; echo "MCP_RC=$?"
grep -E "FAIL|PASS|passed|failed|error" build/s19-mcp.log | tail -60
