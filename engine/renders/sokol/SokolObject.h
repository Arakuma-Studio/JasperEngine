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

    class SokolObject{

    private:

        sg_bindings bind;
        sg_buffer loadIndexBuffer;
        size_t loadIndexOffset;

        sg_pipeline pip;
        sg_pipeline depth_pip;
        sg_pipeline shadow_depth_pip;
        sg_pipeline rtt_pip;
        sg_pipeline rtt_invert_pip;
        sg_pipeline gbuffer_pip;
        sg_pipeline nodepth_pip;
        sg_pipeline rtt_nodepth_pip;
        sg_pipeline zprepass_pip;
        sg_pipeline zprepass_rtt_pip;

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

        // the face state endLoad built the pipelines with, so a variant can be rebuilt from pipeline_desc
        bool loadFaceCulling;
        bool loadDepthWrite;
        CullingMode loadCullingMode;
        WindingOrder loadWindingOrder;
        bool loadPrepassFaceCulling;

#ifdef JASPERENGINE_EDITOR
        // Editor-only variants of the pipelines that cull no face, one per pipeline type,
        // created on the first draw that asks for one and destroyed with the others
        static constexpr int PIPELINE_TYPES = 10;
        sg_pipeline noCullPipelines[PIPELINE_TYPES];
        // types whose pipeline already culls nothing, by bit index
        uint16_t noCullSame;
#endif

        size_t bindSlotIndex;

        // color blending of the color pipelines when set (setBlendState)
        bool blendOverride;
        BlendState blendState;

        std::map< BufferInfo, size_t > bufferToBindSlot;


        sg_vertex_format getVertexFormat(unsigned int elements, AttributeDataType dataType, bool normalized);
        sg_primitive_type getPrimitiveType(PrimitiveType primitiveType);
        static sg_cull_mode getCullMode(CullingMode cullingMode);
        static sg_face_winding getFaceWinding(WindingOrder windingOrder);
        static sg_blend_factor getBlendFactor(BlendFactor factor);
        static sg_blend_op getBlendOp(BlendOperation operation);
        // the description endLoad creates the pipeline of one type from
        sg_pipeline_desc makePipelineDesc(PipelineType pipType) const;
        sg_pipeline& pipelineOf(PipelineType pipType);
        void resetPipelines();

    public:

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
        void setBlendState(const BlendState& state);
        // prepassFaceCulling applies to the PIP_ZPREPASS pipelines, which must cull like the color pass
        bool endLoad(uint16_t pipelines, bool enableFaceCulling, bool enableDepthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling = true);
        // records shader, vertex layout and index type for beginDraw(PipelineState); creates no pipeline
        bool endLoadDynamic(PrimitiveType primitiveType, IndexType indexType);

        // disableFaceCulling draws with a variant of the pipeline that culls no face
        // (editor-only debug views; ignored elsewhere)
        bool beginDraw(PipelineType pipType, bool disableFaceCulling = false);
        // the cached pipeline of that state (SokolPipelineCache), applied unless it is the one
        // applied last in this pass
        bool beginDraw(const PipelineState& state);
        void applyUniformBlock(int slot, unsigned int count, void* data);
        void draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();

        // test/debug use: the sg_pipeline_desc endLoad creates (or created) the pipeline
        // of that type from, with the current load description
        sg_pipeline_desc describePipeline(PipelineType pipType) const;

    };
}
#endif //sokolobject_h
