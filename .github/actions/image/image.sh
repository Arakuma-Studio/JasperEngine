#!/usr/bin/env bash
# The image action's work (action.yml): pull the lane's build image from GHCR by its content hash, or
# build it here when it is not there yet, and hand it to tools/build.sh and tools/smoke-test.sh.
#
#   REGISTRY=ghcr.io/<owner> .github/actions/image/image.sh <target>
#
# Run from the repository root. Writes image= and tag= to $GITHUB_OUTPUT and JASPER_DOCKER_IMAGE,
# JASPER_IMAGE_<TARGET> to $GITHUB_ENV when they are set (else prints them). On an overflow runner it
# also notes the image and when, in the runner's tool cache, for its job-completed hook to prune by
# (tools/ci/self-hosted/job-completed.sh): only what CI pulled, never the machine's own images.
set -euo pipefail

TARGET="${1:?usage: image.sh <target>}"
REGISTRY="$(tr '[:upper:]' '[:lower:]' <<< "${REGISTRY:?REGISTRY=ghcr.io/<owner>}")"

ref_of() { echo "$REGISTRY/jasperengine-$1-build:$(tools/build.sh image-tag "$1")"; }

# a missing tag (the registry says so) fails at once; any other failure is tried once more
pull() {
    local err
    err="$(docker pull -q "$1" 2>&1 > /dev/null)" && return 0
    if grep -qiE 'manifest unknown|not found' <<< "$err"; then return 1; fi
    echo "image: pulling $1 failed, trying again: $err" >&2
    sleep 5
    docker pull -q "$1" > /dev/null
}

# The builder of the registry's layer cache: the docker driver cannot read a type=registry cache
builder() {
    docker buildx inspect jasper-ci > /dev/null 2>&1 \
        || docker buildx create --name jasper-ci --driver docker-container > /dev/null
}

# ensure <target>: a local image of the target's tag, pulled or else built, into ENSURED
ENSURED=""
ensure() {
    local target="$1" ref
    ref="$(ref_of "$target")"
    if docker image inspect "$ref" > /dev/null 2>&1; then
        echo "image: $ref is loaded already" >&2
        ENSURED="$ref"
        return
    fi
    local start=$SECONDS
    if pull "$ref"; then
        echo "image: pulled $ref in $((SECONDS - start)) s" >&2
        ENSURED="$ref"
        return
    fi
    echo "image: $ref is not in the registry; building it (docker/$target.Dockerfile)" >&2
    if [[ "$target" == linux-backends ]]; then
        # FROM the linux image of its hash (docker/linux-backends.Dockerfile's BASE): the docker driver's
        # build, which sees that image when it was built here too (a few apt packages, no cache needed)
        ensure linux
        docker build --build-arg "BASE=$ENSURED" -t "$ref" -f "docker/$target.Dockerfile" docker >&2
    else
        builder
        docker buildx build --builder jasper-ci --load -t "$ref" \
            --cache-from "type=registry,ref=$REGISTRY/jasperengine-$target-build:buildcache" \
            -f "docker/$target.Dockerfile" docker >&2
    fi
    echo "image: built $ref in $((SECONDS - start)) s" >&2
    ENSURED="$ref"
}

ensure "$TARGET"
IMAGE="$ENSURED"
if [[ "${RUNNER_ENVIRONMENT:-}" == self-hosted && -n "${RUNNER_TOOL_CACHE:-}" ]]; then
    echo "$IMAGE $(date +%s)" >> "$RUNNER_TOOL_CACHE/je-ci-images.txt"
fi
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
