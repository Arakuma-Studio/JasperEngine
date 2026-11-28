#!/usr/bin/env bash
# Installs ccache on a Windows runner from its official release zip (github.com/ccache/ccache), pinned
# by version and sha256 below, for .github/actions/ccache-restore. Chocolatey was the source before:
# during its outage (503/504) `choco install ccache` exited 0 with nothing installed, so
# tools/ci/retry.sh stopped retrying and the job failed later on `ccache: command not found` (run
# 46050080028).
#
#   tools/ci/ccache_windows.sh <dir>
#
# Keeps the zip in <dir> (ccache-restore caches that directory) and downloads it only when <dir> has
# none with the pinned sha256; a download goes through tools/ci/retry.sh (curl -f: an HTTP error is a
# failure, retried). Unpacks it in <dir>, runs its ccache.exe --version, which must name the pinned
# version, and adds its directory to GITHUB_PATH. Exits 1 on a checksum or version mismatch, 2 on usage.
set -euo pipefail

CCACHE_VERSION=4.14.3
CCACHE_SHA256=c845c3d65be81954c75659145a522a793a27586bf940c5dcfe18af33fa1a67dd

[[ $# -eq 1 && -n "$1" ]] || { sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$1"
NAME="ccache-$CCACHE_VERSION-windows-x86_64"
ZIP="$DIR/$NAME.zip"
URL="https://github.com/ccache/ccache/releases/download/v$CCACHE_VERSION/$NAME.zip"
mkdir -p "$DIR"

verified() { echo "$CCACHE_SHA256  $1" | sha256sum -c --status - 2> /dev/null; }

if [[ -f "$ZIP" ]] && verified "$ZIP"; then
    echo "ccache_windows: $NAME.zip from the cache"
else
    rm -f "$ZIP" "$ZIP.part"
    "$ROOT/tools/ci/retry.sh" curl -fsSL --connect-timeout 30 --max-time 300 -o "$ZIP.part" "$URL"
    if ! verified "$ZIP.part"; then
        echo "ccache_windows: $URL is not the pinned $NAME.zip: sha256 $(sha256sum "$ZIP.part" | cut -d' ' -f1)," \
             "expected $CCACHE_SHA256" >&2
        rm -f "$ZIP.part"
        exit 1
    fi
    mv "$ZIP.part" "$ZIP"
fi

rm -rf "${DIR:?}/$NAME"
if command -v unzip > /dev/null; then
    unzip -q -o "$ZIP" -d "$DIR"
else
    powershell.exe -NoProfile -NonInteractive -Command \
        "Expand-Archive -LiteralPath '$(cygpath -w "$ZIP")' -DestinationPath '$(cygpath -w "$DIR")' -Force"
fi

# (CCACHE_DIR of its own: without it Windows ccache reads its configuration from LOCALAPPDATA, which an
# overflow runner's job environment does not have)
version="$(CCACHE_DIR="$DIR/version-check" "$DIR/$NAME/ccache.exe" --version | sed -n 1p)" || version=""
if [[ "$version" != "ccache version $CCACHE_VERSION" ]]; then
    echo "ccache_windows: $DIR/$NAME/ccache.exe --version said '$version', not 'ccache version $CCACHE_VERSION'" >&2
    exit 1
fi
echo "ccache_windows: $version in $DIR/$NAME"
if [[ -n "${GITHUB_PATH:-}" ]]; then
    (cygpath -w "$DIR/$NAME" 2> /dev/null || echo "$DIR/$NAME") >> "$GITHUB_PATH"
fi
