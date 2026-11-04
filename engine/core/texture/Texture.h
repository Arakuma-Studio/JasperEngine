// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTURE_H
#define TEXTURE_H

#include "render/TextureRender.h"
#include "texture/Framebuffer.h"
#include "texture/TextureData.h"
#include "pool/TexturePool.h"
#include "pool/TextureDataPool.h"
#include "asset/Guid.h"
#include <string>
#include <memory>

namespace jasperengine{

    class JASPERENGINE_API Texture{
        private:
            static bool hasTexturePixels(const std::shared_ptr<std::array<TextureData,6>>& data, size_t numFaces);
            static std::string buildCubeTextureId(const std::string paths[6]);

            std::string buildPathTextureId() const;

            std::shared_ptr<TextureRender> render = NULL;
            // the TextureRender::getRevision getRender last gave
            uint32_t renderRevision = 0;
            Framebuffer* framebuffer;
            // keeps a shared framebuffer alive while this texture shows it (one made with
            // std::make_shared, see Framebuffer); empty for the others
            std::shared_ptr<Framebuffer> framebufferOwner;
            unsigned long lastFramebufferVersion = 0;

            TextureType type;
            std::string id;

            std::string paths[6];
            // the asset GUID of each source path (the editor's reference identity, S04)
            Guid pathGuids[6];
            // mutable: a const accessor takes a hot-reloaded entry (refreshFromPool)
            mutable std::shared_ptr<std::array<TextureData,6>> data = NULL;
            // the pool generation `data` is from, and the replace epoch last checked (S05 R13)
            mutable uint32_t dataGeneration = 0;
            mutable uint32_t checkedEpoch = 0;

            size_t numFaces;

            bool loadFromPath;
            bool releaseDataAfterLoad;
            bool needLoad;

            //render properties
            TextureFilter minFilter;
            TextureFilter magFilter;
            TextureWrap wrapU;
            TextureWrap wrapV;

            // rasterization scale for SVG sources (1.0 = natural size); ignored for rasters
            float svgScale;

            // see TextureData::fixAlphaBorder, only for files
            TextureAlphaBorder alphaBorder;
            // what AUTO means here, set by the system that draws the texture
            bool alphaBorderAuto;

            void reloadAlphaBorder(bool wasFixed);
            // after a sampler change: lets go of the texture of the previous pool key
            void releaseSamplerRender(const std::string& previousRenderId);
            // Takes the pool's entry when it was replaced since `data` was taken: O(1), and
            // one atomic load while nothing was replaced
            void refreshFromPool() const;
            void adoptPoolData(std::shared_ptr<std::array<TextureData,6>> pooled) const;

        public:
            Texture();
            Texture(const std::string& path);
            Texture(const std::string& id, TextureData data);
            Texture(Framebuffer* framebuffer);

            void setPath(const std::string& path);
            void setData(const std::string& id, TextureData data);
            void setId(const std::string& id);
            // pixels a job makes under the id (TextureDataPool::setGenerator: a lobe map packed
            // from two glTF images), made when the texture is first drawn
            void setGenerated(const std::string& id);
            void setCubeMap(const std::string& path);
            void setCubePath(size_t index, const std::string& path);

            void setCubePaths(const std::string& front, const std::string& back,
                            const std::string& left, const std::string& right,
                            const std::string& up, const std::string& down);

            void setCubeDatas(const std::string& id, TextureData front, TextureData back, TextureData left, TextureData right, TextureData up, TextureData down);
            void setFramebuffer(Framebuffer* framebuffer);

            Texture(const Texture& rhs);
            Texture& operator=(const Texture& rhs);

            bool operator == ( const Texture& rhs ) const;
            bool operator != ( const Texture& rhs ) const;

            virtual ~Texture();

            TextureLoadResult load();
            // Re-arm a file-backed texture after an asynchronous or synchronous
            // load failure. The next load() call starts a fresh attempt.
            void retryLoad();
            void destroy();
            void invalidateRender();

            // a framebuffer that was made and is gone draws the destroyed-framebuffer fallback
            // (setDestroyedFramebufferFallback: RenderSystem's transparent black), else
            // fallBackTexture; with no fallBackTexture, its texture as it is
            TextureRender* getRender(TextureRender* fallBackTexture = NULL);
            // How many times getRender gave its fallback for a Texture that names an image (a
            // stand-in: no pixels to make its GPU texture of yet, nothing loading them) on the
            // calling thread. A renderer reads it around a material's binding: one that took a
            // stand-in binds again once an image can be made (TexturePool::getReadyEpoch).
            static uint32_t getStandInCount();
            // getRender's answers on this thread for an image still loading: a draw that got one
            // showed nothing of that image (a frame that must show it looks at this)
            static uint32_t getLoadingAnswerCount();
            // Its GPU texture has new pixels to show (stale), or was made again in place since this
            // Texture took it: a binding made of it is out of date (TexturePool::getRebindEpoch)
            bool isRenderOutdated() const;
            static void setDestroyedFramebufferFallback(TextureRender* texture);
            std::string getPath(size_t index = 0) const;
            // The asset GUID of a source path: the editor resolves texture references by it
            // and keeps the path as a hint. A new path in a slot drops its GUID.
            const Guid& getPathGuid(size_t index = 0) const;
            void setPathGuid(size_t index, const Guid& guid);
            TextureData& getData(size_t index = 0) const;
            // True when the backing data array exists, so getData() can be dereferenced.
            // A present-but-unloaded or load-failed texture (empty file, missing path)
            // keeps a null data pointer even though it is not empty().
            bool hasData() const;
            const std::string& getId() const;
            // the TexturePool key: the id plus a non-default sampler state
            std::string getRenderId() const;

            size_t getNumFaces() const;
            TextureType getType() const;
            bool isCubeMap() const;

            // the source's (logical) size: UI and sprite layout never change with the GPU image
            unsigned int getWidth() const;
            unsigned int getHeight() const;
            bool isTransparent() const;

            // the GPU image (spec S14 R28): block-compressed, its format ("BC7", "RGBA8") and
            // its level 0 after maxSize, the mip limit and the memory budget
            bool isCompressed() const;
            std::string getGpuFormat() const;
            unsigned int getStoredWidth() const;
            unsigned int getStoredHeight() const;

            void setReleaseDataAfterLoad(bool releaseDataAfterLoad);
            bool isReleaseDataAfterLoad() const;

            void releaseData();

            // Names no image. A Texture of a file whose load failed is empty; one naming a pooled
            // or generated image by id (a model's embedded image) is not, with or without its
            // pixels or GPU texture at the moment: a variant chosen while they were away
            // samples it once they are back.
            bool empty() const;
            Framebuffer* getFramebuffer() const;
            bool isFramebuffer() const;
            bool isFramebufferOutdated() const;

            void setMinFilter(TextureFilter filter);
            TextureFilter getMinFilter() const;

            void setMagFilter(TextureFilter filter);
            TextureFilter getMagFilter() const;

            void setWrapU(TextureWrap wrapU);
            TextureWrap getWrapU() const;

            void setWrapV(TextureWrap wrapV);
            TextureWrap getWrapV() const;

            void setSvgScale(float scale);
            float getSvgScale() const;

            void setAlphaBorder(TextureAlphaBorder alphaBorder);
            TextureAlphaBorder getAlphaBorder() const;

            void setAlphaBorderAuto(bool fix);
            bool isAlphaBorderFixed() const;
    };
}

#endif //TEXTURE_H
