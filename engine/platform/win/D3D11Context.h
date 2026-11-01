// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Direct3D 11 device and swapchain for the Win32 backend. Shaped like
// VulkanContext so JasperEngineWin drives every graphics backend the same way.

#ifndef D3D11Context_h
#define D3D11Context_h

#include "System.h"

namespace jasperengine {

    struct D3D11ContextConfig {
        // Present's sync interval: 0 = no vsync, N = every Nth vblank (Engine.vSyncCount)
        int swapInterval = 1;
    };

    // One window and one device per process, so the state is static like
    // WindowWin's and the System implementation can reach it directly.
    class D3D11Context {
    public:

        // Must run before Engine::systemViewLoaded(), where sokol reads the environment
        static bool create(const D3D11ContextConfig& config);
        static void destroy();

        // Resizes the swapchain to the window when it changed. Frames with
        // nothing to draw into (a minimized window) leave the swapchain invalid
        // and sokol skips the pass.
        static void beginFrame();
        static void endFrame();

        // Engine.vSyncCount: 0..4, used from the next present
        static void setSwapInterval(int interval);

        static sg_environment environment();
        static sg_swapchain swapchain();
    };

}

#endif /* D3D11Context_h */
