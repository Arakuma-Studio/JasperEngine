// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TextureRender_h
#define TextureRender_h

#include "Render.h"
#include "sokol/SokolTexture.h"

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

        // creates a cubemap with custom (pre-filtered) mipmap data,
        // data[level] must contain all 6 faces in one contiguous block
        bool createTextureCubeWithMips(
                const std::string& label, int width,
                ColorFormat colorFormat, int numMipmaps, void* data[], size_t size[],
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        // sampleCount > 1: MSAA attachment (not sampled); depthStencil: DEPTH_STENCIL format
        // (not sampled); resolve: the 1-sample resolve image of an MSAA colour attachment
        bool createFramebufferTexture(
                TextureType type, bool depth, bool shadowMap, int width, int height,
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
                ColorFormat colorFormat = ColorFormat::RGBA, int sampleCount = 1,
                bool depthStencil = false, bool resolve = false);

        // RGBA32F, NEAREST (bone matrices)
        bool createDynamicTexture(const std::string& label, int width, int height);
        // RGBA -> RGBA8, RED -> R8; stream update, CLAMP_TO_EDGE, no mipmaps, the given min/mag
        // filter. updateTexture then takes exactly width * height * 4 (or 1) bytes, rows top first
        bool createDynamicTexture(const std::string& label, int width, int height,
                                  ColorFormat format, TextureFilter filter);
        // at most once per frame (sokol); the next draw that samples the texture sees the data
        void updateTexture(const void* data, size_t size);

        void destroyTexture();

        uint32_t getGLHandler() const;
        const void* getMetalHandler() const;
        const void* getD3D11Handler() const;
        const void* getVulkanHandler() const;
        const void* getVulkanImageHandler() const;

        uint32_t getViewId() const;
        static bool isViewValid(uint32_t viewId);

        bool isCreated();
    };
}


#endif /* TextureRender_h */
