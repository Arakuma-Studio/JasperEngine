# Linux build image for the engine and editor. Mirrors the Ubuntu jobs in
# .github/workflows so a local build fails the same way CI would.
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
    # 22.04 ships CMake 3.22; the vendored glslang requires 3.27 or newer.
    && wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
        > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends cmake \
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
