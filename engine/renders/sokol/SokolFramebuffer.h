// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolFramebuffer_h
#define SokolFramebuffer_h

#include "render/Render.h"
#include "math/Vector4.h"
#include "render/TextureRender.h"

#include "sokol_gfx.h"

namespace jasperengine{
    struct FramebufferDesc;

    class SokolFramebuffer{

    public:
        static constexpr int MAX_COLOR_ATTACHMENTS = 4;

    private:
        // [attachment][face]; only 2D framebuffers use MRT (single face), cube
        // framebuffers use a single color attachment across 6 faces.
        sg_view colorAttachmentViews[MAX_COLOR_ATTACHMENTS][6];
        sg_view depthAttachmentView;
        // MSAA (createFramebuffer(FramebufferDesc)): colorTexture[a] is attachment a's resolve image
        sg_view resolveAttachmentViews[MAX_COLOR_ATTACHMENTS];

        TextureRender colorTexture[MAX_COLOR_ATTACHMENTS];
        TextureRender depthTexture;
        TextureRender msaaColorTexture[MAX_COLOR_ATTACHMENTS];

        int numColorAttachments;
        int sampleCount;
        // false when the depth-stencil attachment view belongs to another framebuffer
        bool ownsDepthAttachment;

        // a multisampled target (createMultisampleFramebuffer): its colour and depth-stencil
        // images, which are never sampled, and the resolve views of the targets it resolved
        // into (by image, a few kept)
        sg_image msaaColorImage;
        sg_image msaaDepthImage;
        int msaaSamples;
        struct ResolveView{
            uint32_t image = SG_INVALID_ID;
            sg_view view = {SG_INVALID_ID};
        };
        static const int RESOLVE_VIEWS = 4;
        ResolveView resolveViews[RESOLVE_VIEWS];
        int nextResolveView;

        void initializeAttachments(size_t faces);

    public:
        SokolFramebuffer();
        SokolFramebuffer(const SokolFramebuffer& rhs);
        SokolFramebuffer& operator=(const SokolFramebuffer& rhs);

        // depth: a depth-stencil attachment (colour passes that draw meshes), depth only, or
        // none (a target only fullscreen effects draw into)
        bool createFramebuffer(TextureType textureType, int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap, FramebufferDepth depth,
                               ColorFormat colorFormat = ColorFormat::RGBA);
        // A multisampled colour (RGBA8) and depth-stencil target that is drawn into and
        // resolved, at the end of each pass, into the colour of a single-sampled target of the
        // same size (getResolved); its images are never sampled. False when the backend cannot
        // make it (samples: 2 or 4, see getMaxMsaaSamples)
        bool createMultisampleFramebuffer(int width, int height, int samples);
        int getMsaaSamples() const;
        // the attachments of a pass drawing into this multisampled target and resolving into
        // target's colour (a single-sampled 2D framebuffer of the same size)
        sg_attachments getResolved(SokolFramebuffer& target);
        // the highest sample count up to requested a multisampled RGBA8 + depth-stencil target
        // can have on this device (1: none)
        static int getMaxMsaaSamples(int requested);
        bool createDepthOnlyFramebuffer(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, bool shadowMap);
        // Multi render target variant (2D only). formats[i] selects the pixel format
        // for color attachment i; pass numColor==1 for the regular single-target case.
        bool createFramebufferMRT(int width, int height, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV, int numColor, const ColorFormat* formats, bool stencil);
        // a 2D framebuffer of that description (FramebufferRender::createFramebuffer(desc)):
        // a multisampled colour resolving into colorTexture[0] in every pass, a depth(-stencil)
        // attachment of its own or shared
        bool createFramebuffer(const FramebufferDesc& desc);
        void destroyFramebuffer();
        bool isCreated();

        int getNumColorAttachments() const;
        int getSampleCount() const;
        sg_view getDepthAttachmentView() const;

        TextureRender& getColorTexture();
        TextureRender& getColorTexture(int index);
        TextureRender& getDepthTexture();

        const void* getD3D11HandlerColorRTV() const;
        const void* getD3D11HandlerDSV() const;

        sg_attachments get(size_t face);
    };
}

#endif /* SokolFramebBuffer_h */
