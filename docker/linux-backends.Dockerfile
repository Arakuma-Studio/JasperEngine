# The Linux build image plus what running games on the other graphics backends
# headless needs (tools/smoke-test.sh with SMOKE_BACKEND=vulkan or gles3): Mesa's
# software Vulkan driver (lavapipe, which implements the VK_EXT_descriptor_buffer
# sokol_gfx needs) and the OpenGL ES headers and library (Mesa's llvmpipe gives
# GLX an ES 3.2 context). Build docker/linux.Dockerfile as jasperengine/linux-build
# first. BASE names that image: tools/build.sh passes its hash tag, CI its GHCR image (images.yml).
ARG BASE=jasperengine/linux-build
FROM ${BASE}

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        mesa-vulkan-drivers vulkan-tools libgles-dev \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 22.04's Vulkan headers (1.3.204) predate VK_EXT_descriptor_buffer: newer ones in
# /usr/local, which FindVulkan searches first (the loader stays the system one)
ARG VULKAN_HEADERS=1.3.296
RUN wget -qO- https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v${VULKAN_HEADERS}.tar.gz \
        | tar -xz -C /tmp \
    && cp -r /tmp/Vulkan-Headers-${VULKAN_HEADERS}/include/vulkan /tmp/Vulkan-Headers-${VULKAN_HEADERS}/include/vk_video /usr/local/include/ \
    && rm -rf /tmp/Vulkan-Headers-${VULKAN_HEADERS}

# Only lavapipe: the hardware drivers' manifests would be tried first
ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
