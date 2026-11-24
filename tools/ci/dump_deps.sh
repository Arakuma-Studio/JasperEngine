#!/usr/bin/env bash
# tools/ci/dump_deps.sh <out-dir>: the unit test trees' dependency databases, for
# `tools/check_changed.py --deps-from <out-dir>` (CI's changes job plans with main's, CI design §8.1):
#   <out-dir>/core-tests.deps    `ninja -t deps` of build/linux/core-tests
#   <out-dir>/editor-tests.deps  of build/linux/editor-tests
#   <out-dir>/htmlui-tests.deps  of build/linux/htmlui-tests-glcore
# Paths in them are the image's (/src/...), which check-changed maps back. Ninja runs in the Linux
# image (another Ninja would delete a tree's log it cannot read); a tree not built is skipped and
# said to be. Exit 1 when a tree that exists gives no database.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: tools/ci/dump_deps.sh <out-dir>" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$1"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
in_image() { [[ -f /.dockerenv ]] && ! command -v docker >/dev/null; }
status=0
for pair in core-tests:core-tests editor-tests:editor-tests htmlui-tests:htmlui-tests-glcore; do
    name="${pair%%:*}"
    tree="build/linux/${pair#*:}"
    if [[ ! -f "$ROOT/$tree/build.ninja" ]]; then
        echo "dump_deps: $tree is not built: no $name.deps"
        rm -f "$OUT/$name.deps"
        continue
    fi
    tmp="$OUT/$name.deps.tmp"
    if in_image; then
        (cd "$ROOT" && ninja -C "$tree" -t deps) > "$tmp" || status=1
    else
        "$ROOT/tools/build.sh" exec linux ninja -C "$tree" -t deps > "$tmp" || status=1
    fi
    if [[ ! -s "$tmp" ]]; then
        echo "dump_deps: ninja -t deps of $tree gave nothing" >&2
        rm -f "$tmp"
        status=1
        continue
    fi
    mv -f "$tmp" "$OUT/$name.deps"
    echo "dump_deps: $OUT/$name.deps ($(grep -c '^[^ ]' "$OUT/$name.deps") objects)"
done
exit $status
