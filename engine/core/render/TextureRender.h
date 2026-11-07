// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TextureRender_h
#define TextureRender_h

#include "Render.h"
#include "texture/TextureFormat.h"
#include "sokol/SokolTexture.h"

#include <span>

namespace jasperengine {

    class JASPERENGINE_API TextureRender{

    public:
        //***Backend***
        SokolTexture backend;
        //***

        TextureRender();
        TextureRender(const TextureRender& rhs);
        TextureRender& operator=(const TextureRender& rhs);

        virtual ~TextureRender();

        bool createTexture(
                const std::string& label, int width, int height,
                ColorFormat colorFormat, TextureType type, int numFaces, void* data[], size_t size[],
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // levels as given (level 0 first, each with every face or array slice contiguous), in a
        // cooked or uncompressed GPU format: the one upload path of file textures (spec S14 D9)
        bool createTextureLevels(const std::string& label, TextureType type, GpuTextureFormat format,
                                 int width, int height, int numFaces, std::span<const TextureLevel> levels,
                                 TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // a 3D texture: width x height x depth RGBA8 texels (x fastest, then y, then the
        // slices), linear or nearest and repeat or clamp on every axis, no mipmaps
        bool createTexture3D(const std::string& label, int width, int height, int depth, const void* data, size_t size,
                             TextureFilter filter, TextureWrap wrap);

        // creates a cubemap with custom (pre-filtered) mipmap data,
        // data[level] must contain all 6 faces in one contiguous block
        bool createTextureCubeWithMips(
                const std::string& label, int width,
                ColorFormat colorFormat, int numMipmaps, void* data[], size_t size[],
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // sampleCount > 1: MSAA attachment (not sampled); depthStencil: DEPTH_STENCIL format
        // (not sampled); resolve: the 1-sample resolve image of an MSAA colour attachment;
        // numMipmaps: the levels of a colour attachment, each drawn by its own pass
        // (CameraRender::startRenderPass(TextureRender*, face, mip))
        bool createFramebufferTexture(
                TextureType type, bool depth, bool shadowMap, int width, int height,
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
                ColorFormat colorFormat = ColorFormat::RGBA, int sampleCount = 1,
                bool depthStencil = false, bool resolve = false, int numMipmaps = 1);

        // RGBA32F, NEAREST (bone matrices)
        bool createDynamicTexture(const std::string& label, int width, int height);
        // RGBA -> RGBA8, RED -> R8, RGBA16F (half floats); stream update, no mipmaps, the given
        // min/mag filter and wrap (a toroidal map repeats). updateTexture then takes exactly
        // width * height * 4 (1, 8) bytes, rows top first
        bool createDynamicTexture(const std::string& label, int width, int height,
                                  ColorFormat format, TextureFilter filter, TextureWrap wrap = TextureWrap::CLAMP_TO_EDGE);
        // RGBA8, linear, clamped, rewritten whole by updateTexture (width * height * 4 bytes)
        // at most once a frame (the post-processing colour LUTs)
        bool createDynamicColorTexture(const std::string& label, int width, int height);
        // at most once per frame (sokol); the next draw that samples the texture sees the data
        // false when not uploaded: past the frame's stream staging buffer (try again next
        // frame), a wrong size, no data
        bool updateTexture(const void* data, size_t size);

        // a second view of source's image with a nearest, non-comparison sampler (the raw depths
        // of a shadow atlas); source must outlive it
        bool createAliasView(const TextureRender& source, const char* label);

        void destroyTexture();

        uint32_t getGLHandler() const;
        const void* getMetalHandler() const;
        const void* getD3D11Handler() const;
        const void* getVulkanHandler() const;
        const void* getVulkanImageHandler() const;

        uint32_t getViewId() const;
        // the size of the image (0 when there is none)
        int getWidth() const;
        int getHeight() const;
        static bool isViewValid(uint32_t viewId);

        bool isCreated();

        // whether the image it was made from has transparent texels (TextureData::isTransparent),
        // for a Texture that found it in TexturePool and so never loaded the image itself
        void setTransparent(bool transparent);
        bool isTransparent() const;

        // what createTextureLevels uploaded (RGBA8 / 1 level for the other creators)
        GpuTextureFormat getGpuFormat() const;
        int getLevelCount() const;
        uint64_t getGpuBytes() const;

        // TexturePool: its image's pixels were replaced and it was not made again yet (it still
        // draws the old ones); and how many times it was made again in place, which takes the old
        // view away from the bindings holding it (Texture::isRenderOutdated)
        void setStale(bool stale) { this->stale = stale; }
        bool isStale() const { return stale; }
        void noteRebuilt() { revision++; }
        uint32_t getRevision() const { return revision; }

    private:
        bool stale = false;
        uint32_t revision = 0;
        bool transparent = false;
        GpuTextureFormat gpuFormat = GpuTextureFormat::RGBA8;
        int levelCount = 1;
        uint64_t gpuBytes = 0;
    };
}


#endif /* TextureRender_h */
