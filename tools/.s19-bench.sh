#!/bin/bash
# S19 scratch: nav tests then je-bench-nav N times (not committed)
set -o pipefail
cd "$(dirname "$0")/.."
cmake --build build/linux/core-tests --target jasperengine-core-tests je-bench-nav -- -k 0 -j ${JOBS:-4} 2>&1 | grep -E "error|FAILED" | head -40
if [ "${SKIP_TESTS:-0}" != 1 ]; then
  build/linux/core-tests/tests/core/jasperengine-core-tests --gtest_filter="Nav*:*NavGrid*" 2>&1 | grep -E "^\[  FAILED|PASSED|Failure|Expected|actual|Which is" | head -60
fi
for i in $(seq 1 ${RUNS:-1}); do
  build/linux/core-tests/tests/core/je-bench-nav --count-allocs --json /tmp/nav-$i.json > /tmp/nav-$i.txt 2>&1
done
python3 - <<'PY'
import json, statistics, glob
runs=[json.load(open(f))['results'] for f in sorted(glob.glob('/tmp/nav-*.json'))]
keys=runs[0].keys()
med={k: statistics.median([r[k] for r in runs if k in r]) for k in keys}
json.dump({"bench":"je-bench-nav","results":med}, open('/tmp/nav-median.json','w'))
print("runs", len(runs))
PY
python3 tools/perf/check_bench.py /tmp/nav-median.json
