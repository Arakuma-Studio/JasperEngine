#!/usr/bin/env bash
# The lint lane of .github/workflows/ci.yml: every source lint and every self-test of the repository's
# own tooling, plain python3 and bash on the host (no image, no build). Runs every check, prints a
# failing one's output and `lint: FAILED <command>`, and exits 1 when one failed.
#
#   tools/ci/lint.sh [--actionlint <path>]
#
# --actionlint also lints .github/workflows (CI passes its pinned binary; without it, actionlint on
# PATH is used, else that check is reported as not run). Every `uses:` of .github/ must name a full
# commit SHA either way.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
ACTIONLINT="$(command -v actionlint || true)"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --actionlint) ACTIONLINT="${2:?--actionlint <path>}"; shift 2 ;;
        *) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
FAILED=()

OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT
check() {
    echo "lint: $*"
    local status=0
    "$@" > "$OUT" 2>&1 || status=$?
    [[ $status == 0 ]] && return
    FAILED+=("$*")
    tail -n 200 "$OUT"
    echo "lint: FAILED $*"
}

# every `uses:` outside the repository's own actions names a 40-hex commit (AC11; a tag can move)
pinned_uses() {
    local bad
    bad="$(grep -rnE '^[[:space:]-]*uses:' .github --include='*.yml' --include='*.yaml' \
        | grep -vE 'uses:[[:space:]]*["'\'']?\./' \
        | grep -vE 'uses:[[:space:]]*["'\'']?[^@[:space:]]+@[0-9a-f]{40}["'\'']?([[:space:]]|$)')"
    [[ -z "$bad" ]] && return 0
    echo "not pinned to a full commit SHA:"
    echo "$bad"
    return 1
}

# C++ and editor rules (the linux build runs the first three again inside the image)
check python3 tools/check-cxx-subset.py
check python3 tools/check-pollers.py --self-test
check python3 tools/check-pollers.py
check python3 tools/check_event_clears.py --self-test
check python3 tools/check_event_clears.py
check python3 tools/check_text_encoding.py --self-test
check python3 tools/check_text_encoding.py
check cmake -P tools/tests/check_toolchain_floor.cmake
check cmake -P tools/tests/cxx-standard-check/run.cmake
# data safety (docs/data-safety.md)
check python3 tools/check_raw_writes.py --self-test
check python3 tools/check_raw_writes.py
check python3 tools/check_stream_keys.py
# editor keys live in the command catalog
check python3 tools/ci/check-hardcoded-shortcuts.py --self-test
check python3 tools/ci/check-hardcoded-shortcuts.py
# contributor docs, notices, the MCP CI suite's classification, the Xcode project's sources
check python3 tools/check_agent_docs.py
check python3 tools/ci/check-notices.py
check python3 tools/ci/check-mcp-ci-suite.py
for sync in tools/xcode_sync_*.py; do
    check python3 "$sync" --check
done

# the tooling's own tests (python3 -I, as tools/check-changed runs them)
check python3 -I tools/test_check_changed.py
check python3 -I tools/test_check_fanout.py
check python3 -I tools/test_xcode_sync_sources.py
check python3 -I tools/mcp/test_shard.py
check python3 -I tools/mcp/test_build_cache.py
check python3 -I tools/mcp/eval/test_grade.py
check python3 -I tools/players/test_write_hashes.py
for test in tools/ci/test_*.py; do
    case "$test" in
        # needs a built editor and the runtime image: the smoke lane's business
        tools/ci/test_player_cli.py) continue ;;
        # scans the shader library, which main fails today (the Title Art shaders): tools/check-changed
        # runs it for a shader change
        tools/ci/test_check_shader_lod.py) continue ;;
    esac
    check python3 -I "$test"
done
for test in tools/ci/test_*.sh; do
    check bash "$test"
done

# the workflows
check pinned_uses
if [[ -n "$ACTIONLINT" ]]; then
    check "$ACTIONLINT" -no-color
else
    echo "lint: NOT RUN actionlint (not on PATH; pass --actionlint <path>)"
fi

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "lint: ${#FAILED[@]} check(s) failed:"
    printf '  %s\n' "${FAILED[@]}"
    exit 1
fi
echo "lint: every check passed"
