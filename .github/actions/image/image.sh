#!/usr/bin/env bash
# The image action's work (action.yml): pull the lane's build image from GHCR by its content hash, or
# build it here when it is not there yet, and hand it to tools/build.sh and tools/smoke-test.sh.
#
#   REGISTRY=ghcr.io/<owner> .github/actions/image/image.sh <target>
#
# Run from the repository root. Writes image= and tag= to $GITHUB_OUTPUT and JASPER_DOCKER_IMAGE,
# JASPER_IMAGE_<TARGET> to $GITHUB_ENV when they are set (else prints them).
set -euo pipefail

TARGET="${1:?usage: image.sh <target>}"
REGISTRY="$(tr '[:upper:]' '[:lower:]' <<< "${REGISTRY:?REGISTRY=ghcr.io/<owner>}")"

ref_of() { echo "$REGISTRY/jasperengine-$1-build:$(tools/build.sh image-tag "$1")"; }

# a missing tag fails at once; a second try is for a registry hiccup
pull() {
    docker pull -q "$1" > /dev/null 2>&1 || { sleep 5; docker pull -q "$1" > /dev/null 2>&1; }
}

# The builder of the registry's layer cache: the docker driver cannot read a type=registry cache
builder() {
    docker buildx inspect jasper-ci > /dev/null 2>&1 \
        || docker buildx create --name jasper-ci --driver docker-container > /dev/null
}

# ensure <target>: a local image of the target's tag, pulled or built, into ENSURED; BUILT_HERE=1
# when it was built (a chained image's base is then on this machine only)
ENSURED=""
BUILT_HERE=0
ensure() {
    local target="$1" ref
    ref="$(ref_of "$target")"
    BUILT_HERE=0
    if docker image inspect "$ref" > /dev/null 2>&1 || pull "$ref"; then
        echo "image: $ref" >&2
        ENSURED="$ref"
        return
    fi
    echo "image: $ref is not in the registry; building it (docker/$target.Dockerfile)" >&2
    if [[ "$target" == linux-backends ]]; then
        # FROM the linux image by its hash (docker/linux-backends.Dockerfile's BASE)
        local base
        ensure linux
        base="$ENSURED"
        if [[ $BUILT_HERE == 1 ]]; then
            # a base only this machine has: the docker driver's build, which sees local images
            docker build -q --build-arg "BASE=$base" -t "$ref" -f "docker/$target.Dockerfile" docker > /dev/null
        else
            builder
            docker buildx build --builder jasper-ci --load --build-arg "BASE=$base" -t "$ref" \
                --cache-from "type=registry,ref=$REGISTRY/jasperengine-$target-build:buildcache" \
                -f "docker/$target.Dockerfile" docker >&2
        fi
    else
        builder
        docker buildx build --builder jasper-ci --load -t "$ref" \
            --cache-from "type=registry,ref=$REGISTRY/jasperengine-$target-build:buildcache" \
            -f "docker/$target.Dockerfile" docker >&2
    fi
    BUILT_HERE=1
    ENSURED="$ref"
}

ensure "$TARGET"
IMAGE="$ENSURED"
TAG="${IMAGE##*:}"
VAR="JASPER_IMAGE_$(tr '[:lower:]-' '[:upper:]_' <<< "$TARGET")"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    { echo "image=$IMAGE"; echo "tag=$TAG"; } >> "$GITHUB_OUTPUT"
else
    echo "image=$IMAGE tag=$TAG"
fi
if [[ -n "${GITHUB_ENV:-}" ]]; then
    { echo "JASPER_DOCKER_IMAGE=$IMAGE"; echo "$VAR=$IMAGE"; } >> "$GITHUB_ENV"
else
    echo "JASPER_DOCKER_IMAGE=$IMAGE $VAR=$IMAGE"
fi
