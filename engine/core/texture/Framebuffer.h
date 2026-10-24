// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "render/FramebufferRender.h"

#include <memory>

namespace jasperengine{

    // A framebuffer owned through a std::shared_ptr (make_shared) is shared by the Textures
    // showing it (Texture::setFramebuffer takes a reference through weak_from_this()): it
    // outlives its owner while a texture links it (FluidSimulator's output). Its GPU side is
    // still freed by its owner (destroy()); a texture of a destroyed framebuffer draws its
    // fallback (Texture::getRender). Others (members, `new`) are linked by raw pointer.
    class JASPERENGINE_API Framebuffer : public std::enable_shared_from_this<Framebuffer> {

        private:
            FramebufferRender render;
            unsigned int width;
            unsigned int height;
            TextureFilter minFilter;
            TextureFilter magFilter;
            TextureWrap wrapU;
            TextureWrap wrapV;

            // when > 1 the framebuffer is created as a 2D multi render target (MRT)
            int numColorAttachments;
            ColorFormat colorFormats[SokolFramebuffer::MAX_COLOR_ATTACHMENTS];
            // a depth-stencil attachment (default), depth only, or none
            FramebufferDepth depth;

            unsigned long version; // incremented when its image is made, replaced or freed

            // set by create(const FramebufferDesc&): create() builds from it
            FramebufferDesc desc;
            bool hasDesc;

        public:
            Framebuffer();
            virtual ~Framebuffer();

            Framebuffer(const Framebuffer& rhs);
            Framebuffer& operator=(const Framebuffer& rhs);

            void create();
            // a framebuffer of that description (MSAA, depth-stencil, shared depth-stencil);
            // later create() calls rebuild the same. False when it could not be created
            bool create(const FramebufferDesc& desc);
            // frees the GPU side (a new version when there was one: textures rebind)
            void destroy();
            bool isCreated();

            FramebufferRender& getRender();
            unsigned long getVersion();

            // configure multiple color attachments; formats may be null (defaults to RGBA)
            void setColorAttachments(int count, const ColorFormat* formats = nullptr);
            int getNumColorAttachments() const;

            // Colour passes drawing meshes (PIP_RTT pipelines) need the default depth-stencil
            // attachment; a target only depth-format pipelines draw into (the SSAO depth
            // pre-pass, the G-buffer) can be depth only. Applies on the next create().
            void setDepthStencil(bool depthStencil);
            bool isDepthStencil() const;
            // DEPTH for a target only fullscreen effects (PIP_EFFECT pipelines) draw into: the
            // SSAO and SSR targets, their blurs, post-process buffer 1. Applies on the next
            // create().
            void setDepthAttachment(FramebufferDepth depth);
            FramebufferDepth getDepthAttachment() const;

            void setWidth(unsigned int width);
            unsigned int getWidth() const;

            void setHeight(unsigned int height);
            unsigned int getHeight() const;

            void setMinFilter(TextureFilter filter);
            TextureFilter getMinFilter() const;

            void setMagFilter(TextureFilter filter);
            TextureFilter getMagFilter() const;

            void setWrapU(TextureWrap wrapU);
            TextureWrap getWrapU() const;

            void setWrapV(TextureWrap wrapV);
            TextureWrap getWrapV() const;
    };
}

#endif //FRAMEBUFFER_H