# Headless Xorg for real-GPU GL runs (tools/gpu/xorg-headless.sh). Only the X server: the NVIDIA
# Xorg driver and GLX module are bind-mounted read-only from the host at run time (they must match
# the host's kernel driver), and the build image stays the CI one.
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends xserver-xorg-core x11-xserver-utils \
    && rm -rf /var/lib/apt/lists/*
