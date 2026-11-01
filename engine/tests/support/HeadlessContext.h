// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_HEADLESS_CONTEXT_H
#define JASPER_TEST_HEADLESS_CONTEXT_H

// No X11 or RmlUi headers here: X11's None/Bool macros clash with RmlUi enums

namespace jasperengine { namespace test {

    // The graphics device of the test binary. Linux: an X display (Xvfb), an unmapped window
    // and a GLX 4.1 core context (glcore) or a VulkanContext with an Xlib surface (vulkan).
    // macOS: the system default Metal device, without a window (HeadlessContextMetal.mm).
    // Nothing is ever presented: frames render into an engine Framebuffer.
    class HeadlessContext {
    public:
        static bool create(int width, int height);
        static void destroy();
        static bool isCreated();
        static const char* getBackendName();   // "glcore", "vulkan" or "metal"
        // the device the frames render on: GL_RENDERER, the Vulkan or Metal device's name
        // ("llvmpipe (LLVM ...)" for Mesa's llvmpipe and lavapipe, the test image's); empty
        // before create
        static const char* getRendererName();
#if defined(SOKOL_METAL)
        // the id<MTLDevice> sokol is set up with (TestSystem::getSokolEnvironment)
        static const void* getMetalDevice();
#endif
    };

} }

#endif //JASPER_TEST_HEADLESS_CONTEXT_H
