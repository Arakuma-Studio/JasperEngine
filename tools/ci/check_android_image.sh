#!/usr/bin/env bash
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# The Android build image holds the toolchain the Gradle template pins (docs/specs/S98-editor-tooling-gaps.md
# REQ-17, AC15): engine/workspaces/androidstudio/app/build.gradle's ndkVersion and
# externalNativeBuild.cmake.version are installed in the image's SDK (docker/android.Dockerfile), so a
# fresh container downloads neither. Run inside the image; tools/build.sh android runs it first.
#
#   tools/build.sh exec android tools/ci/check_android_image.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GRADLE="$ROOT/engine/workspaces/androidstudio/app/build.gradle"
NDK="$(sed -n 's/^ *ndkVersion *= *"\([^"]*\)".*/\1/p' "$GRADLE" | head -1)"
CMAKE="$(sed -n 's/^ *version *=* *"\([0-9][^"]*\)".*/\1/p' "$GRADLE" | head -1)"
[[ -n "$NDK" ]] || { echo "FAIL: $GRADLE pins no ndkVersion" >&2; exit 1; }
[[ -n "$CMAKE" ]] || { echo "FAIL: $GRADLE pins no externalNativeBuild.cmake.version" >&2; exit 1; }
INSTALLED="$(sdkmanager --list_installed 2>/dev/null)"
status=0
for package in "ndk;$NDK" "cmake;$CMAKE"; do
    if grep -qE "^ *$package +\|" <<< "$INSTALLED"; then
        echo "ok: $package is in the image"
    else
        echo "FAIL: the Android image has no $package, which app/build.gradle pins: install it in docker/android.Dockerfile" >&2
        status=1
    fi
done
exit $status
