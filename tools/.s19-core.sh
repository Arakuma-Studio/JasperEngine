#!/bin/bash
# S19 scratch: build the core tests + nav bench and run the navigation tests (not committed)
set -o pipefail
cd "$(dirname "$0")/.."
cmake --build build/linux/core-tests --target jasperengine-core-tests je-bench-nav -- -k 0 -j 4 || exit 2
B=build/linux/core-tests/tests/core/jasperengine-core-tests
$B --gtest_filter="${FILTER:-Nav*:*NavGrid*:Scheduler*:Ecs*}" 2>&1 | tail -n ${TAILN:-200}
