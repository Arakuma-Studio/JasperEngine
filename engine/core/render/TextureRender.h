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

        bool createFramebufferTexture(
                TextureType type, bool depth, bool shadowMap, int width, int height,
                TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
                ColorFormat colorFormat = ColorFormat::RGBA, bool stencil = false);

        bool createDynamicTexture(const std::string& label, int width, int height);
        // RGBA8, linear, clamped, rewritten by updateTexture at most once a frame
        bool createDynamicColorTexture(const std::string& label, int width, int height);
        void updateTexture(const void* data, size_t size);

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

    private:
        bool transparent = false;
    };
}


#endif /* TextureRender_h */
