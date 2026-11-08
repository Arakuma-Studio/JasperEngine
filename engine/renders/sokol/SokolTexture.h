// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolTexture_h
#define SokolTexture_h

#include "render/Render.h"
#include "texture/TextureFormat.h"
#include "sokol_gfx.h"
#include <memory>
#include <span>
#include <string>

namespace jasperengine{
    struct RenderFeatures;

    class SokolTexture{

    private:
        sg_image image;
        sg_sampler sampler;
        sg_view view;
        // held while `sampler` is the one every float data texture shares (createDynamicTexture)
        std::shared_ptr<void> sharedSampler;
        size_t streamBytes;   // RGBA8/R8 dynamic textures: the size every update must have (0 = not checked)
        bool aliasImage = false;   // the image is another texture's (createAliasView): never destroyed here
        // a terrain map's stream image (createDynamicMapTexture): what updateTexture takes
        bool dynamicMap = false;
        ColorFormat uploadFormat = ColorFormat::RGBA;
        // sokol takes one update of a stream image a frame (two terrains may share one map)
        uint64_t lastMapUpdateFrame = UINT64_MAX;

        sg_image_type getTextureType(TextureType textureType);
        sg_filter getFilter(TextureFilter textureFilter);
        sg_filter getFilterMipmap(TextureFilter textureFilter);
        sg_wrap getWrap(TextureWrap textureWrap);

        void createTextureView(const char* label);

        static const void* getVulkanView(sg_view view);
        static const void* getVulkanImage(sg_image image);

    public:
        // The image format and bytes per texel of a texture made from data (createTexture):
        // RGBA -> RGBA8, RED -> R8, RED16 -> R16 (uploaded as float where unorm16 isn't sampled),
        // and the float formats RGBA16F, RGBA32F, R32F, RG16F, R16F (half data as IEEE binary16,
        // float data as binary32, channels interleaved, rows tightly packed, top row first).
        // False for a format a texture can't be made from (SRGB8A8: render targets only)
        static bool getDataPixelFormat(ColorFormat colorFormat, sg_pixel_format& pixelFormat, size_t& bytesPerTexel);
        // Whether a texture of the format may be sampled with a linear filter: always for the 8-
        // and 16-bit unorm formats, per the device's RenderFeatures filter flags for the float
        // ones (RGBA32F/R32F are not filterable on GLES3/WebGL2 without OES_texture_float_linear)
        static bool isLinearFilterable(ColorFormat colorFormat, const RenderFeatures& features);

        SokolTexture();
        SokolTexture(const SokolTexture& rhs);
        SokolTexture& operator=(const SokolTexture& rhs);

        // An immutable texture from data (one block per face), level 0 only (mipmaps come as
        // levels, createTextureLevels). Float formats need data of exactly width * height *
        // bytesPerTexel per face and fall back to NEAREST (min, mag and mip) where the device
        // can't filter them linearly. False (with an error) for an unsupported format, a float
        // format the device can't sample, a size over the device limit or a data size that
        // doesn't match
        bool createTexture(
                    const std::string& label, int width, int height,
                    ColorFormat colorFormat, TextureType type, int numFaces, void* data[], size_t size[],
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // the levels as given, level 0 first, each holding every face (or array slice); the
        // sampler's mip filter applies when there are several
        bool createTextureLevels(
                    const std::string& label, TextureType type, GpuTextureFormat format,
                    int width, int height, int numFaces, std::span<const TextureLevel> levels,
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // the one sokol format of each cooked format (colour stays UNORM: shaders decode sRGB)
        static sg_pixel_format toSokol(GpuTextureFormat format, bool srgb);
        // sampled with linear filtering on this device (after sg_setup)
        static bool isFormatSupported(GpuTextureFormat format);

        // a 3D texture of width x height x depth RGBA8 texels (data: x fastest, then y, then
        // the depth slices), no mipmaps, one filter and wrap on every axis
        bool createTexture3D(
                    const std::string& label, int width, int height, int depth, const void* data, size_t size,
                    TextureFilter filter, TextureWrap wrap);

        // creates a cubemap with custom (pre-filtered) mipmap data,
        // data[level] must contain all 6 faces in one contiguous block
        bool createTextureCubeWithMips(
                    const std::string& label, int width,
                    ColorFormat colorFormat, int numMipmaps, void* data[], size_t size[],
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // sampleCount > 1: an MSAA attachment, never sampled (no texture view or sampler);
        // depthStencil: DEPTH_STENCIL instead of DEPTH (not sampled either); resolve: a 1-sample
        // MSAA resolve attachment, sampled like a colour attachment; numMipmaps: the levels of a
        // single-sampled colour attachment (each rendered by a pass into it, SokolCamera)
        bool createFramebufferTexture(
                    TextureType type, bool depth, bool shadowMap, int width, int height,
                    TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
                    ColorFormat colorFormat = ColorFormat::RGBA, int sampleCount = 1,
                    bool depthStencil = false, bool resolve = false, int numMipmaps = 1);

        bool createDynamicTexture(const std::string& label, int width, int height);
        bool createDynamicTexture(const std::string& label, int width, int height,
                    ColorFormat colorFormat, TextureFilter filter);
        // a second view of source's image with a nearest, non-comparison sampler: the raw
        // depths of a shadow atlas (sampled as unfilterable_float). It does not own the image,
        // which must outlive it.
        bool createAliasView(const SokolTexture& source, const char* label);
        // a view of any image (by id) at one mip level, every slice, with a nearest sampler:
        // the Frame Debugger's preview of a pass attachment. It does not own the image.
        bool createImageAliasView(uint32_t imageId, int mipLevel, const char* label);
        // RGBA8, linear filtering, clamped, rewritten whole by updateTexture at most once a frame
        // (the post-processing colour LUTs)
        bool createDynamicColorTexture(const std::string& label, int width, int height);
        // T1 DC9: a terrain map edited while it is drawn, rewritten whole by updateTexture at most
        // once a frame. RED16 heights are R16 where the device filters it, else R32F where that
        // filters, else R16F; RED is R8, RGBA is RGBA8. Linear, clamped, no mipmaps. Each update
        // takes width * height texels of getUploadFormat() (RED16, R32F, R16F, RED or RGBA)
        bool createDynamicMapTexture(const std::string& label, int width, int height, ColorFormat colorFormat);
        bool isDynamicMap() const { return dynamicMap; }
        ColorFormat getUploadFormat() const { return uploadFormat; }
        bool updateTexture(const void* data, size_t size);

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
