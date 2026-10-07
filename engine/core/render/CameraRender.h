// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CameraRender_h
#define CameraRender_h

#include "Engine.h"
#include "math/Rect.h"
#include "render/PipelineState.h"
#include "sokol/SokolCamera.h"

namespace jasperengine{
    class JASPERENGINE_API CameraRender{
    public:
        //***Backend***
        SokolCamera backend;
        //***

        CameraRender();
        CameraRender(const CameraRender& rhs);
        CameraRender& operator=(const CameraRender& rhs);
        virtual ~CameraRender();

        void setClearColor(Vector4 clearColor);
        void setClearDepth(float clearDepth = 1.0f);
        void setLoadActionLoad();
        // pass actions of the next startRenderPass. setDepthStencilActions writes depth and
        // stencil alike (Metal takes the stencil store action from depth)
        void setColorActions(LoadAction load, StoreAction store, Vector4 clearColor = Vector4(0, 0, 0, 0));
        void setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth = 1.0f, uint8_t clearStencil = 0);

        void startRenderPass(FramebufferRender* framebuffer, size_t face = 0);
        void startRenderPass(int width, int height);
        void startRenderPass();

        void applyViewport(Rect rect);
        void applyScissor(Rect rect);
        // offscreen targets, top-left origin on every backend (row 0 is the image's first row)
        void applyScissorTopLeft(int x, int y, int width, int height);
        void applyViewportTopLeft(int x, int y, int width, int height);

        void endRenderPass();
    };
}


#endif /* CameraRender_h */
