// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolCamera_h
#define SokolCamera_h

#include "math/Rect.h"
#include "math/Vector4.h"
#include "render/FramebufferRender.h"
#include "render/PipelineState.h"

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
        void setColorActions(LoadAction load, StoreAction store, Vector4 clearColor);
        void setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth, uint8_t clearStencil);

        void startRenderPass(FramebufferRender* framebuffer, size_t face);
        void startRenderPass(int width, int height);
        void startRenderPass();

        void applyViewport(Rect rect);
        void applyScissor(Rect rect);
        void applyViewportTopLeft(int x, int y, int width, int height);
        void applyScissorTopLeft(int x, int y, int width, int height);

        void endRenderPass();
    };
}

#endif //SokolCamera_h
