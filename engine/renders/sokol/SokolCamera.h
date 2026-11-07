// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolCamera_h
#define SokolCamera_h

#include "math/Rect.h"
#include "math/Vector4.h"
#include "render/FramebufferRender.h"
#include "render/PipelineState.h"
#include "render/TextureRender.h"

#include "sokol_gfx.h"

namespace jasperengine{
    class SokolCamera{

    private:
        sg_pass pass;
        // the multisampled target the colour passes draw into, resolving into theirs (null: none)
        SokolFramebuffer* multisample = nullptr;

        // the attachments of a pass into framebuffer (through the multisampled target if set)
        sg_attachments attachmentsOf(FramebufferRender* framebuffer, size_t face);
        // the colour attachment view of a pass into a texture, released when the pass ends
        sg_view textureView;

    public:
        SokolCamera();
        SokolCamera(const SokolCamera& rhs);
        SokolCamera& operator=(const SokolCamera& rhs);
        virtual ~SokolCamera();

        void setClearColor(Vector4 clearColor);
        void setClearDepth(float clearDepth = 1.0f);
        void setLoadActionLoad();
        void setColorActions(LoadAction load, StoreAction store, Vector4 clearColor);
        void setColorActions(int attachment, LoadAction load, StoreAction store, Vector4 clearColor);
        void setDepthStencilActions(LoadAction load, StoreAction store, float clearDepth, uint8_t clearStencil);
        // Draws the passes started from now on into a multisampled target (FramebufferRender::
        // createMultisampleFramebuffer, same size) that resolves into the framebuffer they name at
        // their end; its samples are never stored (resolved in the tile on tilers) unless the pass
        // is split. Null: passes draw into their framebuffer.
        void setMultisample(FramebufferRender* msaa);
        // the pass's name (a string literal) in debuggers and the frame stats' pass list
        void setLabel(const char* label);

        void startRenderPass(FramebufferRender* framebuffer, size_t face);
        // into one face (or slice) and mip level of a colour attachment texture
        // (TextureRender::createFramebufferTexture), with no depth: PIP_RTT_COLORONLY pipelines
        void startRenderPass(TextureRender* texture, size_t face, int mip);
        void startRenderPass(int width, int height);
        void startRenderPass();
        // the same, keeping depth and stencil past the pass's end (a pass split in two:
        // resumeRenderPass continues it after other passes ran)
        void startSplitRenderPass(FramebufferRender* framebuffer);
        // continues a pass on the same target: colour, depth and stencil as they were left;
        // split: stored at its end as startSplitRenderPass stores them (another resume follows)
        void resumeRenderPass(FramebufferRender* framebuffer, bool split = false);

        void applyViewport(Rect rect);
        void applyScissor(Rect rect);
        void applyViewportTopLeft(int x, int y, int width, int height);
        void applyScissorTopLeft(int x, int y, int width, int height);

        void endRenderPass();
    };
}

#endif //SokolCamera_h
