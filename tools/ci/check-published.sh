#!/usr/bin/env bash
# A published release as a user gets it (.github/workflows/build-release.yml, design AC9): its Linux
# AppImage, downloaded from the release, matches the release's SHA256SUMS, and downloads a player
# runtime from its own release (`players download`, the archive checked against the hash table the
# editor embeds; a development editor downloads nothing). gh on the host (GITHUB_REPOSITORY,
# GH_TOKEN), the editor in the Linux image:
#
#   tools/ci/check-published.sh <version> [<runtime id>]     (default linux-x64-glcore-release)
#
# Files and logs go to build/published/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: tools/ci/check-published.sh <version> [<runtime id>]" >&2; exit 2; }
VERSION="$1"
ID="${2:-linux-x64-glcore-release}"
[[ "$VERSION" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ && "$ID" =~ ^[a-z0-9-]+$ ]] || { echo "check-published: bad version or id" >&2; exit 2; }
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY (owner/name) names the repository}"
OUT=build/published
APPIMAGE="JasperEngine-$VERSION-linux-x86_64.AppImage"
rm -rf "$OUT"
mkdir -p "$OUT/cwd"

gh release download "$VERSION" --repo "$REPO" -p "$APPIMAGE" -p SHA256SUMS -D "$OUT"
grep -q "  $APPIMAGE\$" "$OUT/SHA256SUMS" || { echo "check-published: SHA256SUMS lists no $APPIMAGE" >&2; exit 1; }
(cd "$OUT" && sha256sum -c --ignore-missing SHA256SUMS)
chmod +x "$OUT/$APPIMAGE"

# no FUSE in a container: the AppImage runtime extracts itself instead of mounting
if ! tools/build.sh exec linux bash -c "cd $OUT/cwd && APPIMAGE_EXTRACT_AND_RUN=1 ../$APPIMAGE players download $ID --players-dir ../players" \
        > "$OUT/download.log" 2>&1; then
    cat "$OUT/download.log"
    echo "check-published: the $VERSION AppImage could not download $ID from its release" >&2
    exit 1
fi
cat "$OUT/download.log"
if grep -q "no release hashes" "$OUT/download.log"; then
    echo "check-published: the $VERSION editor has no release hash table" >&2
    exit 1
fi
grep -q "^installed: $ID\$" "$OUT/download.log" || { echo "check-published: $ID was not installed" >&2; exit 1; }
echo "check-published: $VERSION's AppImage matches SHA256SUMS and downloaded $ID from its own release"
