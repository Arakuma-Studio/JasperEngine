// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CameraRender_h
#define CameraRender_h

#include "Engine.h"
#include "math/Rect.h"
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
        // the name of its passes (a string literal) in debuggers and in the frame stats' pass
        // list (JASPERENGINE_FRAME_STATS)
        void setLabel(const char* label);

        void startRenderPass(FramebufferRender* framebuffer, size_t face = 0);
        void startRenderPass(int width, int height);
        void startRenderPass();
        // a pass of an offscreen target that keeps its depth and stencil when it ends, so
        // resumeRenderPass can continue it after other passes (the scene-colour copy)
        void startSplitRenderPass(FramebufferRender* framebuffer);
        // continues such a pass: colour, depth and stencil loaded as they were left
        void resumeRenderPass(FramebufferRender* framebuffer);

        void applyViewport(Rect rect);
        void applyScissor(Rect rect);

        void endRenderPass();
    };
}


#endif /* CameraRender_h */
