#!/usr/bin/env bash
# Smoke test of a project made from each Project Hub template (editor/resources/templates/<id>): the
# editor's create_project makes it (File > New Project from the template, in the linux image, through
# `jasperengine-editor call`), then tools/smoke-test.sh builds it, runs it and opens it in the editor.
# Needs tools/build.sh linux, and for the default SMOKE_METHOD=prebuilt the player runtimes
# (tools/build.sh players). Projects go to build/ci-templates/<id>, results to build/smoke-template-<id>/.
#
#   tools/ci/smoke-templates.sh [<template id>...]     default: every template
#
# SMOKE_METHOD (prebuilt, compile, build) and the other SMOKE_* variables go to tools/smoke-test.sh.
# JASPER_IMAGE_LINUX / JASPER_IMAGE_RUNTIME name prebuilt images of docker/linux.Dockerfile and
# docker/runtime.Dockerfile (CI's .github/actions/image); unset, each script builds its own.
# Runs every template and exits 1 when one failed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
EDITOR=build/linux/editor-install/bin/jasperengine-editor
[[ -x "$EDITOR" ]] || { echo "No editor at $EDITOR; run tools/build.sh linux" >&2; exit 1; }
METHOD="${SMOKE_METHOD:-prebuilt}"

TEMPLATES=("$@")
if [[ ${#TEMPLATES[@]} -eq 0 ]]; then
    for dir in editor/resources/templates/*/; do
        [[ -f "$dir/template.json" ]] && TEMPLATES+=("$(basename "$dir")")
    done
fi
[[ ${#TEMPLATES[@]} -gt 0 ]] || { echo "smoke-templates: no template in editor/resources/templates" >&2; exit 1; }
for id in "${TEMPLATES[@]}"; do
    [[ "$id" =~ ^[A-Za-z0-9_-]+$ && -f "editor/resources/templates/$id/template.json" ]] \
        || { echo "smoke-templates: no template $id (editor/resources/templates/<id>/template.json)" >&2; exit 1; }
done

# The editor that creates them needs a project open: the empty template's own files are one
WORK=build/ci-templates
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r editor/resources/templates/empty "$WORK/seed"
CREATE="set -e"
for id in "${TEMPLATES[@]}"; do
    CREATE="$CREATE; $EDITOR call --project /src/$WORK/seed --timeout 300 create_project --path /src/$WORK/$id --template $id --unsaved discard"
done
JASPER_DOCKER_IMAGE="${JASPER_IMAGE_LINUX:-}" tools/build.sh exec linux dbus-run-session -- bash -c "$CREATE"

IMAGE="${JASPER_IMAGE_RUNTIME:-}"
[[ "$METHOD" == prebuilt ]] || IMAGE="${JASPER_IMAGE_LINUX:-}"
FAILED=()
for id in "${TEMPLATES[@]}"; do
    echo "smoke-templates: $id (SMOKE_METHOD=$METHOD)"
    JASPER_DOCKER_IMAGE="$IMAGE" SMOKE_METHOD="$METHOD" SMOKE_DIR="build/smoke-template-$id" \
        tools/smoke-test.sh "$WORK/$id" || FAILED+=("$id")
done
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "smoke-templates: FAILED ${FAILED[*]} (reproduce: SMOKE_METHOD=$METHOD tools/ci/smoke-templates.sh ${FAILED[*]})" >&2
    exit 1
fi
echo "smoke-templates: ${#TEMPLATES[@]} template(s) passed: ${TEMPLATES[*]}"
