// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DRAW_BATCHER_H
#define DRAW_BATCHER_H

// Contract (sprite-batching spec, docs/rendering/batching.md)
// - Purpose: the runs of Dynamic Batching. RenderSystem submits each object a pass draws in batch
//   form with its key (BatchKey: what must be equal for two objects to draw as one) and its baked
//   vertices; the batcher puts it into a run and the runs are drawn, in the order they were opened,
//   when the pass flushes them (SokolSystem's deferred flush: any other draw of the pass, its end).
// - Modes: STRICT joins only the newest run (the pass's order is its draw order); REORDER also lets
//   an object join an older run when its world XY bounds overlap nothing drawn after that run
//   opened and every object jumped over is in its sort bucket (orthographic cameras along Z, where
//   disjoint XY bounds are disjoint on screen); GROUP joins any open run with the same key
//   (depth-only passes, whose result does not depend on order).
// - Sort buckets (BatchItem::sortKey): the caller submits objects in its draw order; an object never
//   moves past one of another bucket. 0 for every object today; Sorting Layers / Order in Layer
//   (pixel-art S52) put (layer rank, order) there and submit sorted by it.
// - Breaks: why an object did not join the run before it is counted by reason, the first
//   MAX_BREAK_LOG of a frame are kept with their entities (MCP get_render_stats batch_breaks).
// - Not thread-safe: the render thread (main thread) only.

#include "Export.h"
#include "ecs/Entity.h"
#include "sokol/SokolSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace jasperengine{

    struct MeshComponent;
    struct UIComponent;
    struct Transform;

    // why a draw did not join the batch before it, or why it is not batched at all (stable names:
    // getBatchBreakName, MCP get_render_stats)
    enum class BatchBreak : uint8_t {
        NONE = 0,
        // a key part differs from the previous batch's
        TEXTURE,
        PIPELINE,
        MATERIAL_PARAMS,
        ALPHA_CUTOFF,
        UV_SETS,
        OBJECT_LIGHTS,
        COLOR_FACTOR,
        UI_COLOR,
        TEXT_PARAMS,
        DEPTH_MATERIAL,
        MIRRORED,
        ORIGIN_CELL,
        SCISSOR,
        RUN_VERTEX_LIMIT,
        OVERLAP,
        SORT_BUCKET,
        // something that is not batched drew in between (or the pass ended)
        FOREIGN_DRAW,
        // drawn alone, by the object's own path
        INELIGIBLE_CUSTOM_SHADER,
        INELIGIBLE_EXTRA_PASS,
        INELIGIBLE_IBL,
        INELIGIBLE_SKINNED,
        INELIGIBLE_INSTANCED,
        INELIGIBLE_LATTICE,
        INELIGIBLE_VAT,
        INELIGIBLE_LOD,
        INELIGIBLE_VERTEX_COUNT,
        INELIGIBLE_SUBMESHES,
        INELIGIBLE_GEOMETRY,
        INELIGIBLE_RENDER_TARGET,
        INELIGIBLE_BILLBOARD,
        INELIGIBLE_TRANSFORM,
        INELIGIBLE_SHADER_PROPERTY,
        INELIGIBLE_MIRROR,
        INELIGIBLE_FLUID,
        INELIGIBLE_NOT_READY,
        DISABLED,
        COUNT
    };

    JASPERENGINE_API const char* getBatchBreakName(BatchBreak reason);
    // a reason an object is drawn alone, not a key difference
    inline bool isIneligibleBreak(BatchBreak reason){
        return reason >= BatchBreak::INELIGIBLE_CUSTOM_SHADER && reason < BatchBreak::COUNT;
    }

    enum class BatchPass : uint8_t {
        COLOR,
        UI,
        DEPTH_PREPASS,
        SCENE_DEPTH,
        SHADOW
    };

    enum class BatchMode : uint8_t {
        STRICT,
        REORDER,
        GROUP
    };

    // What two objects must share to draw as one: the pipeline (shader variant, layout, blend, depth,
    // stencil, primitive: pipelines are interned, S32), the variant it is drawn with, the origin
    // cell of the baked positions, the scissor, and as sections of bytes compared one by one the
    // objects' own textures and samplers (the TEXTURE section: the pass's own, shadow maps or SSAO,
    // are the run's) and the per-object values the batch does not bake.
    struct JASPERENGINE_API BatchKey{
        static constexpr size_t MAX_BYTES = 256;

        BatchPass pass = BatchPass::COLOR;
        uint8_t pipType = 0;
        bool mirrored = false;
        bool noCull = false;
        uint32_t pipeline = 0;
        int32_t cell[3] = {0, 0, 0};
        SokolSystem::ScissorRect scissor;

        // sections: [reason][length][bytes...], appended by addSection
        uint16_t size = 0;
        uint8_t bytes[MAX_BYTES];

        void clearSections(){ size = 0; }
        // false when the key has no room (the object is then drawn alone)
        bool addSection(BatchBreak reason, const void* data, size_t length);

        // NONE when an object of `other` can join a run of this key, else the first difference
        BatchBreak compare(const BatchKey& other) const;
    };

    // the object a run draws with: the first one of the run (its pipeline, bindings and pass-wide
    // uniforms are the run's)
    struct BatchHead{
        Entity entity = NULL_ENTITY;
        MeshComponent* mesh = nullptr;
        UIComponent* ui = nullptr;
        Transform* transform = nullptr;
    };

    struct BatchItem{
        Entity entity = NULL_ENTITY;
        uint64_t sortKey = 0;
        // world XY bounds (REORDER mode)
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };

    struct JASPERENGINE_API BatchRun{
        BatchKey key;
        BatchHead head;
        uint64_t sortKey = 0;
        unsigned int stride = 0;
        uint32_t vertexCount = 0;
        uint32_t items = 0;
        Entity lastEntity = NULL_ENTITY;
        // why its first object did not join the run before it (NONE: the pass's first)
        BatchBreak openedBy = BatchBreak::NONE;
        // every object's indices are the quad pattern (0 1 2 0 2 3 over 4 vertices): drawn from the
        // static quad index buffer, nothing uploaded
        bool allQuads = true;
        std::vector<unsigned char> vertices;
        std::vector<uint16_t> indices;
        // the XY bounds of its objects (REORDER mode)
        std::vector<std::array<float, 4>> rects;

        void reset();
        // the vertices of the next object, `count` of `stride` bytes, to fill
        unsigned char* appendVertices(uint32_t count);
        // its indices (relative to its first vertex), moved to the run's vertex range; after its
        // appendVertices
        void appendIndices(const uint16_t* indices, uint32_t count, uint32_t firstVertex);
    };

    // one batch break of the frame, for tools
    struct BatchBreakEvent{
        BatchBreak reason = BatchBreak::NONE;
        BatchPass pass = BatchPass::COLOR;
        Entity entity = NULL_ENTITY;
        Entity previous = NULL_ENTITY;
    };

    // a frame's counters (RenderSystem::getBatchStats)
    struct BatchStats{
        bool enabled = false;
        uint32_t batches = 0;          // draws of more than one object
        uint32_t batchedObjects = 0;   // objects drawn in those
        uint32_t singleDraws = 0;      // runs of one object (drawn in batch form)
        uint32_t aloneDraws = 0;       // objects drawn by their own path (ineligible)
        uint32_t settled = 0;          // objects whose baked vertices were reused as they were
        uint32_t rebaked = 0;          // objects baked again (a value or their geometry changed)
        uint32_t replaced = 0;         // objects placed again (they moved)
        uint32_t culled = 0;
        uint32_t cullCacheHits = 0;
        uint32_t reordered = 0;        // objects that joined an older run (REORDER)
        uint32_t runsSkipped = 0;      // runs not drawn (an upload refused by the Vulkan stream budget)
        uint32_t streamChunks = 0;
        uint64_t uploadBytes = 0;
        uint32_t breaks[(size_t)BatchBreak::COUNT] = {};

        uint32_t savedByBatching() const { return batchedObjects >= batches ? batchedObjects - batches : 0; }
    };

    class JASPERENGINE_API DrawBatcher{
    public:
        static constexpr uint32_t MAX_RUN_VERTICES = 65536;   // 16-bit indices
        static constexpr size_t MAX_BREAK_LOG = 100;
        static constexpr size_t MAX_OVERLAP_RECTS = 256;      // REORDER: rects one object is tested against

        // draws a run (the renderer); the run is reset after
        typedef void (*DrawRun)(void* owner, BatchRun& run);
        // asks for a flush of every open run (SokolSystem::flushDeferred, which calls flush())
        typedef void (*RequestFlush)();

        DrawBatcher();

        void setCallbacks(DrawRun drawRun, RequestFlush requestFlush, void* owner);

        // a pass of its own mode and window (open runs at most), flushed before it ends
        void beginPass(BatchMode mode, size_t maxOpenRuns);
        BatchMode getMode() const { return mode; }

        // The run an object joins (or opens), whose vertices and indices the caller then appends.
        // `stride` is the bytes per vertex of the object's layout (equal keys have equal layouts).
        BatchRun& place(const BatchKey& key, const BatchHead& head, const BatchItem& item, unsigned int stride);

        // draws every open run in order (called by the renderer's flush)
        void flush();
        bool hasOpenRuns() const { return openRuns > 0; }

        // an object drawn by its own path: it ends no run, but it is counted and logged
        void noteAlone(BatchPass pass, Entity entity, BatchBreak reason);

        // the frame's counters and breaks; clearFrame starts the next frame's
        BatchStats& getStats(){ return stats; }
        const BatchStats& getStats() const { return stats; }
        const std::vector<BatchBreakEvent>& getBreakLog() const { return breakLog; }
        void clearFrame();

    private:
        void logBreak(BatchBreak reason, BatchPass pass, Entity entity, Entity previous);
        BatchRun& openRun(const BatchKey& key, const BatchHead& head, const BatchItem& item, unsigned int stride, BatchBreak openedBy);
        bool overlapsAfter(size_t runIndex, const BatchItem& item, size_t& tested) const;

        DrawRun drawRunCallback = nullptr;
        RequestFlush requestFlushCallback = nullptr;
        void* owner = nullptr;

        BatchMode mode = BatchMode::STRICT;
        size_t maxOpenRuns = 1;
        // runs in the order they were opened: the first openRuns of them are in use (their storage
        // is kept for the next frames)
        std::vector<std::unique_ptr<BatchRun>> runs;
        size_t openRuns = 0;
        // the open runs were flushed by something else drawing (the next object's break reason)
        bool foreignSinceLastRun = false;
        bool internalFlush = false;
        Entity lastEntity = NULL_ENTITY;

        BatchStats stats;
        std::vector<BatchBreakEvent> breakLog;
    };

}

#endif // DRAW_BATCHER_H
