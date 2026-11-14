# Linux build image for the engine and editor.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build pkg-config python3 wget gpg file git ca-certificates \
        libx11-dev libxcursor-dev libxi-dev libxrandr-dev \
        libgl1-mesa-dev libvulkan-dev \
        libwayland-dev wayland-protocols extra-cmake-modules \
        libdbus-1-dev \
        ccache \
        # dav1d's x86-64 asm (the HTML UI's <video>): NASM 2.14 or newer (22.04: 2.15)
        nasm \
        # Headless runs: virtual X server, Mesa software GL and Vulkan (lavapipe), screenshots.
        xvfb xauth x11-utils x11-apps imagemagick libgl1-mesa-dri mesa-utils dbus xdotool \
        mesa-vulkan-drivers vulkan-tools \
    # 22.04 ships CMake 3.22; the vendored glslang requires 3.27 or newer. Pinned: Kitware's
    # repository serves its latest release, and a new CMake can reject what the last one accepted.
    && wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
        > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        cmake=4.4.5-0kitware1ubuntu22.04.1 cmake-data=4.4.5-0kitware1ubuntu22.04.1 \
    && rm -rf /var/lib/apt/lists/*

# sokol's Vulkan backend binds through VK_EXT_descriptor_buffer, which needs Vulkan headers
# 1.3.235 or newer; 22.04's libvulkan-dev is 1.3.204. The headers go to /usr/local, ahead of
# the system ones; the 1.3.204 loader is fine (extension entry points come from the driver).
ARG VULKAN_HEADERS_VERSION=v1.3.296
RUN git clone --quiet --depth 1 --branch ${VULKAN_HEADERS_VERSION} \
        https://github.com/KhronosGroup/Vulkan-Headers.git /tmp/vulkan-headers \
    && cmake -S /tmp/vulkan-headers -B /tmp/vulkan-headers/build -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null \
    && cmake --install /tmp/vulkan-headers/build >/dev/null \
    && rm -rf /tmp/vulkan-headers

# The build runs as the host user; git refuses bind-mounted repos owned by
# someone else unless they are marked safe.
RUN git config --system --add safe.directory '*'

# The editor's file dialogs go through D-Bus, which refuses to start without a
# machine id; container images ship an empty one.
RUN dbus-uuidgen > /etc/machine-id

ENV LIBGL_ALWAYS_SOFTWARE=1

# The editor's HTML UI importers download through the curl executable (phase-2 §3.13.1);
# jasperengine-htmlui-editor-tests runs them against a local fixture server. A layer of its
# own, so images built from older copies of this file reuse every layer above.
RUN apt-get update && apt-get install -y --no-install-recommends curl \
    && rm -rf /var/lib/apt/lists/*

# eMudhra intermediate CA compromise (October 2026): ca-certificates 20260816 distrusts
# "emSign ECC Root CA - G3". A layer of its own so cached images pick it up; the build
# fails while the root is still trusted.
RUN apt-get update && apt-get install -y --no-install-recommends --only-upgrade ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ! test -e /etc/ssl/certs/emSign_ECC_Root_CA_-_G3.pem

# uv runs the MCP tests (tools/mcp/run-tests.sh, tools/build.sh mcp-tests): pinned by version and
# sha256, bumped by hand. A layer of its own, after every other one, so cached images keep theirs.
ARG UV_VERSION=0.13.5
ARG UV_SHA256=f68d291c75783122dad58ab77def0e13f3233f358731db5d6d7e8715603b928f
RUN wget -q -O /tmp/uv.tar.gz \
        "https://github.com/astral-sh/uv/releases/download/${UV_VERSION}/uv-x86_64-unknown-linux-gnu.tar.gz" \
    && echo "${UV_SHA256}  /tmp/uv.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/uv.tar.gz -C /tmp \
    && install -m 0755 /tmp/uv-x86_64-unknown-linux-gnu/uv /tmp/uv-x86_64-unknown-linux-gnu/uvx /usr/local/bin/ \
    && rm -rf /tmp/uv.tar.gz /tmp/uv-x86_64-unknown-linux-gnu \
    && uv --version

# The version-control integration's tests (docs/specs/S29-version-control.md) run git-lfs and svn
# against temporary local repositories (a fake LFS lock server on 127.0.0.1, svnadmin + file://).
# A layer of its own, after every other one, so cached images keep theirs.
RUN apt-get update && apt-get install -y --no-install-recommends git-lfs subversion \
    && rm -rf /var/lib/apt/lists/* \
    && git lfs version && svn --version --quiet

# Test tools of the MCP suite; without them their tests skip. Test tools only: nothing of them
# ships (LuaLS MIT, clangd Apache-2.0 with LLVM exception, Ubuntu's FFmpeg GPL with libaom BSD-2,
# Chrome for Testing under Chromium's BSD-3 and Google's terms).
# - lua-language-server --check and clangd --check (test_stubs.py, test_html_ui_docs.py,
#   test_html_ui_examples.py): the versions the HTML UI acceptance runs used, pinned by version and
#   sha256 (22.04's apt clangd is 14);
# - FFmpeg with an AV1 encoder (libaom-av1) and libopus, the video importer's conversion
#   (test_html_ui_video.py): 22.04's package, pinned by version;
# - Chrome for Testing's headless shell, the web gate's browser (test_mesh_raycast.py's web latency
#   check of the build tools/build.sh mcp-tests MCP_SUITE=all makes): the version and sha256 of
#   docker/web.Dockerfile, its runtime libraries from 22.04 (the build fails if one is missing).
# Bumped by hand. A layer of its own, after every other one, so cached images keep theirs.
ARG FFMPEG_VERSION=7:4.4.2-0ubuntu0.22.04.1
ARG CHROME_HEADLESS_SHELL_VERSION=156.0.8078.47
ARG CHROME_HEADLESS_SHELL_SHA256=0d5acc62c3e213f8c87d3c298d66ffcf9a236d12ea4b9f85318bf85dbc279daa
ARG LUALS_VERSION=3.19.1
ARG LUALS_SHA256=e9235d2d72ef55bc41cf8c99cda2ed64777682024b4bb81f5dea425060c5cbb8
ARG CLANGD_VERSION=18.1.3
ARG CLANGD_SHA256=775f6ac798d9c599258e3f4fa14d0af908b1801d38250e72a18fb907b28a1be6
RUN wget -q -O /tmp/luals.tar.gz \
        "https://github.com/LuaLS/lua-language-server/releases/download/${LUALS_VERSION}/lua-language-server-${LUALS_VERSION}-linux-x64.tar.gz" \
    && echo "${LUALS_SHA256}  /tmp/luals.tar.gz" | sha256sum -c - \
    && mkdir -p /opt/lua-language-server && tar -xzf /tmp/luals.tar.gz -C /opt/lua-language-server \
    && printf '%s\n' '#!/bin/sh' \
        '# its log and meta files go to the user cache (the build runs as the host user: /opt is read-only to it)' \
        'cache="${XDG_CACHE_HOME:-$HOME/.cache}/lua-language-server"' \
        'exec /opt/lua-language-server/bin/lua-language-server --logpath="$cache/log" --metapath="$cache/meta" "$@"' \
        > /usr/local/bin/lua-language-server \
    && chmod 0755 /usr/local/bin/lua-language-server \
    && wget -q -O /tmp/clangd.zip \
        "https://github.com/clangd/clangd/releases/download/${CLANGD_VERSION}/clangd-linux-${CLANGD_VERSION}.zip" \
    && echo "${CLANGD_SHA256}  /tmp/clangd.zip" | sha256sum -c - \
    && python3 -m zipfile -e /tmp/clangd.zip /opt \
    && chmod 0755 /opt/clangd_${CLANGD_VERSION}/bin/clangd \
    && ln -s /opt/clangd_${CLANGD_VERSION}/bin/clangd /usr/local/bin/clangd \
    && rm -f /tmp/luals.tar.gz /tmp/clangd.zip \
    && lua-language-server --version && clangd --version \
    && apt-get update && apt-get install -y --no-install-recommends "ffmpeg=${FFMPEG_VERSION}" \
    && rm -rf /var/lib/apt/lists/* \
    && ffmpeg -hide_banner -encoders | grep -E ' (libaom-av1|libopus) ' | wc -l | grep -qx 2 \
    && apt-get update && apt-get install -y --no-install-recommends unzip fonts-liberation \
        libnss3 libnspr4 libatk1.0-0 libatk-bridge2.0-0 libatspi2.0-0 libcups2 libdbus-1-3 \
        libdrm2 libgbm1 libxkbcommon0 libx11-6 libxcb1 libxcomposite1 libxdamage1 libxext6 libxfixes3 \
        libxrandr2 libpango-1.0-0 libcairo2 libasound2 libexpat1 libglib2.0-0 libudev1 \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL -o /tmp/chrome-headless-shell.zip \
        "https://storage.googleapis.com/chrome-for-testing-public/${CHROME_HEADLESS_SHELL_VERSION}/linux64/chrome-headless-shell-linux64.zip" \
    && echo "${CHROME_HEADLESS_SHELL_SHA256}  /tmp/chrome-headless-shell.zip" | sha256sum -c - \
    && unzip -q /tmp/chrome-headless-shell.zip -d /opt \
    && mv /opt/chrome-headless-shell-linux64 /opt/chrome-headless-shell \
    && rm /tmp/chrome-headless-shell.zip \
    && ln -s /opt/chrome-headless-shell/chrome-headless-shell /usr/local/bin/chrome-headless-shell \
    && ! ldd /opt/chrome-headless-shell/chrome-headless-shell | grep 'not found'

# lld (LLVM 14), the linker of Debug and RelWithDebInfo development and test builds
# (JASPERENGINE_LINKER=AUTO, engine/cmake/JasperLinker.cmake; docs/specs/S33-build-speed.md R3):
# GNU ld takes 20-53 s per link of the editor, its tests and the shared engine with debug
# information. Release builds and shipped games keep GNU ld. A layer of its own, after every other.
RUN apt-get update && apt-get install -y --no-install-recommends lld \
    && rm -rf /var/lib/apt/lists/* \
    && ld.lld --version
