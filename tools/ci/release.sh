#!/usr/bin/env bash
# The GitHub release side of .github/workflows/{nightly,release,build-release}.yml
# (docs/superpowers/specs/2026-11-12-ci-design.md §12), through gh on GITHUB_REPOSITORY (GH_TOKEN):
#
#   tools/ci/release.sh newest-nightly
#       prints "<tag> <commit>" of the newest published nightly-YYYYMMDD release (nothing when none)
#   tools/ci/release.sh publish <nightly|stable> <version> <commit> <asset dir> [--dry-run]
#       writes <asset dir>/SHA256SUMS from the files there, creates the release as a draft (or takes
#       the draft a failed run left), uploads every file with --clobber, checks the draft holds each
#       one at its size, then publishes it (a nightly as a prerelease, never "latest"; a stable as
#       "latest"). Refused when an editor package of §12.3 or every player archive is missing.
#       --dry-run stops at the complete draft. The body: GitHub's generated notes and
#       "Opening unsigned builds". A release already published for another commit is refused.
#   tools/ci/release.sh check <nightly|stable> <version> <commit>
#       exits 2 unless the version suits the channel (nightly-YYYYMMDD, vX.Y.Z) and the commit is a full SHA
#   tools/ci/release.sh prune <keep> [<current version>]
#       deletes the published nightlies past the newest <keep> (release and tag), and nightly drafts
#       other than <current version>'s
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO="${GITHUB_REPOSITORY:-}"
NIGHTLY='^nightly-[0-9]{8}$'
STABLE='^v[0-9]+\.[0-9]+\.[0-9]+$'

usage() {
    echo "usage: tools/ci/release.sh check <nightly|stable> <version> <commit> | newest-nightly | publish <nightly|stable> <version> <commit> <asset dir> [--dry-run] | prune <keep> [<current version>]" >&2
    exit 2
}

# "<tag> <draft>" of every nightly release, newest first
nightlies() {
    gh release list --repo "$REPO" --limit 500 --json tagName,isDraft \
        --jq '.[] | select(.tagName | test("'"$NIGHTLY"'")) | "\(.tagName) \(.isDraft)"' | sort -r
}

newest_nightly() {
    local tag
    tag="$(nightlies | awk '$2 == "false" { print $1; exit }')"
    [[ -n "$tag" ]] || return 0
    echo "$tag $(gh api "repos/$REPO/commits/$tag" --jq .sha)"
}

body() {
    local version="$1" commit="$2" previous="$3"
    local args=(-f "tag_name=$version" -f "target_commitish=$commit")
    [[ -n "$previous" ]] && args+=(-f "previous_tag_name=$previous")
    gh api "repos/$REPO/releases/generate-notes" "${args[@]}" --jq .body
    cat <<'EOF'

## Opening unsigned builds

These builds are not code-signed or notarized.

- **macOS:** right-click the app and choose Open the first time, or run `xattr -dr com.apple.quarantine JasperEngine.app`.
- **Windows:** when SmartScreen stops it, choose More info, then Run anyway.
- **Linux:** `chmod +x` the AppImage, then run it.

`SHA256SUMS` lists the SHA-256 of every file of this release (`sha256sum -c SHA256SUMS`).
EOF
}

check() {
    [[ $# -eq 3 ]] || usage
    local channel="$1" version="$2" commit="$3"
    case "$channel" in
        nightly) [[ "$version" =~ $NIGHTLY ]] || { echo "release: a nightly is nightly-YYYYMMDD (got $version)" >&2; exit 2; } ;;
        # three parts: an editor stamped vX.Y is a development editor and downloads no players
        stable) [[ "$version" =~ $STABLE ]] || { echo "release: a stable release is vX.Y.Z (got $version)" >&2; exit 2; } ;;
        *) echo "release: the channel is nightly or stable (got $channel)" >&2; exit 2 ;;
    esac
    [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || { echo "release: the commit is a full 40-hex SHA (got $commit)" >&2; exit 2; }
}

publish() {
    [[ $# -ge 4 && $# -le 5 ]] || usage
    local channel="$1" version="$2" commit="$3" dir="$4" dry_run=0
    [[ $# -eq 5 ]] && { [[ "$5" == --dry-run ]] || usage; dry_run=1; }
    check "$channel" "$version" "$commit"
    [[ -d "$dir" ]] || { echo "release: no asset directory $dir" >&2; exit 2; }
    local expected missing=0
    for expected in windows-x64.zip linux-x86_64.AppImage linux-x86_64.AppImage.zsync linux-x86_64.zip \
                    macos-universal.zip macos-universal.dmg; do
        [[ -f "$dir/JasperEngine-$version-$expected" ]] || { echo "release: no JasperEngine-$version-$expected" >&2; missing=1; }
    done
    compgen -G "$dir/jasperengine-player-*-$version.zip" > /dev/null || { echo "release: no player archive of $version" >&2; missing=1; }
    [[ $missing -eq 0 ]] || exit 1

    rm -f "$dir/SHA256SUMS"
    local files=()
    while IFS= read -r -d '' f; do files+=("$(basename "$f")"); done < <(find "$dir" -maxdepth 1 -type f -print0 | sort -z)
    [[ ${#files[@]} -gt 0 ]] || { echo "release: no assets in $dir" >&2; exit 1; }
    (cd "$dir" && sha256sum -- "${files[@]}" > SHA256SUMS)
    files+=(SHA256SUMS)
    echo "release: ${#files[@]} assets for $version ($commit)"

    local state=""
    state="$(gh release view "$version" --repo "$REPO" --json isDraft --jq .isDraft 2>/dev/null || true)"
    if [[ "$state" == false ]]; then
        local at
        at="$(gh api "repos/$REPO/commits/$version" --jq .sha)"
        [[ "$at" == "$commit" ]] || { echo "release: $version is already published, at $at, not $commit" >&2; exit 1; }
        echo "release: $version is published at $commit already: its assets are uploaded again"
    fi

    local previous=""
    if [[ "$channel" == nightly ]]; then
        previous="$(nightlies | awk -v v="$version" '$2 == "false" && $1 != v { print $1; exit }')"
    fi
    local notes
    notes="$(mktemp)"
    body "$version" "$commit" "$previous" > "$notes"

    local kind=()
    [[ "$channel" == nightly ]] && kind=(--prerelease)
    if [[ -z "$state" ]]; then
        gh release create "$version" --repo "$REPO" --draft --target "$commit" --title "$version" \
            --notes-file "$notes" ${kind[@]+"${kind[@]}"}
    else
        gh release edit "$version" --repo "$REPO" --notes-file "$notes"
    fi
    (cd "$dir" && gh release upload "$version" --repo "$REPO" --clobber -- "${files[@]}")

    # every file at its size: a release is a draft until all of it is there
    local listed f size
    listed="$(gh release view "$version" --repo "$REPO" --json assets --jq '.assets[] | "\(.name) \(.size)"')"
    for f in "${files[@]}"; do
        size="$(stat -c %s "$dir/$f")"
        grep -qxF "$f $size" <<< "$listed" || { echo "release: $f ($size bytes) is not on the release" >&2; missing=1; }
    done
    [[ $missing -eq 0 ]] || exit 1

    if [[ $dry_run -eq 1 ]]; then
        echo "release: dry run: $version stays a draft with every asset (delete it: gh release delete $version --repo $REPO --yes)"
        return 0
    fi
    if [[ "$channel" == nightly ]]; then
        gh release edit "$version" --repo "$REPO" --draft=false --prerelease --latest=false
    else
        gh release edit "$version" --repo "$REPO" --draft=false --latest
    fi
    echo "release: published $version"
}

prune() {
    [[ $# -ge 1 && $# -le 2 && "$1" =~ ^[0-9]+$ ]] || usage
    local keep="$1" current="${2:-}" n=0 tag draft
    while read -r tag draft; do
        [[ -n "$tag" ]] || continue
        if [[ "$draft" == true ]]; then
            [[ "$tag" == "$current" ]] && continue
            echo "release: deleting the nightly draft $tag"
            gh release delete "$tag" --repo "$REPO" --yes
            continue
        fi
        n=$((n + 1))
        [[ $n -le $keep ]] && continue
        echo "release: deleting $tag (older than the newest $keep nightlies)"
        gh release delete "$tag" --repo "$REPO" --cleanup-tag --yes
    done < <(nightlies)
}

cd "$ROOT"
[[ $# -ge 1 ]] || usage
command="$1"; shift
[[ "$command" == check || -n "$REPO" ]] || { echo "release: GITHUB_REPOSITORY (owner/name) names the repository" >&2; exit 2; }
case "$command" in
    check) check "$@" ;;
    newest-nightly) [[ $# -eq 0 ]] || usage; newest_nightly ;;
    publish) publish "$@" ;;
    prune) prune "$@" ;;
    *) usage ;;
esac
