#!/usr/bin/env bash
# The Linux editor's release packages (docs/superpowers/specs/2026-11-12-ci-design.md §12.2-12.3,
# contract C5), from build/linux/editor-install (tools/build.sh linux), in the linux image (Ubuntu
# 22.04: its glibc is the packages' floor). From the host it runs itself there through
# `tools/build.sh exec linux`.
#
#   tools/ci/package-linux.sh [--owner <github owner>] [--no-check] <version>
#
#   dist/JasperEngine-<version>-linux-x86_64.AppImage   the install under usr/ of an AppDir, with this
#                                                       image's libstdc++ and libgcc_s (taken over only
#                                                       where the host's are older) and an AppRun that
#                                                       turns an older glibc away with a message
#   dist/JasperEngine-<version>-linux-x86_64.AppImage.zsync  its delta-update file; the AppImage embeds
#                                                       gh-releases-zsync|<owner>|JasperEngine|latest|...
#   dist/JasperEngine-<version>-linux-x86_64.zip        the install's bin/
#
# owner: --owner, else GITHUB_REPOSITORY_OWNER, else the owner in `git remote get-url origin`. Then,
# unless --no-check, the AppImage's editor runs `call --help`, and `call --project <scratch copy of
# tools/fixtures/post-processing> editor_status` (a one-shot headless editor, under Xvfb).
# appimagetool is pinned by version and sha256 (cached in build/.cache/appimagetool/). Log:
# build/test-results/linux-package.log. Exit 2 on bad arguments, 1 when a step fails.
set -euo pipefail
# shellcheck source-path=SCRIPTDIR source=platform-common.sh
. "$(dirname "${BASH_SOURCE[0]}")/platform-common.sh"

USAGE="$(ci_header "${BASH_SOURCE[0]}")"
OWNER="${GITHUB_REPOSITORY_OWNER:-}"
CHECK=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --owner) [[ $# -ge 2 ]] || ci_usage "$USAGE"; OWNER="$2"; shift 2 ;;
        --no-check) CHECK=0; shift ;;
        -*) ci_usage "$USAGE" ;;
        *) break ;;
    esac
done
[[ $# == 1 ]] || ci_usage "$USAGE"
VERSION="$1"
# (the names it goes into: a version a tag or a nightly can carry, nothing a path or a URL breaks on)
[[ "$VERSION" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] || ci_usage "version: letters, digits and ._+- (got '$VERSION')"
if [[ -z "$OWNER" ]]; then
    # git@github.com:<owner>/<repo>.git or https://github.com/<owner>/<repo>(.git)
    OWNER="$(git remote get-url origin 2>/dev/null | sed -nE 's#^(git@|ssh://git@|https://)github\.com[:/]([^/]+)/.*#\2#p')"
fi
[[ "$OWNER" =~ ^[A-Za-z0-9-]+$ ]] || ci_usage "no GitHub owner for the zsync update string (got '$OWNER'): pass --owner or set GITHUB_REPOSITORY_OWNER"

# On the host: the same command in the linux image (tools/build.sh forwards no environment, so the
# owner goes as an argument)
if [[ ! -f /.dockerenv ]] || command -v docker >/dev/null 2>&1; then
    flags=(--owner "$OWNER")
    [[ $CHECK == 0 ]] && flags+=(--no-check)
    exec tools/build.sh exec linux tools/ci/package-linux.sh "${flags[@]}" "$VERSION"
fi

INSTALL=build/linux/editor-install
WORK=build/linux/package
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
# The oldest glibc the message below names: Ubuntu 22.04's (Debian 12 has 2.36). The image is
# ubuntu:22.04 (docker/linux.Dockerfile), so its binaries need no newer one; a newer image would.
MESSAGE_GLIBC=2.35

package() {
    [[ -x "$INSTALL/bin/jasperengine-editor" ]] || { echo "no editor in $INSTALL: run tools/build.sh linux first" >&2; exit 1; }
    grep -aqF -- "$VERSION" "$INSTALL/bin/jasperengine-editor" \
        || echo "::warning::$INSTALL/bin/jasperengine-editor does not carry the version string $VERSION: it was stamped with another one"
    local appimage="JasperEngine-$VERSION-linux-x86_64.AppImage"
    rm -rf "$WORK" && mkdir -p "$WORK" dist
    rm -f "dist/$appimage" "dist/$appimage.zsync" "dist/JasperEngine-$VERSION-linux-x86_64.zip"

    # The AppDir is the same install tree under usr/: $ORIGIN and engine/ already resolve relative to
    # the binary, so nothing moves
    local appdir="$WORK/AppDir"
    mkdir -p "$appdir"
    cp -a "$INSTALL" "$appdir/usr"
    cp editor/resources/linux/jasperengine-editor.desktop "$appdir/"
    cp editor/resources/icons/linux/jasperengine-editor-512.png "$appdir/jasperengine-editor.png"

    # 22.04's libstdc++ is newer than older targets carry, and JASPERENGINE_SHARED rules out
    # -static-libstdc++: the editor and libjasperengine.so would each get their own runtime
    mkdir -p "$appdir/usr/lib"
    local lib
    for lib in libstdc++.so.6 libgcc_s.so.1; do
        cp -L "$(gcc -print-file-name="$lib")" "$appdir/usr/lib/$lib"
    done

    # glibc cannot ride along the way libstdc++ can, so AppRun turns anything older than these
    # binaries were linked against away with a message
    local floor
    floor="$(objdump -T "$appdir/usr/bin/jasperengine-editor" "$appdir/usr/bin/libjasperengine.so" \
        "$appdir/usr/lib/libstdc++.so.6" "$appdir/usr/lib/libgcc_s.so.1" \
        | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
    [[ -n "$floor" ]] || { echo "no GLIBC_ symbol versions in the binaries" >&2; exit 1; }
    echo "glibc floor: $floor"
    if [[ "$(printf '%s\n%s\n' "$floor" "$MESSAGE_GLIBC" | sort -V | tail -1)" != "$MESSAGE_GLIBC" ]]; then
        echo "the binaries need glibc $floor, newer than the $MESSAGE_GLIBC of Ubuntu 22.04 that AppRun's message names: update it" >&2
        exit 1
    fi
    write_apprun "$floor" > "$appdir/AppRun"
    chmod +x "$appdir/AppRun"

    # Pinned: "continuous" would put an unreviewed build of a tool that repackages the editor into the
    # release path
    local tool="build/.cache/appimagetool/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage"
    if ! echo "$APPIMAGETOOL_SHA256  $tool" | sha256sum -c --status - 2>/dev/null; then
        mkdir -p "$(dirname "$tool")"
        rm -f "$tool"
        wget -q -O "$tool.part" \
            "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage"
        echo "$APPIMAGETOOL_SHA256  $tool.part" | sha256sum -c -
        mv "$tool.part" "$tool"
    fi
    chmod +x "$tool"
    # ARCH is guessed from ELF files otherwise, and a container has no FUSE. -u embeds the update
    # information and writes the .zsync (into the working directory) for AppImageUpdate and Gear Lever
    (cd dist && ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$ROOT/$tool" "$ROOT/$appdir" \
        -u "gh-releases-zsync|$OWNER|JasperEngine|latest|JasperEngine-*-x86_64.AppImage.zsync" "$appimage")
    # (-u embeds the update information even when zsyncmake did not run)
    test -s "dist/$appimage.zsync" || { echo "appimagetool wrote no dist/$appimage.zsync" >&2; exit 1; }

    ci_zip_dir "$INSTALL/bin" "dist/JasperEngine-$VERSION-linux-x86_64.zip"
    ls -la dist
    [[ $CHECK == 1 ]] && launch_check "dist/$appimage"
    return 0
}

write_apprun() {
    sed "s/@GLIBC_FLOOR@/$1/g" <<'APPRUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"

# Highest GLIBCXX_3.4.N a libstdc++ carries. strings(1) is not a given on a
# desktop, grep is.
glibcxx_minor() {
  [ -r "$1" ] || return 1
  grep -ao 'GLIBCXX_3\.4\.[0-9][0-9]*' "$1" | sed 's/.*\.//' | sort -n | tail -1
}

# Only take over where the host's runtime is older: doing it everywhere would
# hand the host's Mesa an older libstdc++ than it was built against. Ask the
# loader where that copy is rather than guess at distro layouts.
host_lib="$(LD_TRACE_LOADED_OBJECTS=1 "$HERE/usr/bin/jasperengine-editor" 2>/dev/null \
  | sed -n 's/.*libstdc++\.so\.6 => \([^ ]*\).*/\1/p')"
bundled="$(glibcxx_minor "$HERE/usr/lib/libstdc++.so.6")"
host="$(glibcxx_minor "$host_lib")"
if [ -n "$bundled" ] && { [ -z "$host" ] || [ "$host" -lt "$bundled" ]; }; then
  LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
fi

glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $NF}')"
if [ -n "$glibc" ] && [ "$glibc" != "@GLIBC_FLOOR@" ] \
  && [ "$(printf '%s\n%s\n' "$glibc" "@GLIBC_FLOOR@" | sort -V | head -1)" = "$glibc" ]; then
  msg="JasperEngine needs glibc @GLIBC_FLOOR@ or newer, and this system has $glibc.
Ubuntu 22.04 and Debian 12 are the oldest releases it runs on; older ones
need a build from source."
  printf '%s\n' "$msg" >&2
  # Started from a launcher the message has nowhere to go, so it needs a
  # window. Closing a zenity dialog exits non-zero, so pick on what exists.
  if [ ! -t 2 ]; then
    if command -v zenity >/dev/null 2>&1; then zenity --error --no-wrap --text="$msg" 2>/dev/null
    elif command -v xmessage >/dev/null 2>&1; then xmessage -center "$msg" 2>/dev/null
    fi
  fi
  exit 1
fi

exec "$HERE/usr/bin/jasperengine-editor" "$@"
APPRUN
}

# The design's launch check (§12.2 step 4): the editor has no --version, so `call --help` shows the
# AppImage starts, and a one-shot headless editor opening a project shows the editor runs
launch_check() {
    local appimage="$ROOT/$1" scratch="$WORK/launch-check"
    rm -rf "$scratch" && mkdir -p "$scratch"
    cp -R tools/fixtures/post-processing "$scratch/project"
    echo "== $1 call --help"
    (cd "$scratch" && APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" call --help) > "$scratch/help.log" 2>&1 \
        || { cat "$scratch/help.log"; echo "FAIL: call --help" >&2; return 1; }
    head -5 "$scratch/help.log"
    echo "== $1 call --project <copy of tools/fixtures/post-processing> editor_status"
    (cd "$scratch" && APPIMAGE_EXTRACT_AND_RUN=1 tools_xvfb "$appimage" call --project project editor_status) > "$scratch/status.log" 2>&1 \
        || { cat "$scratch/status.log"; echo "FAIL: editor_status through a one-shot headless editor" >&2; return 1; }
    cat "$scratch/status.log"
    grep -q 'post-processing\|project' "$scratch/status.log" || { echo "FAIL: editor_status names no project" >&2; return 1; }
    echo "ok: the AppImage's editor starts and opens a project"
}

tools_xvfb() { env -u DISPLAY "$ROOT/tools/with-xvfb.sh" "$@"; }

ci_logged linux-package package
