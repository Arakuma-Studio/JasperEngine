// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolTexture_h
#define SokolTexture_h

#include "render/Render.h"
#include "sokol_gfx.h"
#include <string>

namespace jasperengine{
    class SokolTexture{

    private:
        sg_image image;
        sg_sampler sampler;
        sg_view view;
        size_t streamBytes;
        bool aliasImage = false;   // the image is another texture's (createAliasView): never destroyed here   // RGBA8/R8 dynamic textures: the size every update must have (0 = not checked)

        sg_image_type getTextureType(TextureType textureType);
        sg_filter getFilter(TextureFilter textureFilter);
        sg_filter getFilterMipmap(TextureFilter textureFilter);
        sg_wrap getWrap(TextureWrap textureWrap);

        sg_image generateMipmaps(const sg_image_desc* desc_);
        void createTextureView(const char* label);

        static const void* getVulkanView(sg_view view);
        static const void* getVulkanImage(sg_image image);

    public:
        SokolTexture();
        SokolTexture(const SokolTexture& rhs);
        SokolTexture& operator=(const SokolTexture& rhs);

        bool createTexture(
                    const std::string& label, int width, int height,
                    ColorFormat colorFormat, TextureType type, int numFaces, void* data[], size_t size[],
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // creates a cubemap with custom (pre-filtered) mipmap data,
        // data[level] must contain all 6 faces in one contiguous block
        bool createTextureCubeWithMips(
                    const std::string& label, int width,
                    ColorFormat colorFormat, int numMipmaps, void* data[], size_t size[],
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // sampleCount > 1: an MSAA attachment, never sampled (no texture view or sampler);
        // depthStencil: DEPTH_STENCIL instead of DEPTH (not sampled either); resolve: a 1-sample
        // MSAA resolve attachment, sampled like a colour attachment
        bool createFramebufferTexture(
                    TextureType type, bool depth, bool shadowMap, int width, int height,
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
                    ColorFormat colorFormat = ColorFormat::RGBA, int sampleCount = 1,
                    bool depthStencil = false, bool resolve = false);

        bool createDynamicTexture(const std::string& label, int width, int height);
        bool createDynamicTexture(const std::string& label, int width, int height,
                    ColorFormat colorFormat, TextureFilter filter);
        // a second view of source's image with a nearest, non-comparison sampler: the raw
        // depths of a shadow atlas (sampled as unfilterable_float). It does not own the image,
        // which must outlive it.
        bool createAliasView(const SokolTexture& source, const char* label);
        // RGBA8, linear filtering, clamped, rewritten whole by updateTexture at most once a frame
        // (the post-processing colour LUTs)
        bool createDynamicColorTexture(const std::string& label, int width, int height);
        void updateTexture(const void* data, size_t size);

        void destroyTexture();

        uint32_t getGLHandler() const;
        const void* getMetalHandler() const;
        const void* getD3D11Handler() const;
        const void* getVulkanHandler() const;
        const void* getVulkanImageHandler() const;

        uint32_t getViewId() const;
        int getWidth() const;
        int getHeight() const;
        static bool isViewValid(uint32_t viewId);

        bool isCreated();

        sg_image get();
        sg_sampler getSampler();
        sg_view getView();
    };
}

#endif /* SokolTexture_h */
