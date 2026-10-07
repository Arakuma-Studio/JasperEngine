// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "render/FramebufferRender.h"

namespace jasperengine{

    class JASPERENGINE_API Framebuffer{

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

            unsigned long version; // increment every creation

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
            void destroy();
            bool isCreated();

            FramebufferRender& getRender();
            unsigned long getVersion();

            // configure multiple color attachments; formats may be null (defaults to RGBA)
            void setColorAttachments(int count, const ColorFormat* formats = nullptr);
            int getNumColorAttachments() const;

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