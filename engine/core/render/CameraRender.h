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
        // the same for another colour attachment of an MRT target (attachment 0 is the one above)
        void setColorActions(int attachment, LoadAction load, StoreAction store, Vector4 clearColor);
        void setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth = 1.0f, uint8_t clearStencil = 0);
        // the name of its passes (a string literal) in debuggers and in the frame stats' pass
        // list (JASPERENGINE_FRAME_STATS)
        void setLabel(const char* label);
        // the offscreen passes started from now on draw into this multisampled target and
        // resolve into the framebuffer they are given (null: none); see SokolCamera
        void setMultisample(FramebufferRender* msaa);

        void startRenderPass(FramebufferRender* framebuffer, size_t face = 0);
        // into one face and mip level of a colour attachment texture, with no depth
        // (PIP_RTT_COLORONLY pipelines)
        void startRenderPass(TextureRender* texture, size_t face, int mip);
        void startRenderPass(int width, int height);
        void startRenderPass();
        // a pass of an offscreen target that keeps its depth and stencil when it ends, so
        // resumeRenderPass can continue it after other passes (the scene-colour copy)
        void startSplitRenderPass(FramebufferRender* framebuffer);
        // continues such a pass: colour, depth and stencil loaded as they were left
        void resumeRenderPass(FramebufferRender* framebuffer);

        void applyViewport(Rect rect);
        void applyScissor(Rect rect);
        // offscreen targets, top-left origin on every backend (row 0 is the image's first row)
        void applyScissorTopLeft(int x, int y, int width, int height);
        void applyViewportTopLeft(int x, int y, int width, int height);

        void endRenderPass();
    };
}


#endif /* CameraRender_h */
