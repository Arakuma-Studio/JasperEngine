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

        bool createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap);
        bool createDepthOnlyFramebuffer(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap);
        bool createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats);
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
