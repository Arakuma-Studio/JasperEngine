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
        // the multisampled target the colour passes draw into, resolving into theirs (null: none)
        SokolFramebuffer* multisample = nullptr;

        // the attachments of a pass into framebuffer (through the multisampled target if set)
        sg_attachments attachmentsOf(FramebufferRender* framebuffer, size_t face);

    public:
        SokolCamera();
        SokolCamera(const SokolCamera& rhs);
        SokolCamera& operator=(const SokolCamera& rhs);
        virtual ~SokolCamera();

        void setClearColor(Vector4 clearColor);
        void setClearDepth(float clearDepth = 1.0f);
        void setLoadActionLoad();
        // Draws the passes started from now on into a multisampled target (FramebufferRender::
        // createMultisampleFramebuffer, same size) that resolves into the framebuffer they name at
        // their end; its samples are never stored (resolved in the tile on tilers) unless the pass
        // is split. Null: passes draw into their framebuffer.
        void setMultisample(FramebufferRender* msaa);
        // the pass's name (a string literal) in debuggers and the frame stats' pass list
        void setLabel(const char* label);

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
