#!/usr/bin/env bash
# Builds the standalone engine (cmake -S engine) with Recast/Detour switched off
# (JASPERENGINE_NAVIGATION=OFF, what an export with Project Settings > Navigation > Include In
# Builds off compiles) and its core tests, then runs the suites that build has:
# NavStubTest (compiled only when navigation is OFF: every navmesh call fails and logs once) and
# NavGridTest (grids keep working). docs/specs/S74-navigation-fixes.md R28; docs/testing.md.
#
#   tools/check-navigation-off.sh [-j N]        inside the Linux image:
#   tools/build.sh exec linux tools/check-navigation-off.sh
#
# Build dir: build/navigation-off (kept: a second run is incremental); its log next to it, the
# test results in build/test-results/navigation-off.xml. Exit 0 when the tree links, every
# selected test passes and at least MIN_STUB_TESTS NavStubTest tests ran: a build that compiled
# NavStubTest out (navigation still on) is a failure, not a pass.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-4}"
if [ "${1:-}" = "-j" ]; then JOBS="$2"; shift 2; fi
MIN_STUB_TESTS=2
FILTER='NavStubTest.*:NavGridTest.*'
LAUNCHER=()
command -v ccache > /dev/null && LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)

cd "$ROOT" || exit 2
dir="$ROOT/build/navigation-off"
log="$dir.log"
xml="$ROOT/build/test-results/navigation-off.xml"
mkdir -p "$ROOT/build/test-results"

if ! cmake --preset engine-linux-tests -S "$ROOT/engine" -B "$dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DJASPERENGINE_TESTS=ON -DJASPERENGINE_NAVIGATION=OFF -DJASPERENGINE_WERROR=OFF "${LAUNCHER[@]}" > "$log" 2>&1; then
    echo "FAIL configure (see $log)"; tail -20 "$log"; exit 1
fi
if ! grep -q "JASPERENGINE_NAVIGATION:BOOL=OFF" "$dir/CMakeCache.txt"; then
    echo "FAIL the tree is not configured with JASPERENGINE_NAVIGATION=OFF ($dir/CMakeCache.txt)"; exit 1
fi
if ! cmake --build "$dir" --target jasperengine-core-tests -j "$JOBS" -- -k 0 >> "$log" 2>&1; then
    echo "FAIL build (see $log):"
    grep -E "error:|^FAILED" "$log" | sort -u | head -40
    exit 1
fi
echo "built (warnings: $(grep -c "warning:" "$log" || true))"

rm -f "$xml"
status=0
set -f  # the filter is not a glob
"$ROOT/tools/with-xvfb.sh" "$dir/tests/core/jasperengine-core-tests" --gtest_filter="$FILTER" --gtest_output="xml:$xml" || status=1
set +f
python3 "$ROOT/tools/ci/gtest-gate.py" --xml "$xml" --min-tests "$MIN_STUB_TESTS" --max-skipped 0 || status=1
stub=$(python3 -I - "$xml" <<'EOF'
import sys
import xml.etree.ElementTree as ET
try:
    cases = ET.parse(sys.argv[1]).getroot().iter("testcase")
except (OSError, ET.ParseError):
    print(0)
    sys.exit(0)
print(sum(1 for c in cases if c.get("classname") == "NavStubTest" and c.get("result", "completed") == "completed"
          and c.find("failure") is None and c.find("skipped") is None))
EOF
)
if [ "${stub:-0}" -lt "$MIN_STUB_TESTS" ]; then
    echo "FAIL $stub NavStubTest test(s) passed, at least $MIN_STUB_TESTS expected (is navigation really off?)"
    status=1
else
    echo "PASS navigation OFF: $stub NavStubTest test(s) passed ($FILTER)"
fi
exit $status
