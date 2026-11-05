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

#include "sokol_gfx.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>


namespace jasperengine{

    struct BufferInfo{
        uint32_t id;
        size_t offset;

        bool operator==(const BufferInfo &o) const {
            return id == o.id && offset == o.offset;
        }

        bool operator<(const BufferInfo &o) const {
            return id < o.id || (id == o.id && offset < o.offset);
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

    class SokolObject{

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
        // false: skipped, its uniforms did not fit the frame's uniform buffer (SokolSystem::reserveUniformBytes)
        // or its descriptor sets the descriptor buffer (SokolSystem::reserveDescriptorBytes)
        bool draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();


    };
}
#endif //sokolobject_h
