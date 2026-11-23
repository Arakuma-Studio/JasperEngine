#!/usr/bin/env bash
# The editor install CI hands from the build job to the jobs that run it (ci.yml's linux to mcp and
# smoke, the nightly's MCP editor to its shards), without the folders the install copies unchanged
# from the checkout: those jobs check out the same commit, and the Title Art fonts, HDRIs and
# thumbnails and the sound library are half the archive and do not compress.
#
#   tools/ci/editor_install.sh pack <install-dir> <archive.tar.zst>
#   tools/ci/editor_install.sh unpack <archive.tar.zst> <dir>      (makes <dir>/<install-dir's name>)
#
# pack leaves a folder out only when it equals its checkout folder (contents and executable bits,
# the library's .gitattributes aside, which the install drops), and records it in the archive's
# .from-checkout; unpack copies those back from the checkout. SOURCE_ROOT: the checkout (default:
# this repository).
set -euo pipefail

ROOT="${SOURCE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
LIST=.from-checkout

# <path in the install> <path in the checkout>: the install rules of CMakeLists.txt that copy a
# checkout folder as it is
COPIES=(
    "bin/titleart/hdri editor/resources/titleart/hdri"
    "bin/titleart/fonts editor/resources/titleart/fonts"
    "bin/titleart/thumbnails editor/resources/titleart/thumbnails"
    "bin/sound-library editor/resources/sound-library/library"
)

usage() { sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }

executables() { (cd "$1" && find . -type f -perm -u+x ! -name .gitattributes | LC_ALL=C sort); }

same() {
    diff -rq --exclude=.gitattributes "$1" "$2" > /dev/null 2>&1 \
        && [[ "$(executables "$1")" == "$(executables "$2")" ]]
}

pack() {
    local install="${1%/}" archive="$2" name parent dest src
    [[ -d "$install" ]] || { echo "editor_install: no install at $install" >&2; exit 1; }
    name="$(basename "$install")"
    parent="$(dirname "$install")"
    local excludes=() listed=()
    for pair in "${COPIES[@]}"; do
        read -r dest src <<< "$pair"
        [[ -d "$install/$dest" ]] || continue
        if [[ -d "$ROOT/$src" ]] && same "$install/$dest" "$ROOT/$src"; then
            excludes+=("--exclude=$name/$dest")
            listed+=("$pair")
        else
            echo "editor_install: $dest differs from $src: packed" >&2
        fi
    done
    printf '%s\n' "${listed[@]}" > "$install/$LIST"
    mkdir -p "$(dirname "$archive")"
    tar -C "$parent" --anchored "${excludes[@]}" -cf - "$name" | zstd -q -T0 -o "$archive" -f
    rm -f "$install/$LIST"
    echo "editor_install: $archive, $(du -h "$archive" | cut -f1); from the checkout: ${#listed[@]} folders"
}

unpack() {
    local archive="$1" dir="${2%/}" name dest src
    mkdir -p "$dir"
    name="$(tar --zstd -tf "$archive" | sed -n '1{s|/.*||;p}')"
    tar -C "$dir" --zstd -xf "$archive"
    local install="$dir/$name"
    if [[ -f "$install/$LIST" ]]; then
        while read -r dest src; do
            [[ -n "$dest" ]] || continue
            [[ -d "$ROOT/$src" ]] || { echo "editor_install: the checkout has no $src for $dest" >&2; exit 1; }
            mkdir -p "$(dirname "$install/$dest")"
            rm -rf "${install:?}/$dest"
            cp -R "$ROOT/$src" "$install/$dest"
            rm -f "$install/$dest/.gitattributes"
        done < "$install/$LIST"
        rm -f "$install/$LIST"
    fi
}

[[ $# -eq 3 ]] || usage
case "$1" in
    pack) pack "$2" "$3" ;;
    unpack) unpack "$2" "$3" ;;
    *) usage ;;
esac
