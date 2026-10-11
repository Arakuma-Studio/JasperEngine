// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolCamera_h
#define SokolCamera_h

#include "math/Rect.h"
#include "math/Vector4.h"
#include "render/FramebufferRender.h"

#include "sokol_gfx.h"

namespace jasperengine{
    class SokolCamera{

    private:
        sg_pass pass;

    public:
        SokolCamera();
        SokolCamera(const SokolCamera& rhs);
        SokolCamera& operator=(const SokolCamera& rhs);
        virtual ~SokolCamera();

        void setClearColor(Vector4 clearColor);
        void setClearDepth(float clearDepth = 1.0f);
        void setLoadActionLoad();

        void startRenderPass(FramebufferRender* framebuffer, size_t face);
        void startRenderPass(int width, int height);
        void startRenderPass();
        // the same, keeping depth and stencil past the pass's end (a pass split in two:
        // resumeRenderPass continues it after other passes ran)
        void startSplitRenderPass(FramebufferRender* framebuffer);
        // continues a pass on the same target: colour, depth and stencil as they were left
        void resumeRenderPass(FramebufferRender* framebuffer);

        void applyViewport(Rect rect);
        void applyScissor(Rect rect);

        void endRenderPass();
    };
}

#endif //SokolCamera_h
