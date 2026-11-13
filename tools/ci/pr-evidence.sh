#!/usr/bin/env bash
# Attaches evidence (screenshots, logs) to a pull request without committing it: gh has no
# attachment upload, so the files become assets of one rolling prerelease, `pr-evidence`, which is
# created when missing and never marked latest (the AppImage's zsync updates follow "latest").
#
#   tools/ci/pr-evidence.sh <pr-number> <file>...
#
# Each file is uploaded as pr<N>-<basename> (characters GitHub would rewrite become '-'), replacing
# an earlier upload of the same name. Prints one Markdown line per file for the PR body: an image
# for .png/.jpg/.jpeg/.gif/.webp, else a link. GH_REPO / -R work as for any gh command.
set -euo pipefail

TAG=pr-evidence

usage() {
    echo "usage: tools/ci/pr-evidence.sh <pr-number> <file>..." >&2
    exit 2
}

[[ $# -ge 2 ]] || usage
PR="$1"
shift
[[ "$PR" =~ ^[1-9][0-9]*$ ]] || { echo "pr-evidence: '$PR' is not a pull request number" >&2; exit 2; }
command -v gh >/dev/null || { echo "pr-evidence: gh is not on PATH" >&2; exit 2; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# Stage every file under its asset name first, so a missing file or a name clash uploads nothing.
NAMES=()
for file in "$@"; do
    [[ -f "$file" ]] || { echo "pr-evidence: $file is not a file" >&2; exit 2; }
    base="$(basename -- "$file")"
    name="pr${PR}-${base//[^A-Za-z0-9._-]/-}"
    if [[ -e "$STAGE/$name" ]]; then
        echo "pr-evidence: two files would both upload as $name; rename one" >&2
        exit 2
    fi
    cp -- "$file" "$STAGE/$name"
    NAMES+=("$name")
done

ensure_release() {
    local pre
    if pre="$(gh release view "$TAG" --json isPrerelease --jq .isPrerelease 2>/dev/null)"; then
        if [[ "$pre" != "true" ]]; then
            echo "pr-evidence: release $TAG exists but is not a prerelease; mark it a prerelease first" >&2
            exit 1
        fi
        return 0
    fi
    gh release create "$TAG" --prerelease --latest=false --title "PR evidence" \
        --notes "Screenshots and logs attached to pull requests by tools/ci/pr-evidence.sh. Not a release." \
        >/dev/null && return 0
    # Another upload may have created it meanwhile.
    gh release view "$TAG" --json isPrerelease --jq .isPrerelease >/dev/null
}

ensure_release
(cd "$STAGE" && gh release upload "$TAG" --clobber "${NAMES[@]}") >/dev/null

ASSETS="$(gh release view "$TAG" --json assets --jq '.assets[] | "\(.name)\t\(.url)"')"
status=0
for name in "${NAMES[@]}"; do
    url="$(awk -F '\t' -v n="$name" '$1 == n { print $2; exit }' <<<"$ASSETS")"
    if [[ -z "$url" ]]; then
        echo "pr-evidence: $name is not among the assets of $TAG after the upload" >&2
        status=1
        continue
    fi
    case "${name,,}" in
        *.png|*.jpg|*.jpeg|*.gif|*.webp) echo "![${name}](${url})" ;;
        *) echo "[${name}](${url})" ;;
    esac
done
exit "$status"
