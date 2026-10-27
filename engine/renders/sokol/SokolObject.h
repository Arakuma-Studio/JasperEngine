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
    // references of its own, clear() and the destructor release them
    class SokolVariantRefs{
    public:
        struct Variant {
            uint8_t kind;
            uint8_t type; // pipeline type, by bit index
            sg_pipeline pipeline;
        };

        SokolVariantRefs() = default;
        SokolVariantRefs(const SokolVariantRefs& rhs);
        SokolVariantRefs& operator=(const SokolVariantRefs& rhs);
        ~SokolVariantRefs();

        // the variant of that kind and type this object holds (its pipeline SG_INVALID_ID when
        // it could not be made), null when it holds none
        const Variant* find(uint8_t kind, uint8_t type) const;
        // a reference acquired by the caller
        void add(uint8_t kind, uint8_t type, sg_pipeline pipeline);
        void clear();

    private:
        std::vector<Variant> list;
    };

    class SokolObject{

    private:

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

        // the render state endLoad built the pipelines with, so a variant can be rebuilt from pipeline_desc
        RenderState loadState;
        // the sample count of the PIP_MSAA* pipelines (endLoad's)
        int msaaSamples;

        // Variants of the pipelines, made on the first draw that asks for one: VARIANT_MIRRORED has
        // the face winding reversed (a draw whose transform mirrors the mesh, so its front faces
        // stay front: culling and gl_FrontFacing), VARIANT_NO_CULL culls no face (editor-only
        // debug views), VARIANT_MIRRORED_NO_CULL both. They are SokolPipelineCache's shared
        // variants (every object with the same description draws with one pipeline): this object
        // holds a reference to each it uses, its own also in a copy (taken again on copy),
        // released by beginLoad, destroy and the destructor
        static constexpr uint8_t VARIANT_MIRRORED = 0;
        static constexpr uint8_t VARIANT_NO_CULL = 1;
        static constexpr uint8_t VARIANT_MIRRORED_NO_CULL = 2;
        SokolVariantRefs variants;
        // types whose pipeline already culls nothing (its no-cull variant is itself), by bit index
        uint16_t noCullSame;

        size_t bindSlotIndex;
        // the vertex inputs (locations) the shader declares, set by setShader
        uint32_t shaderInputs;

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
        bool endLoad(uint16_t pipelines, const RenderState& state, int msaaSamples = 1);
        // records shader, vertex layout and index type for beginDraw(PipelineState); creates no pipeline
        bool endLoadDynamic(PrimitiveType primitiveType, IndexType indexType);

        // disableFaceCulling draws with a variant of the pipeline that culls no face
        // (editor-only debug views; ignored elsewhere); mirrored with one whose face winding is
        // reversed (a draw whose transform mirrors the mesh)
        bool beginDraw(PipelineType pipType, bool disableFaceCulling = false, bool mirrored = false);
        // the cached pipeline of that state (SokolPipelineCache), applied unless it is the one
        // applied last in this pass
        bool beginDraw(const PipelineState& state);
        void applyUniformBlock(int slot, unsigned int count, void* data);
        void draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();


    };
}
#endif //sokolobject_h
