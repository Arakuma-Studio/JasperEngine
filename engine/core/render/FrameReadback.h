// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Synchronous GPU readback of a render target's colour (docs/specs/S30-shipping.md R20): the
// shipped game's --screenshot (the engine's composite framebuffer of the capture frame) and the
// GPU tests' goldens. glcore (glGetTexImage), gles3/WebGL2 (a framebuffer and glReadPixels),
// Vulkan (a staging copy; colour attachments carry TRANSFER_SRC), D3D11 (a staging texture) and
// Metal (a blit on sokol's queue, installed by the Apple platforms, which compile Objective-C).
// Main thread only, after the frame that drew the target was committed.

#include "Export.h"
#include "render/TextureRender.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    class Framebuffer;

    class JASPERENGINE_API FrameReadback {
    public:
        // width * height * bytesPerPixel bytes, rows top first (offscreen targets are top-left
        // origin on every backend); bytesPerPixel 4 (RGBA8), 1 (R8), 8 (RGBA16F: IEEE halves) or
        // 16 (RGBA32F); the size must match the texture's format. GLES3 / WebGL2 read float
        // targets only with EXT_color_buffer_float (false with "float readback needs
        // EXT_color_buffer_float" otherwise). False with a message
        static bool readTexture(TextureRender& texture, int width, int height, int bytesPerPixel,
                                std::vector<uint8_t>& pixels, std::string* error = nullptr);
        // the RGBA8 colour of a created framebuffer
        static bool readFramebuffer(Framebuffer& framebuffer, std::vector<uint8_t>& rgba, std::string* error = nullptr);
        // waits until the GPU has done everything submitted (glFinish, vkQueueWaitIdle)
        static bool finishGpu();
        // a fence after everything submitted so far, and whether the GPU has passed the last one,
        // without waiting (main thread). The GL family (glFenceSync, flushed so it starts; not
        // Windows' GL 1.1 headers); elsewhere, and with no fence, it has always passed
        static void fenceGpu();
        static bool gpuFencePassed();

        // Metal's reader (Objective-C, platform/apple/MetalReadback.h installs it)
        using PlatformReader = bool (*)(TextureRender& texture, int width, int height, int bytesPerPixel,
                                        std::vector<uint8_t>& pixels, std::string* error);
        static void setPlatformReader(PlatformReader reader);
    };

}
