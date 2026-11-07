#!/usr/bin/env bash
# A release's tag check: a release ships only when its tag is a
# version (vX.Y or vX.Y.Z) and a tag of exactly the commit that was built, the commit whose
# git describe named every binary (CMakeLists.txt). There is no VERSION file: git is the one
# source of the version (docs/testing.md).
#
#   tools/ci/check-release-tag.sh <tag> [<commit>]     (commit: default HEAD)
set -euo pipefail

[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: tools/ci/check-release-tag.sh <tag> [<commit>]" >&2; exit 2; }
TAG="$1"
COMMIT="${2:-HEAD}"

if [[ ! "$TAG" =~ ^v[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
    echo "check-release-tag: '$TAG' is not a release version (vX.Y or vX.Y.Z)" >&2
    exit 1
fi
# the version tags on the built commit (git describe --exact-match names only one of them)
AT_COMMIT="$(git tag --points-at "$COMMIT" --list 'v[0-9]*')"
if ! grep -qxF "$TAG" <<< "$AT_COMMIT"; then
    echo "check-release-tag: $TAG is not a tag of the built commit $(git rev-parse --short "$COMMIT") (its version tags: ${AT_COMMIT:-none})" >&2
    exit 1
fi
echo "check-release-tag: $TAG is the built commit $(git rev-parse --short "$COMMIT")"
