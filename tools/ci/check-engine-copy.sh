#!/usr/bin/env bash
# The editor build's engine copies follow engine/ (BF5): a header added to engine/ appears in
# build/linux/editor/engine and in build/linux/editor-install/bin/engine after tools/build.sh
# linux, and is gone from both after it is deleted and the editor rebuilt. Two incremental
# editor builds (SKIP_ENGINE=1).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
PROBE=core/BF5EngineCopyProbe.h
COPY=build/linux/editor/engine/$PROBE
INSTALLED=build/linux/editor-install/bin/engine/$PROBE
trap 'rm -f "engine/$PROBE"' EXIT
build() { env SKIP_ENGINE=1 tools/build.sh linux > build/check-engine-copy.log 2>&1 || { tail -n 30 build/check-engine-copy.log; exit 1; }; }

echo "// probe of tools/ci/check-engine-copy.sh; included by nothing" > "engine/$PROBE"
build
[[ -f $COPY && -f $INSTALLED ]] || { echo "FAIL: the new header is not in both copies"; exit 1; }
echo "ok   a new header is copied and installed"
rm "engine/$PROBE"
build
fails=0
[[ ! -e $COPY ]] && echo "ok   a deleted header leaves the build tree's copy" || { echo "FAIL: $COPY is still there"; fails=1; }
[[ ! -e $INSTALLED ]] && echo "ok   a deleted header leaves the install" || { echo "FAIL: $INSTALLED is still there"; fails=1; }
exit $fails
