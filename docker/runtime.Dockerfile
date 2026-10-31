# The no-compiler image of docs/specs/S30-shipping.md AC1-AC3: what a user's computer has without
# a C++ toolchain, emsdk, Android NDK or Gradle. The editor install and the prebuilt player
# runtimes come from the build images (tools/build.sh linux, tools/build.sh players); here they
# build games (jasperengine-editor build) and run them under Xvfb (Mesa software GL and Vulkan),
# and the web build in Chrome's headless shell. The last layer proves there is no compiler.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates python3 file git \
        libx11-6 libxcursor1 libxi6 libxrandr2 libxext6 libxkbcommon0 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libdecor-0-0 \
        libgl1 libegl1 libgles2 libgl1-mesa-dri libvulkan1 mesa-vulkan-drivers libdbus-1-3 dbus \
        xvfb xauth x11-utils x11-apps imagemagick wget \
    && rm -rf /var/lib/apt/lists/*

RUN git config --system --add safe.directory '*' && dbus-uuidgen > /etc/machine-id
ENV LIBGL_ALWAYS_SOFTWARE=1

# Chrome for Testing's headless shell (tools/web/browser_gate.py), as in docker/web.Dockerfile
ARG CHROME_HEADLESS_SHELL_VERSION=156.0.8078.47
ARG CHROME_HEADLESS_SHELL_SHA256=0d5acc62c3e213f8c87d3c298d66ffcf9a236d12ea4b9f85318bf85dbc279daa
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl unzip fonts-liberation \
        libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libatspi2.0-0t64 libcups2t64 \
        libdrm2 libgbm1 libxcb1 libxcomposite1 libxdamage1 libxfixes3 \
        libpango-1.0-0 libcairo2 libasound2t64 libexpat1 libglib2.0-0t64 libudev1 \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL -o /tmp/chrome-headless-shell.zip \
        "https://storage.googleapis.com/chrome-for-testing-public/${CHROME_HEADLESS_SHELL_VERSION}/linux64/chrome-headless-shell-linux64.zip" \
    && echo "${CHROME_HEADLESS_SHELL_SHA256}  /tmp/chrome-headless-shell.zip" | sha256sum -c - \
    && unzip -q /tmp/chrome-headless-shell.zip -d /opt \
    && mv /opt/chrome-headless-shell-linux64 /opt/chrome-headless-shell \
    && rm /tmp/chrome-headless-shell.zip \
    && ln -s /opt/chrome-headless-shell/chrome-headless-shell /usr/local/bin/chrome-headless-shell \
    && ! ldd /opt/chrome-headless-shell/chrome-headless-shell | grep 'not found'

# uv runs the editor's MCP checks of the smoke test (tools/mcp/nav_smoke.py), as in docker/linux.Dockerfile
ARG UV_VERSION=0.13.5
ARG UV_SHA256=f68d291c75783122dad58ab77def0e13f3233f358731db5d6d7e8715603b928f
RUN wget -q -O /tmp/uv.tar.gz \
        "https://github.com/astral-sh/uv/releases/download/${UV_VERSION}/uv-x86_64-unknown-linux-gnu.tar.gz" \
    && echo "${UV_SHA256}  /tmp/uv.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/uv.tar.gz -C /tmp \
    && install -m 0755 /tmp/uv-x86_64-unknown-linux-gnu/uv /tmp/uv-x86_64-unknown-linux-gnu/uvx /usr/local/bin/ \
    && rm -rf /tmp/uv.tar.gz /tmp/uv-x86_64-unknown-linux-gnu \
    && uv --version

# no compiler of any kind: a prebuilt build must not need one
RUN for tool in cc c++ gcc g++ clang clang++ emcc cmake make ninja; do \
        if command -v "$tool" >/dev/null 2>&1; then echo "runtime image has $tool" >&2; exit 1; fi; \
    done
