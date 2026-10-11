// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FramebufferRender_h
#define FramebufferRender_h

#include "Render.h"
#include "sokol/SokolFramebuffer.h"

namespace jasperengine{
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
        bool createDepthOnlyFramebuffer(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap);
        bool createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats, bool stencil = true);
        void destroyFramebuffer();
        bool isCreated();

        int getNumColorAttachments() const;

        TextureRender& getColorTexture();
        TextureRender& getColorAttachmentTexture(int index);
        TextureRender& getDepthTexture();

        const void* getD3D11HandlerColorRTV() const;
        const void* getD3D11HandlerDSV() const;
    };
}


#endif /* FramebufferRender_h */
