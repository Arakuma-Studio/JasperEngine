# Shared by generate.sh and check.sh (sourced): the Linux build image and the player runs.
# shellcheck shell=bash

UFM_IMAGE=jasperengine/linux-build

# ufm_image: builds (or reuses) the Linux build image of this checkout; JASPER_DOCKER_IMAGE
# names a prebuilt one instead
ufm_image() {
    if [[ -n "${JASPER_DOCKER_IMAGE:-}" ]]; then
        UFM_IMAGE="$JASPER_DOCKER_IMAGE"
    else
        UFM_IMAGE="$(docker build -q -f "$ROOT/docker/linux.Dockerfile" "$ROOT/docker")"
    fi
}

# ufm_runtime_image: the no-compiler image of a user's computer (docker/runtime.Dockerfile), where
# the prebuilt players run (S30)
ufm_runtime_image() {
    UFM_RUNTIME_IMAGE="$(docker build -q -f "$ROOT/docker/runtime.Dockerfile" "$ROOT/docker")"
}

# ufm_run <command...>: runs a bash command line in the image, in /src (this checkout), as the
# host user, with the checkout's compiler cache; HOME is build/upgrade-from-main/home.
# UFM_RUN_IMAGE=<id> runs another image (the runtime one); the player runtimes of
# build/players are found there (JASPER_PLAYERS_DIR)
ufm_run() {
    local main
    main="$(cd "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
    mkdir -p "$ROOT/build/upgrade-from-main/home" "$main/build/.cache/ccache" "$ROOT/build/.cache/uv"
    local cpu=()
    [[ -n "${DOCKER_CPUS:-}" ]] && cpu=(--cpus "$DOCKER_CPUS" -e LP_NUM_THREADS="$DOCKER_CPUS")
    local uv
    uv="$(command -v uv || true)"
    local uvmount=()
    [[ -n "$uv" ]] && uvmount=(-v "$uv:/usr/local/bin/uv-host:ro")
    docker run --rm ${cpu[@]+"${cpu[@]}"} ${uvmount[@]+"${uvmount[@]}"} \
        --user "$(id -u):$(id -g)" \
        -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
        -v "$ROOT:/src" -w /src \
        -v "$main/.git:$main/.git:ro" \
        -v "$main/build/.cache/ccache:/ccache" -e CCACHE_DIR=/ccache \
        -e HOME=/src/build/upgrade-from-main/home -e UV_CACHE_DIR=/src/build/.cache/uv \
        -e JASPER_PLAYERS_DIR=/src/build/players \
        "${UFM_RUN_IMAGE:-$UFM_IMAGE}" bash -euo pipefail -c "$*"
}

# Inside the image: the uv the image ships, else the host's
UFM_UV='$(command -v uv || echo /usr/local/bin/uv-host)'
UFM_PY="dbus-run-session -- $UFM_UV run --quiet --no-project --with-requirements tools/mcp/requirements.txt python"
