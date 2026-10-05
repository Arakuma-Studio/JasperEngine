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
        # Headless runs: virtual X server, Mesa software GL, screenshots.
        xvfb xauth x11-utils x11-apps imagemagick libgl1-mesa-dri mesa-utils dbus xdotool \
    # 22.04 ships CMake 3.22; the vendored glslang requires 3.27 or newer.
    && wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
        > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*

# The build runs as the host user; git refuses bind-mounted repos owned by
# someone else unless they are marked safe.
RUN git config --system --add safe.directory '*'

# The editor's file dialogs go through D-Bus, which refuses to start without a
# machine id; container images ship an empty one.
RUN dbus-uuidgen > /etc/machine-id

ENV LIBGL_ALWAYS_SOFTWARE=1
