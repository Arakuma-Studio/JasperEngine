// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolObject.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Matrix4.h"
#include "Log.h"
#include "SokolCmdQueue.h"
#include "SokolShader.h"
#include "Engine.h"

using namespace jasperengine;

namespace {
    sg_pipeline makePipeline(const sg_pipeline_desc& desc){
        if (Engine::isAsyncThread())
            return SokolCmdQueue::add_command_make_pipeline(desc);
        return sg_make_pipeline(desc);
    }

    void destroyPipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID)
            return;
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_pipeline(pipeline);
        }else{
            sg_destroy_pipeline(pipeline);
        }
    }
}

SokolObject::SokolObject(){
    pip.id = SG_INVALID_ID;
    depth_pip.id = SG_INVALID_ID;
    shadow_depth_pip.id = SG_INVALID_ID;
    rtt_pip.id = SG_INVALID_ID;
    rtt_invert_pip.id = SG_INVALID_ID;
    gbuffer_pip.id = SG_INVALID_ID;
    nodepth_pip.id = SG_INVALID_ID;
    rtt_nodepth_pip.id = SG_INVALID_ID;
    zprepass_pip.id = SG_INVALID_ID;
    zprepass_rtt_pip.id = SG_INVALID_ID;
    bind = {};
    loadIndexBuffer.id = SG_INVALID_ID;
    loadIndexOffset = 0;
    pipeline_desc = {};
    loadState = RenderState();
    bindSlotIndex = 0;
    resetPipelines();
}

// Forgets the variants without destroying them: a copy of this object shares the
// handles, as it shares the pipelines, and destroy() releases them once
void SokolObject::resetPipelines(){
#ifdef JASPERENGINE_EDITOR
    for (sg_pipeline& variant : noCullPipelines){
        variant.id = SG_INVALID_ID;
    }
    noCullSame = 0;
#endif
}

SokolObject::SokolObject(const SokolObject& rhs) = default;

SokolObject& SokolObject::operator=(const SokolObject& rhs) = default;

sg_vertex_format SokolObject::getVertexFormat(unsigned int elements, AttributeDataType dataType, bool normalized){
    if (dataType == AttributeDataType::BYTE){
        if (elements == 4)
            return (normalized)?SG_VERTEXFORMAT_BYTE4N:SG_VERTEXFORMAT_BYTE4;
    }
    if (dataType == AttributeDataType::UNSIGNED_BYTE){
        if (elements == 4)
            return (normalized)?SG_VERTEXFORMAT_UBYTE4N:SG_VERTEXFORMAT_UBYTE4;
    }
    if (dataType == AttributeDataType::SHORT){
        if (elements == 2)
            return (normalized)?SG_VERTEXFORMAT_SHORT2N:SG_VERTEXFORMAT_SHORT2;
        if (elements == 4)
            return (normalized)?SG_VERTEXFORMAT_SHORT4N:SG_VERTEXFORMAT_SHORT4;
    }
    if (dataType == AttributeDataType::UNSIGNED_SHORT){
        if (elements == 2)
            return SG_VERTEXFORMAT_USHORT2N;
        if (elements == 4)
            return SG_VERTEXFORMAT_USHORT4N;
    }
    if (dataType == AttributeDataType::FLOAT){
        if (elements == 1)
            return SG_VERTEXFORMAT_FLOAT;
        if (elements == 2)
            return SG_VERTEXFORMAT_FLOAT2;
        if (elements == 3)
            return SG_VERTEXFORMAT_FLOAT3;
        if (elements == 4)
            return SG_VERTEXFORMAT_FLOAT4;
    }
    return SG_VERTEXFORMAT_INVALID;
}

sg_primitive_type SokolObject::getPrimitiveType(PrimitiveType primitiveType){
    if (primitiveType == PrimitiveType::TRIANGLES)
        return SG_PRIMITIVETYPE_TRIANGLES;
    else if (primitiveType == PrimitiveType::TRIANGLE_STRIP)
        return SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    else if (primitiveType == PrimitiveType::LINES)
        return SG_PRIMITIVETYPE_LINES;
    else if (primitiveType == PrimitiveType::POINTS)
        return SG_PRIMITIVETYPE_POINTS;

    return _SG_PRIMITIVETYPE_DEFAULT;
}

sg_cull_mode SokolObject::getCullMode(CullingMode cullingMode){
    if (cullingMode == CullingMode::BACK){
        return SG_CULLMODE_BACK;
    }else if (cullingMode == CullingMode::FRONT){
        return SG_CULLMODE_FRONT;
    }

    return _SG_CULLMODE_DEFAULT;
}

sg_face_winding SokolObject::getFaceWinding(WindingOrder windingOrder){
    if (windingOrder == WindingOrder::CCW){
        return SG_FACEWINDING_CCW;
    }else if (windingOrder == WindingOrder::CW){
        return SG_FACEWINDING_CW;
    }

    return _SG_FACEWINDING_DEFAULT;
}

sg_compare_func SokolObject::getCompareFunc(DepthCompare compare){
    switch (compare){
        case DepthCompare::LESS:          return SG_COMPAREFUNC_LESS;
        case DepthCompare::EQUAL:         return SG_COMPAREFUNC_EQUAL;
        case DepthCompare::GREATER_EQUAL: return SG_COMPAREFUNC_GREATER_EQUAL;
        case DepthCompare::GREATER:       return SG_COMPAREFUNC_GREATER;
        case DepthCompare::NOT_EQUAL:     return SG_COMPAREFUNC_NOT_EQUAL;
        case DepthCompare::ALWAYS:        return SG_COMPAREFUNC_ALWAYS;
        default:                          return SG_COMPAREFUNC_LESS_EQUAL;
    }
}

void SokolObject::applyBlend(sg_color_target_state& target, bool offscreen){
    if (loadState.blend == BlendMode::NONE){
        target.blend.enabled = false;
        return;
    }
    target.blend.enabled = true;
    switch (loadState.blend){
        case BlendMode::PREMULTIPLIED:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::ADDITIVE:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            break;
        case BlendMode::MULTIPLY:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ZERO;
            break;
        default:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
    }
    // an offscreen target keeps its coverage in alpha, whatever the colour blend
    if (offscreen){
        target.blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        target.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }
}

void SokolObject::beginLoad(PrimitiveType primitiveType){
    bind = {0};
    pip = {0};
    depth_pip = {0};
    shadow_depth_pip = {0};
    rtt_pip = {0};
    rtt_invert_pip = {0};
    gbuffer_pip = {0};
    pipeline_desc = {0};
    resetPipelines();

    pipeline_desc.primitive_type = getPrimitiveType(primitiveType);

    bindSlotIndex = 0;
    bufferToBindSlot.clear();
}

void SokolObject::setShader(ShaderRender* shader){
    pipeline_desc.shader = shader->backend.get();
}

void SokolObject::setIndex(BufferRender* buffer, AttributeDataType dataType, size_t offset){
    sg_buffer ibuf = buffer->backend.get();
    bind.index_buffer = ibuf;
    bind.index_buffer_offset = offset;
    loadIndexBuffer = ibuf;
    loadIndexOffset = offset;

    if (dataType == AttributeDataType::UNSIGNED_SHORT){
        pipeline_desc.index_type = SG_INDEXTYPE_UINT16;
    }else if (dataType == AttributeDataType::UNSIGNED_INT){
        pipeline_desc.index_type = SG_INDEXTYPE_UINT32;
    }else{
        pipeline_desc.index_type = SG_INDEXTYPE_NONE;
    }
}

void SokolObject::addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance){
    if (slot != -1){
        sg_buffer vbuf = buffer->backend.get();

        // D3D11 cannot have offset (AlignedByteOffset) bigger than 2048
        // https://github.com/floooh/sokol/issues/818
        // Metal also cannot use large offsets
        size_t bufferOffset = 0;
        size_t attrOffset = 0;

        if (Engine::isOpenGL()){
            bufferOffset = 0;
            attrOffset = offset;
        }else{
            bufferOffset = offset;
            attrOffset = 0;
        }
        
        if (bufferToBindSlot.count({vbuf.id, bufferOffset}) == 0){
            bind.vertex_buffers[bindSlotIndex] = vbuf;
            bind.vertex_buffer_offsets[bindSlotIndex] = bufferOffset;
            bufferToBindSlot[{vbuf.id, bufferOffset}] = bindSlotIndex;

            pipeline_desc.layout.buffers[bindSlotIndex].stride = stride;

            if (perInstance){
                pipeline_desc.layout.buffers[bindSlotIndex].step_func = SG_VERTEXSTEP_PER_INSTANCE;
            }

            bindSlotIndex++;
        }

        size_t indexBuf = bufferToBindSlot[{vbuf.id, bufferOffset}];

        pipeline_desc.layout.attrs[slot].buffer_index = indexBuf;
        pipeline_desc.layout.attrs[slot].offset = attrOffset;
        pipeline_desc.layout.attrs[slot].format = getVertexFormat(elements, dataType, normalized);
    }
}

void SokolObject::replaceVertexBuffer(uint32_t fromBufferId, sg_buffer toBuffer, size_t byteOffset){
    // A single interleaved buffer can occupy several bind slots (one per attribute
    // offset on Metal/D3D), so swap every slot that referenced the original buffer.
    for (auto const& kv : bufferToBindSlot){
        if (kv.first.id == fromBufferId){
            bind.vertex_buffers[kv.second] = toBuffer;
            bind.vertex_buffer_offsets[kv.second] = (int)(kv.first.offset + byteOffset);
        }
    }
}

void SokolObject::setIndexBuffer(sg_buffer buffer){
    bind.index_buffer = buffer;
    bind.index_buffer_offset = 0;
}

void SokolObject::resetIndexBuffer(){
    bind.index_buffer = loadIndexBuffer;
    bind.index_buffer_offset = (int)loadIndexOffset;
}

void SokolObject::addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer){
    if (slot != -1){
        sg_view sbufview = buffer->backend.getView();

        if (stage == ShaderStageType::VERTEX || stage == ShaderStageType::FRAGMENT){
            bind.views[SOKOL_STORAGEBUFFER_VIEW_SLOT_OFFSET + slot] = sbufview;
        }
    }
}

void SokolObject::addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture){
    if (slot.first != -1){
        sg_view texview = texture->backend.getView();
        sg_sampler sampler = texture->backend.getSampler();
        if (stage == ShaderStageType::VERTEX || stage == ShaderStageType::FRAGMENT){
            bind.views[slot.first] = texview;
            bind.samplers[slot.second] = sampler;
        }
    }
}

// The description of one pipeline type, from the load description and face state.
// endLoad creates each pipeline from it, and a no-culling variant only changes its cull mode.
sg_pipeline_desc SokolObject::makePipelineDesc(PipelineType pipType){
    sg_pipeline_desc desc = pipeline_desc;

    // offscreen passes render with a Y-flipped projection on GL (top-left
    // origin), which reverses the triangle winding
    const WindingOrder loadWinding = loadState.windingOrder;
    WindingOrder rttWinding = loadWinding;
    if (Engine::isOpenGL()){
        rttWinding = (loadWinding == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
    }

    switch (pipType){
    case PipelineType::PIP_DEPTH:
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            desc.face_winding = getFaceWinding(loadWinding);
        }
        desc.sample_count = 1;
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        break;

    // Depth prepass: the depth shader writes only depth into the color pass target, so
    // formats and winding follow that target (PIP_DEFAULT or PIP_RTT) rather than PIP_DEPTH.
    case PipelineType::PIP_ZPREPASS:
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        // the depth the color pass tests, offset like it
        desc.depth.bias = loadState.depthBias;
        desc.depth.bias_slope_scale = loadState.depthBiasSlope;
        desc.colors[0].write_mask = SG_COLORMASK_NONE;
        if (loadState.prepassFaceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            desc.face_winding = getFaceWinding(loadWinding);
        }
        break;

    case PipelineType::PIP_ZPREPASS_RTT:
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        desc.depth.bias = loadState.depthBias;
        desc.depth.bias_slope_scale = loadState.depthBiasSlope;
        desc.colors[0].write_mask = SG_COLORMASK_NONE;
        desc.sample_count = 1;
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        if (loadState.prepassFaceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            desc.face_winding = getFaceWinding(rttWinding);
        }
        break;

    case PipelineType::PIP_SHADOW_DEPTH:
        // Depth-only pipeline (no color target); keeps the fragment stage for
        // alpha-mask discard. SG_PIXELFORMAT_NONE marks it depth-only in Sokol.
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            desc.face_winding = getFaceWinding(loadWinding);
        }
        desc.sample_count = 1;
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        desc.color_count = 0;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
        break;

    case PipelineType::PIP_GBUFFER:
        // Mesh geometry pass with three color attachments (MRT):
        //   color[0] = packed depth (same as PIP_DEPTH, so SSAO/SSR depth is unchanged)
        //   color[1] = view-space normal (octahedral .rg) + roughness (.b) + metallic (.a)
        //   color[2] = linear base color (.rgb) + hasIBL flag (.a)
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            desc.face_winding = getFaceWinding(loadWinding);
        }
        desc.sample_count = 1;
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        desc.color_count = 3;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.colors[1].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.colors[2].pixel_format = SG_PIXELFORMAT_RGBA8;
        break;

    case PipelineType::PIP_DEFAULT:
    case PipelineType::PIP_DEFAULT_NODEPTH:
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            desc.face_winding = getFaceWinding(loadWinding);
        }
        desc.colors[0].write_mask = SG_COLORMASK_RGB;
        applyBlend(desc.colors[0], false);
        if (pipType == PipelineType::PIP_DEFAULT){
            desc.depth.compare = getCompareFunc(loadState.depthCompare);
            desc.depth.write_enabled = loadState.depthWrite;
            desc.depth.bias = loadState.depthBias;
            desc.depth.bias_slope_scale = loadState.depthBiasSlope;
        }else{
            desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
            desc.depth.write_enabled = false;
        }
        break;

    // PIP_RTT (offscreen), PIP_RTT_INVERT (planar reflection) and PIP_RTT_NODEPTH share
    // an identical pipeline except for winding and depth state
    case PipelineType::PIP_RTT:
    case PipelineType::PIP_RTT_NODEPTH:
    case PipelineType::PIP_RTT_INVERT:
        desc.sample_count = 1;
        desc.depth.write_enabled = loadState.depthWrite;
        desc.depth.compare = getCompareFunc(loadState.depthCompare);
        desc.depth.bias = loadState.depthBias;
        desc.depth.bias_slope_scale = loadState.depthBiasSlope;
        desc.colors[0].write_mask = SG_COLORMASK_RGBA;
        applyBlend(desc.colors[0], true);
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
            // planar reflection mirrors handedness, reversing winding once more
            WindingOrder winding = rttWinding;
            if (pipType == PipelineType::PIP_RTT_INVERT){
                winding = (rttWinding == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
            }
            desc.face_winding = getFaceWinding(winding);
        }
        if (pipType == PipelineType::PIP_RTT_NODEPTH){
            desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
            desc.depth.write_enabled = false;
        }
        break;
    }

    return desc;
}

sg_pipeline& SokolObject::pipelineOf(PipelineType pipType){
    switch (pipType){
    case PipelineType::PIP_DEPTH: return depth_pip;
    case PipelineType::PIP_ZPREPASS: return zprepass_pip;
    case PipelineType::PIP_ZPREPASS_RTT: return zprepass_rtt_pip;
    case PipelineType::PIP_SHADOW_DEPTH: return shadow_depth_pip;
    case PipelineType::PIP_GBUFFER: return gbuffer_pip;
    case PipelineType::PIP_RTT: return rtt_pip;
    case PipelineType::PIP_RTT_INVERT: return rtt_invert_pip;
    case PipelineType::PIP_DEFAULT_NODEPTH: return nodepth_pip;
    case PipelineType::PIP_RTT_NODEPTH: return rtt_nodepth_pip;
    default: return pip;
    }
}

bool SokolObject::endLoad(uint16_t pipelines, const RenderState& state){
    loadState = state;

    // creation order of the pipeline types
    static const PipelineType types[] = {
        PipelineType::PIP_DEPTH, PipelineType::PIP_ZPREPASS, PipelineType::PIP_ZPREPASS_RTT,
        PipelineType::PIP_SHADOW_DEPTH, PipelineType::PIP_GBUFFER,
        PipelineType::PIP_DEFAULT, PipelineType::PIP_DEFAULT_NODEPTH,
        PipelineType::PIP_RTT, PipelineType::PIP_RTT_NODEPTH, PipelineType::PIP_RTT_INVERT
    };

    for (PipelineType type : types){
        if (!(pipelines & (int)type)){
            continue;
        }
        sg_pipeline& pipeline = pipelineOf(type);
        pipeline = makePipeline(makePipelineDesc(type));
        if (pipeline.id == SG_INVALID_ID){
            return false;
        }
    }

    return true;
}

bool SokolObject::beginDraw(PipelineType pipType, bool disableFaceCulling){
    sg_pipeline selectedPipeline = pipelineOf(pipType);

    // Deferred resource creation can leave an allocated handle in FAILED state.
    // Never apply it: doing so would turn one creation error into a cascade of
    // invalid pipeline, uniform, binding, and draw validation failures.
    if (selectedPipeline.id == SG_INVALID_ID ||
        sg_query_pipeline_state(selectedPipeline) != SG_RESOURCESTATE_VALID){
        return false;
    }

#ifdef JASPERENGINE_EDITOR
    if (disableFaceCulling){
        int index = 0;
        while (index < PIPELINE_TYPES && (1 << index) != (int)pipType){
            index++;
        }
        if (index < PIPELINE_TYPES){
            sg_pipeline& variant = noCullPipelines[index];
            if (variant.id == SG_INVALID_ID && !(noCullSame & (1u << index))){
                sg_pipeline_desc desc = makePipelineDesc(pipType);
                if (desc.cull_mode == SG_CULLMODE_BACK || desc.cull_mode == SG_CULLMODE_FRONT){
                    desc.cull_mode = SG_CULLMODE_NONE;
                    variant = makePipeline(desc);
                }else{
                    // already culls nothing: the pipeline is its own variant
                    noCullSame |= (uint16_t)(1u << index);
                }
            }
            // a variant that failed to build leaves the pipeline as it is
            if (variant.id != SG_INVALID_ID && sg_query_pipeline_state(variant) == SG_RESOURCESTATE_VALID){
                selectedPipeline = variant;
            }
        }
    }
#else
    (void)disableFaceCulling;
#endif

    sg_apply_pipeline(selectedPipeline);
    return true;
}

void SokolObject::applyUniformBlock(int slot, unsigned int count, void* data){
    if (slot != -1){
        sg_apply_uniforms(slot, {data, count});
    }
}

void SokolObject::draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount){
    sg_apply_bindings(bind);
    sg_draw(baseElement, vertexCount, instanceCount);
}

void SokolObject::destroy(){
    if (sg_isvalid()){
        destroyPipeline(pip);
        destroyPipeline(depth_pip);
        destroyPipeline(shadow_depth_pip);
        destroyPipeline(rtt_pip);
        destroyPipeline(rtt_invert_pip);
        destroyPipeline(gbuffer_pip);
        destroyPipeline(nodepth_pip);
        destroyPipeline(rtt_nodepth_pip);
        destroyPipeline(zprepass_pip);
        destroyPipeline(zprepass_rtt_pip);
#ifdef JASPERENGINE_EDITOR
        for (sg_pipeline& variant : noCullPipelines){
            destroyPipeline(variant);
        }
#endif
    }

    pip.id = SG_INVALID_ID;
    depth_pip.id = SG_INVALID_ID;
    shadow_depth_pip.id = SG_INVALID_ID;
    rtt_pip.id = SG_INVALID_ID;
    rtt_invert_pip.id = SG_INVALID_ID;
    gbuffer_pip.id = SG_INVALID_ID;
    nodepth_pip.id = SG_INVALID_ID;
    rtt_nodepth_pip.id = SG_INVALID_ID;
    zprepass_pip.id = SG_INVALID_ID;
    zprepass_rtt_pip.id = SG_INVALID_ID;
    resetPipelines();
    bind = {};
    pipeline_desc = {};
    bindSlotIndex = 0;
}
