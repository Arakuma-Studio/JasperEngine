// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolobject_h
#define sokolobject_h

#include "render/Render.h"
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

        // the render state endLoad built the pipelines with, so a variant can be rebuilt from pipeline_desc
        RenderState loadState;

#ifdef JASPERENGINE_EDITOR
        // Editor-only variants of the pipelines that cull no face, one per pipeline type,
        // created on the first draw that asks for one and destroyed with the others
        static constexpr int PIPELINE_TYPES = 10;
        sg_pipeline noCullPipelines[PIPELINE_TYPES];
        // types whose pipeline already culls nothing, by bit index
        uint16_t noCullSame;
#endif

        size_t bindSlotIndex;

        std::map< BufferInfo, size_t > bufferToBindSlot;


        sg_vertex_format getVertexFormat(unsigned int elements, AttributeDataType dataType, bool normalized);
        sg_primitive_type getPrimitiveType(PrimitiveType primitiveType);
        sg_cull_mode getCullMode(CullingMode cullingMode);
        sg_face_winding getFaceWinding(WindingOrder windingOrder);
        sg_compare_func getCompareFunc(DepthCompare compare);
        // the colour blending of the load state into a colour target
        void applyBlend(sg_color_target_state& target, bool offscreen);
        // the description endLoad creates the pipeline of one type from
        sg_pipeline_desc makePipelineDesc(PipelineType pipType);
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
        void setIndexBuffer(sg_buffer buffer);
        void resetIndexBuffer();
        void addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer);
        void addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture);
        bool endLoad(uint16_t pipelines, const RenderState& state);

        // disableFaceCulling draws with a variant of the pipeline that culls no face
        // (editor-only debug views; ignored elsewhere)
        bool beginDraw(PipelineType pipType, bool disableFaceCulling = false);
        void applyUniformBlock(int slot, unsigned int count, void* data);
        void draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();

    };
}
#endif //sokolobject_h
