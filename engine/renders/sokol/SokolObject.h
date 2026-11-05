// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolobject_h
#define sokolobject_h

#include "render/Render.h"
#include "render/PipelineState.h"
#include "buffer/Attribute.h"
#include "render/BufferRender.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"
#include "shader/ShaderData.h"

#include "sokol_gfx.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>


namespace jasperengine{

    // A vertex bind slot: the buffer, its offset, and the layout the slot steps it with. Two
    // attributes of the same GPU buffer at the same offset share a slot only when they also step it
    // the same way: buffers shared by content (MeshBufferPool) give equal bytes of different
    // attributes one GPU buffer, which must not fold an 8-byte stride into a 16-byte one.
    struct BufferInfo{
        uint32_t id;
        size_t offset;
        unsigned int stride = 0;
        bool perInstance = false;

        bool operator==(const BufferInfo &o) const {
            return id == o.id && offset == o.offset && stride == o.stride && perInstance == o.perInstance;
        }

        bool operator<(const BufferInfo &o) const {
            if (id != o.id) return id < o.id;
            if (offset != o.offset) return offset < o.offset;
            if (stride != o.stride) return stride < o.stride;
            return perInstance < o.perInstance;
        }
    };

    // References to SokolPipelineCache's shared pipeline variants (acquireVariant): a copy takes
    // references of its own, clear() and the destructor release them. A reference made in an
    // earlier render context (SystemRender::getContextId) is neither retained nor released (the
    // context's shutdown destroyed the variants; sokol may hand their ids out again) and is not found
    class SokolVariantRefs{
    public:
        struct Variant {
            uint8_t kind;
            uint8_t type; // pipeline type, by bit index
            sg_pipeline pipeline;
            uint64_t context; // SystemRender::getContextId it was acquired in
        };

        SokolVariantRefs() = default;
        SokolVariantRefs(const SokolVariantRefs& rhs);
        SokolVariantRefs& operator=(const SokolVariantRefs& rhs);
        ~SokolVariantRefs();

        // the variant of that kind and type this object holds in the current context (its
        // pipeline SG_INVALID_ID when it could not be made), null when it holds none
        const Variant* find(uint8_t kind, uint8_t type) const;
        // a reference acquired by the caller in the current context (replaces one of that kind
        // and type from an earlier context)
        void add(uint8_t kind, uint8_t type, sg_pipeline pipeline);
        // releases the references of the kinds from `kind` on
        void removeFrom(uint8_t kind);
        void clear();

    private:
        std::vector<Variant> list;
    };

    // What the per-instance data of an automatically instanced batch (spec S55) is made of: the
    // per-draw object block its draws apply, read into the vectors of the instanced variant
    // (includes/object_params.glsl, includes/object_depth_params.glsl)
    enum class BatchLayout : uint8_t {
        NONE = 0,
        MESH,   // u_vs_pbrParams (model, normal, mvp): 10 vectors an instance
        DEPTH   // u_vs_depthParams (model, view-projection, mvp): 8 vectors an instance
    };

    class SokolObject;

    // Automatic GPU instancing and redundant state (spec S55), on while GPU Instancing is (Project
    // Settings > Graphics; off, every draw applies its pipeline, uniforms and bindings as before).
    // On, a pipeline already applied in the pass is not applied again, nor a uniform block whose
    // bytes are the ones last applied to its slot since that pipeline (sokol keeps them; it asks for
    // them again only after sg_apply_pipeline). Between begin() and end() (a pass's opaque queue)
    // the draws of objects told setNextDrawBatch are held: consecutive ones that differ only in
    // their object block (same pipeline, bindings, elements and every other uniform byte) become
    // one instanced draw of their variant, the object blocks its instances (appended to one stream
    // buffer a frame); a held draw that stays alone is drawn as it would have been. Render thread.
    class SokolDrawBatch{
    public:
        struct FrameStats{
            uint32_t instancedDraws = 0;    // instanced draws the batches made
            uint32_t batchedInstances = 0;  // the draws they replaced
            uint64_t uniformBytesKept = 0;  // uniform bytes not applied again (already in their slot)
            uint32_t pipelinesKept = 0;     // pipelines not applied again
            uint64_t instanceBytes = 0;     // instance data appended
            // why held draws that could batch did not join the open batch (diagnostics: a scene
            // whose copies stay single shows which state differs between them)
            uint32_t offeredDraws = 0;      // held with a linked variant
            uint32_t splitPipeline = 0;     // another pipeline, variant or object block slot
            uint32_t splitElements = 0;     // other elements (a LOD level, another submesh range)
            uint32_t splitBindings = 0;     // other buffers, textures or samplers
            uint32_t splitUniforms = 0;     // another uniform block or its bytes
            uint32_t splitUniformSlots = 0; // the slots whose bytes differed (bit 31: other blocks)
            uint32_t splitBindingParts = 0; // 1 vertex buffers, 2 their offsets, 4 index buffer, 8 views, 16 samplers
        };

        static void setEnabled(bool enabled);
        static bool isEnabled();
        static void begin();
        // draws what is held
        static void end();
        static bool isRecording();
        // draws the batch being held now (before something else draws, outside SokolObject)
        static void flush();
        // a pipeline or uniforms applied outside SokolObject: nothing applied before is assumed
        static void invalidate();
        // the frame was committed (SokolSystem::commit): its counters become the last frame's, the
        // instance stream buffer grows when the frame needed more
        static void endFrame();
        static const FrameStats& getLastFrameStats();
        // bytes into this frame's instance stream buffer (sokol's, shared with the batches): their
        // offset, -1 when they do not fit this frame (it grows for the next) or the stream budget
        // refused them
        static int appendStream(const void* data, size_t bytes);
        static sg_buffer streamBuffer();
        // releases the instance stream buffer (sokol's, made on the first batch of a frame and
        // made again bigger when a frame needed more)
        static void shutdown();

        // (SokolObject) a uniform block applied directly: false when refused by the frame's
        // uniform buffer
        static bool applyUniforms(int slot, const void* data, unsigned int size);
        static void applyPipeline(sg_pipeline pipeline);
        // (SokolObject, while recording) the draw being set up from beginDraw to draw: true when
        // held (its uniforms copied, its draw drawn with its batch)
        static void beginHeld(SokolObject* object, sg_pipeline pipeline);
        static bool holdUniforms(SokolObject* object, int slot, unsigned int size, const void* data);
        static bool holdDraw(SokolObject* object, unsigned int base, unsigned int count, unsigned int instances);
    };

    class SokolObject{
        friend class SokolDrawBatch;

    private:
        void addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset,
                          bool normalized, bool perInstance, bool interleaved);

        sg_bindings bind;
        sg_buffer loadIndexBuffer;
        size_t loadIndexOffset;

        // the pipeline types (PipelineType), by bit index
        static constexpr int PIPELINE_TYPES = 16;
        sg_pipeline pipelines[PIPELINE_TYPES];

        sg_pipeline_desc pipeline_desc;

        // the dynamic path (endLoadDynamic): shader, vertex layout, index and primitive type;
        // its pipelines belong to SokolPipelineCache
        sg_pipeline_desc baseDesc;
        bool dynamicLoaded;
        // the last pipeline of the dynamic path: consecutive draws with the same state skip the
        // cache's lookup (valid while SokolPipelineCache::generation() is the same)
        uint64_t dynamicBaseHash;
        PipelineState lastDynamicState;
        sg_pipeline lastDynamicPipeline;
        uint32_t lastDynamicGeneration;

        // GL warm draws (spec S32): per pipeline type, the pipeline confirmed warm with this
        // object's bindings and the warm draw requested for it; the dynamic path's the same per
        // pipeline. A draw waits for its warm draw (beginDraw is false until then)
        // created at once and never waiting for a warm draw (ObjectRender::setImmediatePipelines)
        bool immediatePipelines = false;
        // the last beginDraw drew nothing because its pipeline (or shader) is still being created
        // or warmed, not because it failed: the object needs no reload (isDrawWaiting)
        bool drawWaiting = false;
        // the pipelines made ahead for another variant of the shader (prewarmWith), per type
        struct Prewarmed {
            uint32_t shader;
            int index;
            sg_pipeline pipeline;
        };
        std::vector<Prewarmed> prewarmed;
        uint32_t warmedPipeline[PIPELINE_TYPES];
        uint64_t warmKey[PIPELINE_TYPES];
        std::vector<std::pair<uint32_t, uint64_t>> dynamicWarm;
        bool isWarm(sg_pipeline pipeline, uint32_t& warmed, uint64_t& key, sg_shader shader);
        bool isDynamicWarm(sg_pipeline pipeline);
        void resetWarm();

        // the render state endLoad built the pipelines with, so a variant can be rebuilt from pipeline_desc
        RenderState loadState;
        // the sample count of the PIP_MSAA* pipelines (endLoad's)
        int msaaSamples;

        // Variants of the pipelines, made on the first draw that asks for one: VARIANT_MIRRORED has
        // the face winding reversed (a draw whose transform mirrors the mesh, so its front faces
        // stay front: culling and gl_FrontFacing), VARIANT_NO_CULL culls no face (editor-only
        // debug views), VARIANT_MIRRORED_NO_CULL both. They are SokolPipelineCache's shared
        // variants (interned pipelines, spec S32: every object with the same description draws with one): this object
        // holds a reference to each it uses, its own also in a copy (taken again on copy),
        // released by beginLoad, destroy and the destructor
        static constexpr uint8_t VARIANT_MIRRORED = 0;
        static constexpr uint8_t VARIANT_NO_CULL = 1;
        static constexpr uint8_t VARIANT_MIRRORED_NO_CULL = 2;
        // the weighted blended OIT accumulation pipelines of an object loaded alike another
        // (beginAccumulationDraw): unmirrored and mirrored, by the colour pipeline type they follow
        static constexpr uint8_t VARIANT_ACCUMULATE = 3;
        static constexpr uint8_t VARIANT_ACCUMULATE_MIRRORED = 4;
        // The LOD cross-fade's (spec S54 R6): the same description on the fade variant of the
        // shader ('Lfd', its bindings the base's: RenderSystem checks them), and its mirrored and
        // no-cull forms (+ the kinds above); made for fading draws only
        static constexpr uint8_t VARIANT_LOD_FADE = 5;
        // the editor's Overdraw Draw Mode: additive, no depth test or write, no culling
        static constexpr uint8_t VARIANT_OVERDRAW = 9;
        SokolVariantRefs variants;
        // the fade variant's shader the VARIANT_LOD_FADE kinds were made on, and their GL warm
        // draws (apart from the base pipelines', which the same object keeps drawing)
        uint32_t lodFadeShaderId = SG_INVALID_ID;
        uint32_t lodFadeWarmed[PIPELINE_TYPES];
        uint64_t lodFadeWarmKey[PIPELINE_TYPES];
        // the pipeline the last beginDraw applied (diagnostics, tests)
        uint32_t appliedPipeline = SG_INVALID_ID;
        // the pipeline of a type for a draw: a variant made on first use (mirrored, no cull,
        // LOD fade), or the type's own; waits (false, drawWaiting) while it is being made
        enum class Selection { READY, WAIT, FAIL };
        Selection selectPipeline(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader, sg_pipeline& selected);
        // types whose pipeline already culls nothing (its no-cull variant is itself), by bit index
        uint16_t noCullSame;

        size_t bindSlotIndex;
        // the vertex inputs (locations) the shader declares, set by setShader
        uint32_t shaderInputs;
        // the shader's vertex inputs, names at their locations (setShader): prewarmWith makes another
        // variant's pipelines on this object's layout only when that variant takes the same inputs
        uint64_t shaderInputSignature = 0;

        // color blending of the color pipelines when set (setBlendState)
        bool blendOverride;
        BlendState blendState;

        std::map< BufferInfo, size_t > bufferToBindSlot;

        // (spec S55) As the instanced variant of another object (linkBatchVariant): that object's
        // uniform slots in this one's shader (-1: the per-instance object block), and the bind slot
        // of the instance stream buffer, after that object's vertex buffers
        int8_t batchUniformSlots[SG_MAX_UNIFORMBLOCK_BINDSLOTS];
        int batchInstanceBindSlot = -1;
        BatchLayout batchLayout = BatchLayout::NONE;
        const char* batchLinkFailure = nullptr;
        // the next draw's batch (setNextDrawBatch), taken by that draw
        SokolObject* nextBatchVariant = nullptr;
        int nextBatchObjectSlot = -1;
        float nextBatchLodFade = 0.0f;
        // the pipeline type of the last beginDraw (by bit index) and whether it drew the plain
        // pipeline (not a mirrored or no-cull variant): the variant draws the same type
        int lastPipelineIndex = -1;
        bool lastPipelinePlain = false;
        // the variant's pipeline of that type, ready to draw with these bindings (created, its
        // GL warm draw done): false while it is not (its batch is drawn draw by draw)
        bool batchPipeline(int pipelineIndex, const sg_bindings& bindings, sg_pipeline& pipeline);


        sg_vertex_format getVertexFormat(unsigned int elements, AttributeDataType dataType, bool normalized);
        sg_primitive_type getPrimitiveType(PrimitiveType primitiveType);
        static sg_cull_mode getCullMode(CullingMode cullingMode);
        static sg_face_winding getFaceWinding(WindingOrder windingOrder);
        static sg_compare_func getCompareFunc(DepthCompare compare);
        static sg_blend_factor getBlendFactor(BlendFactor factor);
        static sg_blend_op getBlendOp(BlendOperation operation);
        // the colour blending of the load state into a colour target (then a BlendState set
        // with setBlendState replaces it)
        void applyBlend(sg_color_target_state& target, bool offscreen) const;
        // the stencil state and colour write mask of the load state, for a colour pipeline
        // (a swapchain target never takes alpha writes)
        void applyStencilAndColorMask(sg_pipeline_desc& desc, bool offscreen) const;
        // the description endLoad creates the pipeline of one type from
        sg_pipeline_desc makePipelineDesc(PipelineType pipType) const;
        sg_pipeline& pipelineOf(PipelineType pipType);
        // counts (and reports) a load leaving a vertex input of its shader without a buffer
        void checkVertexInputs();
        void resetPipelines();

    public:
        // the colour draws until it is turned off take the Overdraw variant (the editor's Scene
        // view Draw Mode; ignored outside the editor)
        static void setOverdraw(bool overdraw);
        // pipelines made by every object and not destroyed yet (diagnostics)
        static int getLivePipelineCount();
        // the shared pipeline variants alive (SokolPipelineCache::variantCount)
        static size_t getSharedVariantCount();
        // loads that left a vertex input of their shader without a buffer (diagnostics)
        static int getUnfedVertexInputLoads();

        SokolObject();
        SokolObject(const SokolObject& rhs);
        SokolObject& operator=(const SokolObject& rhs);

        void beginLoad(PrimitiveType primitiveType);
        void setShader(ShaderRender* shader);
        void setIndex(BufferRender* buffer, AttributeDataType dataType, size_t offset);
        void addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance);
        // an attribute of an interleaved buffer that stays one bind slot on every backend: the
        // offset (inside the stride, below 2048) is the attribute's, not the buffer binding's
        // (a layout of more attributes than sokol has vertex buffer bind slots)
        void addInterleavedAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance);
        // rebind every vertex bind-slot that was filled from fromBufferId to toBuffer,
        // keeping the same per-slot layout/offset (used to swap a per-view instance
        // buffer in just before draw). Affects only the bindings, not the pipeline.
        void replaceVertexBuffer(uint32_t fromBufferId, sg_buffer toBuffer, size_t byteOffset);
        void setIndexBuffer(sg_buffer buffer, size_t byteOffset = 0);
        void resetIndexBuffer();
        // for good (the buffer the load bound was replaced: a shared buffer of new bytes, a private
        // one): every vertex bind slot of fromBufferId now binds toBuffer with the same layout and
        // offsets, and replaceVertexBuffer finds it by toBuffer's id; the index buffer the same
        void retargetVertexBuffer(uint32_t fromBufferId, sg_buffer toBuffer);
        void retargetIndexBuffer(uint32_t fromBufferId, sg_buffer toBuffer);
        // the vertex buffer bind slots the attributes added so far use (diagnostics, tests)
        size_t getVertexBindSlotCount() const { return bindSlotIndex; }
        // the view and sampler ids bound at a texture slot (0, 0 when the slot is -1)
        void getTextureBinding(std::pair<int, int> slot, uint32_t& view, uint32_t& sampler) const;
        void addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer);
        void addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture);
        void addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture, TextureRender* samplerOf);
        void setBlendState(const BlendState& state);
        void setImmediatePipelines(bool immediate);
        bool isReady() const;
        uint32_t getPipelineId(PipelineType pipType) const;
        bool prewarmWith(ShaderRender* shader);
        bool isDrawWaiting() const;
        static uint32_t getWaitingDrawCount();
        bool endLoad(uint16_t pipelines, const RenderState& state, int msaaSamples = 1);
        // records shader, vertex layout and index type for beginDraw(PipelineState); creates no pipeline
        bool endLoadDynamic(PrimitiveType primitiveType, IndexType indexType);
        // another variant of source's shader (the same vertex inputs and bindings) on source's
        // vertex layout, bindings and render state; creates no pipeline (beginAccumulationDraw)
        bool loadAlike(const SokolObject& source, ShaderRender* shader);
        // source's bindings (buffers, textures) as they are now, for the next draws
        void shareBindings(const SokolObject& source);
        // the weighted blended OIT accumulation (RenderSystem, includes/oit.glsl): the pipeline of
        // colorType (PIP_RTT or PIP_MSAA) with two colour targets, RGBA16F and R16F, both blended
        // RGB ONE, ONE and alpha ZERO, ONE_MINUS_SRC_ALPHA, writing no depth. Made on first use
        bool beginAccumulationDraw(PipelineType colorType, bool mirrored);

        // disableFaceCulling draws with a variant of the pipeline that culls no face
        // (editor-only debug views; ignored elsewhere); mirrored with one whose face winding is
        // reversed (a draw whose transform mirrors the mesh)
        bool beginDraw(PipelineType pipType, bool disableFaceCulling = false, bool mirrored = false, ShaderRender* lodFadeShader = nullptr);
        // the LOD fade variant of that draw made (asked for on the first call) and warm
        bool isLodFadeReady(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader);
        uint32_t getAppliedPipelineId() const;
        // the cached pipeline of that state (SokolPipelineCache), applied unless it is the one
        // applied last in this pass
        bool beginDraw(const PipelineState& state);
        void applyUniformBlock(int slot, unsigned int count, void* data);

        // (spec S55) The per-instance attributes of a batch variant (BatchLayout's vectors, at the
        // locations given in order), in the instance stream buffer, after every other attribute
        void addBatchInstanceAttributes(const int* locations, int count, BatchLayout layout);
        // After both loaded: makes this object (its shader variantShader) the instanced variant of
        // source (shader sourceShader), whose per-draw block objectBlock it reads per instance. False
        // when it cannot draw source's batches: another uniform block, texture, sampler or storage
        // buffer layout, other vertex buffers, or the object block still there (a shader ignoring
        // HAS_AUTO_INSTANCING)
        bool linkBatchVariant(const SokolObject& source, const ShaderData& sourceShader, const ShaderData& variantShader,
                              const std::string& objectBlock);
        bool isBatchVariant() const;
        // what the last linkBatchVariant refused (null: linked, or never tried)
        const char* getBatchLinkFailure() const;
        // the next draw of this object may join a batch drawn by variant (linked to it), its object
        // block in uniform slot objectSlot, with this LOD fade (spec S54: 0 none)
        void setNextDrawBatch(SokolObject* variant, int objectSlot, float lodFade);
        // per-instance data of this draw in the frame's stream buffer instead of the vertex buffer
        // fromBufferId (the same layout): false when it does not fit this frame
        bool bindStreamVertexBuffer(uint32_t fromBufferId, const void* data, size_t bytes);
        // the bindings now (diagnostics and sort keys: the first vertex and the index buffer)
        uint32_t getVertexBufferId() const;
        uint32_t getIndexBufferId() const;
        // false: skipped, its uniforms did not fit the frame's uniform buffer (SokolSystem::reserveUniformBytes)
        // or its descriptor sets the descriptor buffer (SokolSystem::reserveDescriptorBytes)
        bool draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();


    };
}
#endif //sokolobject_h
