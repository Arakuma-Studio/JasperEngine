// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FramebufferRender_h
#define FramebufferRender_h

#include "Render.h"
#include "PipelineState.h"
#include "sokol/SokolFramebuffer.h"

#include <string>

namespace jasperengine{
    class FramebufferRender;

    // A 2D colour framebuffer with an optional depth(-stencil) attachment and MSAA
    // (createFramebuffer(const FramebufferDesc&)).
    struct FramebufferDesc {
        int width = 0, height = 0;
        ColorFormat color = ColorFormat::RGBA;
        DepthStencilFormat depth = DepthStencilFormat::DEPTH_STENCIL;
        int sampleCount = 1;                    // > 1: MSAA colour + 1-sample resolve texture
        TextureFilter minFilter = TextureFilter::LINEAR, magFilter = TextureFilter::LINEAR;
        TextureWrap wrapU = TextureWrap::CLAMP_TO_EDGE, wrapV = TextureWrap::CLAMP_TO_EDGE;
        const FramebufferRender* shareDepthStencil = nullptr; // reuse its depth-stencil attachment (same size and samples)
        // draw into its first colour image (one sample, same size; it must outlive this one): a
        // second set of attachments for that image, with this one's depth
        const FramebufferRender* shareColor = nullptr;
        // more colour attachments (MRT): colorCount in all, the second to the fourth of
        // moreColors' formats, each multisampled and resolved like the first under MSAA
        int colorCount = 1;
        ColorFormat moreColors[3] = {ColorFormat::RGBA, ColorFormat::RGBA, ColorFormat::RGBA};
        std::string label;
    };

    class JASPERENGINE_API FramebufferRender{

    public:
        //***Backend***
        SokolFramebuffer backend;
        //***
        
        FramebufferRender();
        FramebufferRender(const FramebufferRender& rhs);
        FramebufferRender& operator=(const FramebufferRender& rhs);
        virtual ~FramebufferRender();

        // The depth attachment is depth-stencil (what colour passes that draw meshes use, and
        // the PIP_RTT pipelines expect) unless shadowMap: shadow atlases stay depth only
        bool createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap);
        // stencil chooses the attachment: depth-stencil, or depth only for targets drawn
        // only by depth-format pipelines (PIP_DEPTH, PIP_GBUFFER)
        bool createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap, bool stencil);
        // any depth attachment, or none (FramebufferDepth::NONE: drawn only by PIP_EFFECT pipelines)
        bool createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap, FramebufferDepth depth,
                               ColorFormat colorFormat = ColorFormat::RGBA);
        // a multisampled colour + depth-stencil target, resolved into another target's colour
        // at the end of each pass drawing into it (CameraRender::setMultisample)
        bool createMultisampleFramebuffer(int width, int height, int samples);
        int getMsaaSamples() const;
        // the highest sample count up to requested the device has for such a target (1: none)
        static int getMaxMsaaSamples(int requested);
        bool createDepthOnlyFramebuffer(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap);
        bool createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats, bool stencil = true);
        // With sampleCount > 1 the colour attachment is multisampled and every pass resolves
        // it into getColorTexture() (the 1-sample resolve image). A shared depth-stencil stays
        // owned by its framebuffer, which must outlive this one
        bool createFramebuffer(const FramebufferDesc& desc);
        void destroyFramebuffer();
        bool isCreated();

        int getNumColorAttachments() const;
        int getSampleCount() const;

        TextureRender& getColorTexture();
        TextureRender& getColorAttachmentTexture(int index);
        TextureRender& getDepthTexture();

        const void* getD3D11HandlerColorRTV() const;
        const void* getD3D11HandlerDSV() const;
    };
}


#endif /* FramebufferRender_h */
