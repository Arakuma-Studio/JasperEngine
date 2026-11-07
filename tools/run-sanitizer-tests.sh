#!/usr/bin/env bash
# Builds jasperengine-editor-tests with AddressSanitizer + UndefinedBehaviorSanitizer and runs it
# under a virtual X server, from a worktree root.
#
#   tools/run-sanitizer-tests.sh [gtest args...]      e.g. --gtest_filter='Physics2D*'
#
# The build goes to build/asan: RelWithDebInfo, ccache, -fsanitize=address,undefined
# -fno-omit-frame-pointer -fno-sanitize-recover=undefined (undefined behaviour aborts like an ASan
# error). The CMake has no sanitizer option of its own: these are the standard CMAKE_*_FLAGS, so
# every target, the vendored libraries included, is instrumented.
#
# By default every test suite runs in its own process, SAN_JOBS at a time, so one report (which
# ends its process: halt_on_error) does not hide the suites after it. Logs: build/asan/san-logs/
# <Suite>.log; the failed suites and the first line of each report are listed at the end.
# With gtest arguments (a --gtest_filter, --gtest_repeat) it runs them in one process instead.
#
# SAN_JOBS       parallel build jobs and test processes (default 5: a 16-core box; 3 on 8 cores)
# SAN_SERIAL=1   one process for the whole suite instead (stops at the first report)
# SAN_LEAKS=1    LeakSanitizer on (off by default), with tools/sanitizers/lsan.supp: third-party and
#                driver leaks only (engine leaks are fixed, not suppressed)
# SAN_BUILD      the build directory (default build/asan); SAN_NO_BUILD=1 skips the build
#
# The EXPECT_EXIT tests (SpriteNormals*DeathTest, TextureSynthesis*) run their bodies in a forked
# child that must exit 0: a report in the child makes it exit 1, so it fails the test too (the
# report is in the test's log, between the child's output lines).
set -euo pipefail

if [ "${1:-}" = --suite ]; then
    log=$SAN_LOGS/${2//\//_}.log
    if "$SAN_TESTS" --gtest_filter="$2.*" >"$log" 2>&1; then echo "PASS $2"; else echo "FAIL $2"; fi
    exit 0
fi

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
build=${SAN_BUILD:-build/asan}
jobs=${SAN_JOBS:-5}
san='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined'

if [ "${SAN_NO_BUILD:-0}" != 1 ]; then
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJASPERENGINE_TESTS=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_C_FLAGS="$san" -DCMAKE_CXX_FLAGS="$san" \
        -DCMAKE_EXE_LINKER_FLAGS="$san" -DCMAKE_SHARED_LINKER_FLAGS="$san" -DCMAKE_MODULE_LINKER_FLAGS="$san" >/dev/null
    cmake --build "$build" --target jasperengine-editor-tests -j"$jobs"
fi

tests=$root/$build/jasperengine-editor-tests
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
if [ "${SAN_LEAKS:-0}" = 1 ]; then
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
    export LSAN_OPTIONS=suppressions=$root/tools/sanitizers/lsan.supp:print_suppressions=0
else
    export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
fi

if [ "${SAN_SERIAL:-0}" = 1 ] || [ $# -gt 0 ]; then
    exec tools/with-xvfb.sh "$tests" "$@"
fi

logs=$root/$build/san-logs
rm -rf "$logs"
mkdir -p "$logs"
# one process per suite (this script again, with --suite: exported functions may not reach xargs)
export SAN_TESTS=$tests SAN_LOGS=$logs
ASAN_OPTIONS=detect_leaks=0 "$tests" --gtest_list_tests 2>/dev/null | grep -E '^[A-Za-z_][A-Za-z0-9_/]*\.$' | tr -d . > "$logs/suites.txt"
# one X server for every process
tools/with-xvfb.sh \
    xargs -P "$jobs" -I{} "$root/tools/run-sanitizer-tests.sh" --suite {} < "$logs/suites.txt" > "$logs/summary.txt"
echo "$(grep -c '^PASS' "$logs/summary.txt") of $(wc -l < "$logs/suites.txt") suites passed"

status=0
for suite in $(grep '^FAIL ' "$logs/summary.txt" | cut -d' ' -f2); do
    status=1
    log=$logs/${suite//\//_}.log
    report=$(grep -m1 -E 'ERROR: (Address|Leak)Sanitizer|runtime error:|\[  FAILED  \]' "$log" || true)
    echo "FAIL $suite: ${report:-see $log}"
done
exit $status
