#!/usr/bin/env bash
# Downloads the SVG benchmark corpus at pinned commits into build/htmlui-bench/corpus
# (git-ignored) and builds the per-set file lists used by svgbench and the scripts.
#
#   tools/htmlui-bench/scripts/fetch_corpus.sh [CORPUS_DIR]
#
# Sets (see docs/html-ui/svg-benchmark.md):
#   icons          Lucide, every icon (ISC)
#   multicolor     microsoft/fluentui-emoji "Color" SVGs, 300 chosen with seed 20261007 (MIT)
#   illustrations  thorvg/thorvg.example res/svg (MIT)
#   huge           Wikimedia BlankMap-World.svg + thorvg.example files > 300 KB
#   conformance    resvg test suite crates/resvg/tests (MIT/Apache-2.0), with reference PNGs
# Every set is preprocessed: currentColor -> #000000 (sets/<set>/...).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
CORPUS="${1:-$ROOT/build/htmlui-bench/corpus}"
SRC="$CORPUS/src"
mkdir -p "$SRC"

LUCIDE_COMMIT=1fae58d0a9c661a338036838caf3399b5507bab6
FLUENT_COMMIT=1ffb34c752ecf5d402f04cfb4b392c77f57c54bc
THORVG_EXAMPLE_COMMIT=5c61ffd87cd2b1b64a7f6193aa12c17fb78d9d5a
RESVG_COMMIT=1fe3cfe88bc692612f1ccd4403ba3476b2cdd813
WORLD_URL="https://upload.wikimedia.org/wikipedia/commons/4/4d/BlankMap-World.svg"
WORLD_SHA1=45bf62cb455555028cf0a7593ceb7beef08a6298

# sparse_clone <url> <dir> <commit> <pattern...>: blobless, sparse (non-cone) checkout of one commit
sparse_clone() {
    local url="$1" dir="$2" commit="$3"; shift 3
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
        echo "  $dir already at $commit"; return
    fi
    rm -rf "$dir"
    git clone -q --filter=blob:none --no-checkout "$url" "$dir"
    git -C "$dir" sparse-checkout set --no-cone "$@"
    git -C "$dir" checkout -q "$commit"
}

echo "== lucide"
if [ -d /tmp/htmlui-src/lucide/.git ] && [ "$(git -C /tmp/htmlui-src/lucide rev-parse HEAD)" = "$LUCIDE_COMMIT" ]; then
    rm -rf "$SRC/lucide"; mkdir -p "$SRC/lucide"
    cp -r /tmp/htmlui-src/lucide/icons "$SRC/lucide/icons"
    echo "  copied from /tmp/htmlui-src/lucide"
else
    sparse_clone https://github.com/lucide-icons/lucide "$SRC/lucide" "$LUCIDE_COMMIT" '/icons/*.svg'
fi

echo "== fluentui-emoji"
sparse_clone https://github.com/microsoft/fluentui-emoji "$SRC/fluentui-emoji" "$FLUENT_COMMIT" '/assets/*/Color/*.svg'

echo "== thorvg.example"
sparse_clone https://github.com/thorvg/thorvg.example "$SRC/thorvg.example" "$THORVG_EXAMPLE_COMMIT" '/res/svg/*'

echo "== resvg tests"
sparse_clone https://github.com/linebender/resvg "$SRC/resvg" "$RESVG_COMMIT" '/crates/resvg/tests/tests/' '/crates/resvg/tests/fonts/' '/crates/resvg/tests/integration/'

echo "== wikimedia BlankMap-World.svg"
mkdir -p "$SRC/wikimedia"
if [ ! -f "$SRC/wikimedia/BlankMap-World.svg" ]; then
    curl -sSfL -A "JasperEngine-htmlui-bench/1.0 (benchmark corpus)" -o "$SRC/wikimedia/BlankMap-World.svg" "$WORLD_URL"
fi
got=$(sha1sum "$SRC/wikimedia/BlankMap-World.svg" | cut -d' ' -f1)
if [ "$got" != "$WORLD_SHA1" ]; then
    echo "  WARNING: BlankMap-World.svg sha1 $got != pinned $WORLD_SHA1 (Wikimedia updated the file)"
fi

echo "== preparing sets"
python3 "$HERE/prepare_corpus.py" "$CORPUS"
