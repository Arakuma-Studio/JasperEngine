// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef SPRITE_INSTANCE_BATCH_H
#define SPRITE_INSTANCE_BATCH_H

// Contract (docs/specs/S52-sprite-instancing.md §6.5, D7)
// - Purpose: instances of one Sprite drawn with the Sprite's own HAS_INSTANCING variant: each
//   instance is an InstanceRenderData (the per-instance matrix, colour and texture rect the
//   instanced variants read as i_matrix_col1..4, i_color, i_textureRect, 96 bytes) and, once
//   enableFx() was called, a Vector4 of the four FX slots (i_spriteFx, 16 bytes); with pivots on
//   (a snapping scene), the instance's pivot (i_snapPivot, 8 bytes): a rotated instance snaps
//   at its position, as its Sprite drawn alone does, not at its frame's offset.
// - Producers: the Sprite Batch component (SpriteBatchComponent, scripts' bulk streams) and S50's
//   tilemap chunks. A producer writes instances() / fx() after setCount, sets the page runs, and
//   calls markChanged; the renderer uploads when the revision moved past the one it uploaded
//   (Retained), or every frame it draws (PerFrame), never when the batch is culled or empty.
// - Draws: one instanced draw per page run ([first, first + count) on one texture page), in order.
// - Not thread-safe: the main thread.

#include "Export.h"
#include "buffer/ExternalBuffer.h"
#include "component/InstancedMeshComponent.h"
#include "math/AABB.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Texture.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine{

    // a run of consecutive instances drawn from one page (texture) of the Sprite's frames
    struct SpritePageRun{
        uint32_t first = 0;
        uint32_t count = 0;
        uint16_t page = 0;
    };

    class JASPERENGINE_API SpriteInstanceBatch{
    public:
        enum class Storage : uint8_t { Retained, PerFrame };

        SpriteInstanceBatch();
        ~SpriteInstanceBatch();
        SpriteInstanceBatch(const SpriteInstanceBatch&) = delete;
        SpriteInstanceBatch& operator=(const SpriteInstanceBatch&) = delete;

        void setStorage(Storage value){ storage = value; }
        Storage getStorage() const { return storage; }

        // the GPU buffer's room, in instances (the mesh reloads when it grows: its bindings are
        // made at load)
        void reserve(uint32_t capacity);
        uint32_t getCapacity() const { return capacity; }
        void setCount(uint32_t count);
        uint32_t getCount() const { return count; }

        std::span<InstanceRenderData> instances(){ return {cpuInstances.data(), count}; }
        std::span<const InstanceRenderData> instances() const { return {cpuInstances.data(), count}; }
        // empty until enableFx()
        std::span<Vector4> fx(){ return fxEnabled ? std::span<Vector4>(cpuFx.data(), count) : std::span<Vector4>(); }
        std::span<const Vector4> fx() const { return fxEnabled ? std::span<const Vector4>(cpuFx.data(), count) : std::span<const Vector4>(); }
        // the FX stream from now on (a variant with i_spriteFx: the mesh reloads)
        void enableFx();
        bool hasFx() const { return fxEnabled; }
        // the pivot stream (empty while off; a change of it reloads the mesh)
        std::span<Vector2> pivots(){ return pivotsEnabled ? std::span<Vector2>(cpuPivots.data(), count) : std::span<Vector2>(); }
        void setPivotsEnabled(bool enabled);
        bool hasPivots() const { return pivotsEnabled; }

        // bumps the revision and refreshes the bounds from the instances' matrices (unit quad
        // corners: (x0, y0) .. (x1, y1) of the mesh the instances draw)
        void markChanged(float x0, float y0, float x1, float y1);
        uint64_t getRevision() const { return revision; }
        const AABB& localBounds() const { return bounds; }

        void setPageRuns(std::span<const SpritePageRun> runs);
        const std::vector<SpritePageRun>& getPageRuns() const { return pageRuns; }
        // the textures the runs' pages are (a sheet's pages), sampled as `sampler` (the Sprite's
        // texture settings); without pages every run draws with the material's texture
        void setPages(const std::vector<std::string>& paths, const Texture& sampler);
        size_t getPageCount() const { return pageTextures.size(); }
        Texture* getPageTexture(uint16_t page){ return page < pageTextures.size() ? &pageTextures[page] : nullptr; }

        // GPU side (RenderSystem): the instance layout every load binds, and the FX stream's
        ExternalBuffer& getInstanceBuffer(){ return instanceBuffer; }
        ExternalBuffer& getFxBuffer(){ return fxBuffer; }
        ExternalBuffer& getPivotBuffer(){ return pivotBuffer; }
        // creates the GPU buffers at the capacity (a load); false when sokol refused them
        bool createGpu();
        void destroyGpu();
        bool isGpuCreated() const;
        // what an upload needs to do before a draw this frame: nothing when the GPU copy is the
        // current revision (Retained) or was uploaded this frame (PerFrame, uploadFrame)
        bool needsUpload(uint64_t frame) const;
        // uploads the instances and the FX stream; the bytes written (0 when refused)
        size_t upload(uint64_t frame);
        uint32_t getGpuCapacity() const { return gpuCapacity; }
        // the GPU side was made with the FX stream (a load binds it)
        bool gpuHasFx() const { return gpuFx; }
        bool gpuHasPivots() const { return gpuPivots; }

    private:
        Storage storage = Storage::Retained;
        uint32_t capacity = 0;
        uint32_t count = 0;
        bool fxEnabled = false;
        std::vector<InstanceRenderData> cpuInstances;
        std::vector<Vector4> cpuFx;
        bool pivotsEnabled = false;
        std::vector<Vector2> cpuPivots;
        std::vector<SpritePageRun> pageRuns;
        std::vector<Texture> pageTextures;
        AABB bounds;
        uint64_t revision = 1;

        ExternalBuffer instanceBuffer;
        ExternalBuffer fxBuffer;
        ExternalBuffer pivotBuffer;
        uint32_t gpuCapacity = 0;
        bool gpuFx = false;
        bool gpuPivots = false;
        uint64_t uploadedRevision = 0;
        uint64_t uploadFrame = 0;
    };

}

#endif // SPRITE_INSTANCE_BATCH_H
