// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolObject.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Matrix4.h"
#include "Log.h"
#include "SokolAsyncCreate.h"
#include "SokolCmdQueue.h"
#include "SokolPipelineCache.h"
#include "SokolShader.h"
#include "SokolSystem.h"
#include "Engine.h"
#include "render/SystemRender.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace jasperengine;

namespace {
    // pipelines made and not destroyed yet (SokolObject::getLivePipelineCount)
    std::atomic<int> livePipelines(0);
    // loads with an unfed vertex input (SokolObject::getUnfedVertexInputLoads)
    std::atomic<int> unfedVertexInputLoads(0);
    // a uniform block of the draw being set up did not fit the frame's uniform buffer
    bool uniformsDropped = false;

    // the bit index of a pipeline type
    int pipelineIndex(PipelineType pipType){
        int index = 0;
        while (index < 16 && (1 << index) != (int)pipType){
            index++;
        }
        return index;
    }

    // interned (spec S32 R3): an object with the description of another shares its pipeline,
    // which is created asynchronously where that compiles. Counted per reference
    sg_pipeline makePipeline(const sg_pipeline_desc& desc, bool immediate){
        const sg_pipeline pipeline = SokolPipelineCache::acquire(desc, immediate);
        // an invalid handle (pool full) is never released, so it is not counted
        if (pipeline.id != SG_INVALID_ID)
            livePipelines++;
        return pipeline;
    }

    // draws that waited for a pipeline being created or warmed (spec S32), ever: a pass whose
    // result is kept (a cached shadow slot, a probe capture) draws again when it grew meanwhile
    uint32_t waitingDraws = 0;

    void destroyPipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID)
            return;
        livePipelines--;
        SokolPipelineCache::release(pipeline);
    }

}

SokolObject::SokolObject(){
    for (sg_pipeline& pipeline : pipelines){
        pipeline.id = SG_INVALID_ID;
    }
    msaaSamples = 1;
    bind = {};
    loadIndexBuffer.id = SG_INVALID_ID;
    loadIndexOffset = 0;
    pipeline_desc = {};
    baseDesc = {};
    dynamicLoaded = false;
    dynamicBaseHash = 0;
    lastDynamicPipeline.id = SG_INVALID_ID;
    lastDynamicGeneration = 0;
    loadState = RenderState();
    bindSlotIndex = 0;
    shaderInputs = 0;
    shaderInputSignature = 0;
    blendOverride = false;
    blendState = BlendState();
    resetPipelines();
    resetWarm();
}

void SokolObject::resetWarm(){
    for (int i = 0; i < PIPELINE_TYPES; i++){
        warmedPipeline[i] = SG_INVALID_ID;
        warmKey[i] = 0;
    }
    dynamicWarm.clear();
}

// GL with a worker context (spec S32 R4): the first draw of a pipeline with this object's
// bindings waits for its warm draw on the worker, which makes the driver compile the program for
// this state there. warmed holds the pipeline confirmed, key the warm draw asked for
bool SokolObject::isWarm(sg_pipeline pipeline, uint32_t& warmed, uint64_t& key){
    if (warmed == pipeline.id || immediatePipelines || !SokolAsyncCreate::warmDrawsActive())
        return true;
    if (key == 0)
        key = SokolAsyncCreate::requestWarmDraw(pipeline, bind);
    if (!SokolAsyncCreate::isWarm(key) || !SokolAsyncCreate::beginFirstDraw(pipeline_desc.shader))
        return false;
    warmed = pipeline.id;
    key = 0;
    return true;
}

bool SokolObject::isDynamicWarm(sg_pipeline pipeline){
    if (immediatePipelines || !SokolAsyncCreate::warmDrawsActive())
        return true;
    for (std::pair<uint32_t, uint64_t>& entry : dynamicWarm){
        if (entry.first == pipeline.id){
            if (entry.second == 0)
                return true;
            if (!SokolAsyncCreate::isWarm(entry.second) || !SokolAsyncCreate::beginFirstDraw(baseDesc.shader))
                return false;
            entry.second = 0;
            return true;
        }
    }
    const uint64_t key = SokolAsyncCreate::requestWarmDraw(pipeline, bind);
    // (warm already: the first draw still waits while the worker warms the program)
    dynamicWarm.emplace_back(pipeline.id, key);
    if (!SokolAsyncCreate::isWarm(key) || !SokolAsyncCreate::beginFirstDraw(baseDesc.shader))
        return false;
    dynamicWarm.back().second = 0;
    return true;
}

// Releases this object's references to the shared pipeline variants (SokolPipelineCache
// destroys a variant with its last reference) and forgets which pipelines already cull nothing
void SokolObject::resetPipelines(){
    variants.clear();
    noCullSame = 0;
}

size_t SokolObject::getSharedVariantCount(){
    return SokolPipelineCache::variantCount();
}

int SokolObject::getLivePipelineCount(){
    return livePipelines.load();
}

int SokolObject::getUnfedVertexInputLoads(){
    return unfedVertexInputLoads.load();
}

SokolObject::SokolObject(const SokolObject& rhs) = default;

SokolObject& SokolObject::operator=(const SokolObject& rhs) = default;

SokolVariantRefs::SokolVariantRefs(const SokolVariantRefs& rhs): list(rhs.list){
    const uint64_t context = SystemRender::getContextId();
    for (const Variant& variant : list){
        if (variant.context == context)
            SokolPipelineCache::retainVariant(variant.pipeline);
    }
}

SokolVariantRefs& SokolVariantRefs::operator=(const SokolVariantRefs& rhs){
    if (this != &rhs){
        const uint64_t context = SystemRender::getContextId();
        for (const Variant& variant : rhs.list){
            if (variant.context == context)
                SokolPipelineCache::retainVariant(variant.pipeline);
        }
        clear();
        list = rhs.list;
    }
    return *this;
}

SokolVariantRefs::~SokolVariantRefs(){
    clear();
}

const SokolVariantRefs::Variant* SokolVariantRefs::find(uint8_t kind, uint8_t type) const{
    const uint64_t context = SystemRender::getContextId();
    for (const Variant& variant : list){
        if (variant.kind == kind && variant.type == type && variant.context == context)
            return &variant;
    }
    return nullptr;
}

void SokolVariantRefs::add(uint8_t kind, uint8_t type, sg_pipeline pipeline){
    for (Variant& variant : list){
        if (variant.kind == kind && variant.type == type){
            // one from an earlier context (find skips it): nothing to release
            variant.pipeline = pipeline;
            variant.context = SystemRender::getContextId();
            return;
        }
    }
    list.push_back({kind, type, pipeline, SystemRender::getContextId()});
}

void SokolVariantRefs::clear(){
    const uint64_t context = SystemRender::getContextId();
    for (const Variant& variant : list){
        if (variant.context == context)
            SokolPipelineCache::releaseVariant(variant.pipeline);
    }
    list.clear();
}

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
    if (dataType == AttributeDataType::HALF){
        if (elements == 2)
            return SG_VERTEXFORMAT_HALF2;
        if (elements == 4)
            return SG_VERTEXFORMAT_HALF4;
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

void SokolObject::applyBlend(sg_color_target_state& target, bool offscreen) const{
    if (loadState.blend == BlendMode::NONE){
        if (!offscreen){
            target.blend.enabled = false;
            return;
        }
        // an offscreen target composites coverage in alpha as the legacy pipelines did
        // (below), so an opaque surface writing alpha < 1 does not punch a hole in what
        // is behind it; the colour replaces
        target.blend.enabled = true;
        target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
        target.blend.dst_factor_rgb = SG_BLENDFACTOR_ZERO;
        target.blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        target.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
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
        // the HTML UI effects' blends of premultiplied colour (PipelineState's; alpha below)
        case BlendMode::ADDITIVE_PREMULTIPLIED:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            break;
        case BlendMode::SCREEN_PREMULTIPLIED:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            break;
        case BlendMode::MULTIPLY_PREMULTIPLIED:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::SUBTRACT_PREMULTIPLIED:
            target.blend.op_rgb = SG_BLENDOP_REVERSE_SUBTRACT;
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            target.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
            return;
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

namespace {
    sg_compare_func toSokolStencilCompare(StencilCompare compare){
        switch (compare){
            case StencilCompare::NEVER:         return SG_COMPAREFUNC_NEVER;
            case StencilCompare::LESS:          return SG_COMPAREFUNC_LESS;
            case StencilCompare::EQUAL:         return SG_COMPAREFUNC_EQUAL;
            case StencilCompare::LESS_EQUAL:    return SG_COMPAREFUNC_LESS_EQUAL;
            case StencilCompare::GREATER:       return SG_COMPAREFUNC_GREATER;
            case StencilCompare::NOT_EQUAL:     return SG_COMPAREFUNC_NOT_EQUAL;
            case StencilCompare::GREATER_EQUAL: return SG_COMPAREFUNC_GREATER_EQUAL;
            default:                            return SG_COMPAREFUNC_ALWAYS;
        }
    }

    sg_stencil_op toSokolStencilOp(StencilOp op){
        switch (op){
            case StencilOp::ZERO:       return SG_STENCILOP_ZERO;
            case StencilOp::REPLACE:    return SG_STENCILOP_REPLACE;
            case StencilOp::INCR_CLAMP: return SG_STENCILOP_INCR_CLAMP;
            case StencilOp::DECR_CLAMP: return SG_STENCILOP_DECR_CLAMP;
            case StencilOp::INVERT:     return SG_STENCILOP_INVERT;
            case StencilOp::INCR_WRAP:  return SG_STENCILOP_INCR_WRAP;
            case StencilOp::DECR_WRAP:  return SG_STENCILOP_DECR_WRAP;
            default:                    return SG_STENCILOP_KEEP;
        }
    }
}

void SokolObject::applyStencilAndColorMask(sg_pipeline_desc& desc, bool offscreen) const{
    if (loadState.stencilEnabled){
        sg_stencil_face_state face = {};
        face.compare = toSokolStencilCompare(loadState.stencilCompare);
        face.fail_op = toSokolStencilOp(loadState.stencilFail);
        face.depth_fail_op = toSokolStencilOp(loadState.stencilDepthFail);
        face.pass_op = toSokolStencilOp(loadState.stencilPass);
        desc.stencil.enabled = true;
        desc.stencil.front = face;
        desc.stencil.back = face;
        desc.stencil.read_mask = loadState.stencilReadMask;
        desc.stencil.write_mask = loadState.stencilWriteMask;
        desc.stencil.ref = loadState.stencilRef;
    }

    // the swapchain keeps its alpha (RGB only, as before the mask was configurable)
    int mask = loadState.colorWriteMask & (offscreen ? COLOR_WRITE_ALL : (COLOR_WRITE_R | COLOR_WRITE_G | COLOR_WRITE_B));
    // ColorWriteBits are sokol's channel bits; 0 is sokol's "default" (all), so none is explicit
    desc.colors[0].write_mask = (mask == 0) ? SG_COLORMASK_NONE : (sg_color_mask)mask;
}

void SokolObject::beginLoad(PrimitiveType primitiveType){
    // forgets every pipeline without destroying it (a copy may share them; destroy() releases
    // a load's own), so what endLoad does not make again is never drawn or destroyed stale
    bind = {0};
    for (sg_pipeline& pipeline : pipelines){
        pipeline = {0};
    }
    pipeline_desc = {0};
    baseDesc = {};
    dynamicLoaded = false;
    dynamicBaseHash = 0;
    lastDynamicPipeline.id = SG_INVALID_ID;
    lastDynamicGeneration = 0;
    resetPipelines();

    pipeline_desc.primitive_type = getPrimitiveType(primitiveType);

    bindSlotIndex = 0;
    bufferToBindSlot.clear();
    shaderInputs = 0;
    shaderInputSignature = 0;

    blendOverride = false;
    blendState = BlendState();
}

namespace {
    // the vertex inputs of a shader, each name at its location (FNV-1a), whatever their order
    uint64_t vertexInputSignature(const ShaderData& data){
        uint64_t signature = 0;
        for (const ShaderStage& stage : data.stages){
            if (stage.type != ShaderStageType::VERTEX)
                continue;
            for (const ShaderAttr& attr : stage.attributes){
                uint64_t h = 1469598103934665603ull ^ (uint64_t)(uint32_t)attr.location;
                for (const char c : attr.name)
                    h = (h ^ (uint8_t)c) * 1099511628211ull;
                signature += h * 1099511628211ull;
            }
        }
        return signature;
    }
}

void SokolObject::setShader(ShaderRender* shader){
    pipeline_desc.shader = shader->backend.get();
    shaderInputs = 0;
    for (const ShaderStage& stage : shader->shaderData.stages){
        if (stage.type != ShaderStageType::VERTEX)
            continue;
        for (const ShaderAttr& attr : stage.attributes){
            if (attr.location >= 0 && attr.location < SG_MAX_VERTEX_ATTRIBUTES)
                shaderInputs |= (1u << attr.location);
        }
    }
    shaderInputSignature = vertexInputSignature(shader->shaderData);
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
    addAttribute(slot, buffer, elements, dataType, stride, offset, normalized, perInstance, false);
}

void SokolObject::addInterleavedAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance){
    addAttribute(slot, buffer, elements, dataType, stride, offset, normalized, perInstance, true);
}

void SokolObject::addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance, bool interleaved){
    if (slot != -1){
        sg_buffer vbuf = buffer->backend.get();

        // D3D11 cannot have offset (AlignedByteOffset) bigger than 2048
        // https://github.com/floooh/sokol/issues/818
        // Metal also cannot use large offsets
        size_t bufferOffset = 0;
        size_t attrOffset = 0;

        if (Engine::isOpenGL() || interleaved){
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

void SokolObject::setIndexBuffer(sg_buffer buffer, size_t byteOffset){
    bind.index_buffer = buffer;
    bind.index_buffer_offset = (int)byteOffset;
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

void SokolObject::addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture, TextureRender* samplerOf){
    if (slot.first != -1){
        if (stage == ShaderStageType::VERTEX || stage == ShaderStageType::FRAGMENT){
            bind.views[slot.first] = texture->backend.getView();
            bind.samplers[slot.second] = samplerOf->backend.getSampler();
        }
    }
}

// The description of one pipeline type, from the load description and face state.
// endLoad creates each pipeline from it, and a no-culling variant only changes its cull mode.
sg_blend_factor SokolObject::getBlendFactor(BlendFactor factor){
    switch (factor){
    case BlendFactor::ZERO: return SG_BLENDFACTOR_ZERO;
    case BlendFactor::ONE: return SG_BLENDFACTOR_ONE;
    case BlendFactor::SRC_COLOR: return SG_BLENDFACTOR_SRC_COLOR;
    case BlendFactor::ONE_MINUS_SRC_COLOR: return SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SRC_ALPHA: return SG_BLENDFACTOR_SRC_ALPHA;
    case BlendFactor::ONE_MINUS_SRC_ALPHA: return SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DST_COLOR: return SG_BLENDFACTOR_DST_COLOR;
    case BlendFactor::ONE_MINUS_DST_COLOR: return SG_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DST_ALPHA: return SG_BLENDFACTOR_DST_ALPHA;
    case BlendFactor::ONE_MINUS_DST_ALPHA: return SG_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    }
    return _SG_BLENDFACTOR_DEFAULT;
}

sg_blend_op SokolObject::getBlendOp(BlendOperation operation){
    switch (operation){
    case BlendOperation::ADD: return SG_BLENDOP_ADD;
    case BlendOperation::SUBTRACT: return SG_BLENDOP_SUBTRACT;
    case BlendOperation::REVERSE_SUBTRACT: return SG_BLENDOP_REVERSE_SUBTRACT;
    }
    return _SG_BLENDOP_DEFAULT;
}

bool SokolObject::isReady() const{
    for (const sg_pipeline& pipeline : pipelines){
        if (pipeline.id != SG_INVALID_ID && sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID)
            return false;
    }
    return true;
}

// The pipelines this object will have on another variant of its shader (spec S32 R4, R8), made
// ahead with its description and warmed with its bindings: it keeps drawing until they are ready,
// and once it loads again on that variant its pipelines are these (interned), so it switches
// without a frame left empty. Held until destroy()
bool SokolObject::prewarmWith(ShaderRender* shader){
    if (immediatePipelines || !shader)
        return true;
    const sg_shader target = shader->backend.get();
    if (target.id == SG_INVALID_ID || target.id == pipeline_desc.shader.id || sg_query_shader_state(target) != SG_RESOURCESTATE_VALID)
        return true;
    // A variant with other vertex inputs (the right variant of an object that loaded on a
    // stand-in: a_tangent where the stand-in had a_color) needs the layout its own load binds.
    // Made on this one, its pipelines read inputs no buffer feeds: zero on GL, undefined on
    // Vulkan, and Metal fails them ("Vertex attribute a_color(3) is missing from the vertex
    // descriptor", SplineGpu.RoadOnTerrain). Nothing to make ahead: the load makes them.
    if (vertexInputSignature(shader->shaderData) != shaderInputSignature)
        return true;
    // what was made for another variant goes (the shader asked for changed meanwhile)
    for (size_t i = 0; i < prewarmed.size();){
        if (prewarmed[i].shader != target.id){
            destroyPipeline(prewarmed[i].pipeline);
            prewarmed.erase(prewarmed.begin() + (std::ptrdiff_t)i);
        }else{
            i++;
        }
    }
    const bool warmDraws = SokolAsyncCreate::warmDrawsActive();
    bool ready = true;
    for (int index = 0; index < PIPELINE_TYPES; index++){
        if (pipelines[index].id == SG_INVALID_ID || sg_query_pipeline_state(pipelines[index]) != SG_RESOURCESTATE_VALID)
            continue;
        Prewarmed* entry = nullptr;
        for (Prewarmed& made : prewarmed){
            if (made.index == index)
                entry = &made;
        }
        if (!entry){
            sg_pipeline_desc desc = makePipelineDesc((PipelineType)(1 << index));
            desc.shader = target;
            const sg_pipeline pipeline = makePipeline(desc, false);
            if (pipeline.id == SG_INVALID_ID)
                continue;
            prewarmed.push_back(Prewarmed{target.id, index, pipeline});
            entry = &prewarmed.back();
        }
        const sg_resource_state state = sg_query_pipeline_state(entry->pipeline);
        if (state == SG_RESOURCESTATE_ALLOC){
            ready = false;
        }else if (state == SG_RESOURCESTATE_VALID && warmDraws &&
                !SokolAsyncCreate::isWarm(SokolAsyncCreate::requestWarmDraw(entry->pipeline, bind))){
            ready = false;
        }
        // (a failed one is not waited for: the load shows it)
    }
    return ready;
}

bool SokolObject::isDrawWaiting() const{
    return drawWaiting;
}

uint32_t SokolObject::getWaitingDrawCount(){
    return waitingDraws;
}

uint32_t SokolObject::getPipelineId(PipelineType pipType) const{
    const int index = pipelineIndex(pipType);
    return index < PIPELINE_TYPES ? pipelines[index].id : (uint32_t)SG_INVALID_ID;
}

void SokolObject::setImmediatePipelines(bool immediate){
    immediatePipelines = immediate;
}

void SokolObject::setBlendState(const BlendState& state){
    blendOverride = true;
    blendState = state;
}

sg_pipeline_desc SokolObject::makePipelineDesc(PipelineType pipType) const{
    sg_pipeline_desc desc = pipeline_desc;

    // offscreen passes render with a Y-flipped projection on GL (top-left
    // origin), which reverses the triangle winding.
    // The winding is set even without culling: it also decides gl_FrontFacing, which a
    // double-sided material shades its back with (sokol's default is CW, so a CCW mesh drawn
    // to the screen would have its front faces lit as backs)
    const WindingOrder loadWinding = loadState.windingOrder;
    WindingOrder rttWinding = loadWinding;
    if (Engine::isOpenGL()){
        rttWinding = (loadWinding == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
    }

    switch (pipType){
    case PipelineType::PIP_DEPTH:
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
        }
        desc.face_winding = getFaceWinding(loadWinding);
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
        }
        desc.face_winding = getFaceWinding(loadWinding);
        break;

    case PipelineType::PIP_ZPREPASS_RTT:
    case PipelineType::PIP_ZPREPASS_MSAA:
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        desc.depth.bias = loadState.depthBias;
        desc.depth.bias_slope_scale = loadState.depthBiasSlope;
        desc.colors[0].write_mask = SG_COLORMASK_NONE;
        desc.sample_count = (pipType == PipelineType::PIP_ZPREPASS_MSAA) ? msaaSamples : 1;
        // the offscreen colour pass it draws into has a depth-stencil attachment
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        if (loadState.prepassFaceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
        }
        desc.face_winding = getFaceWinding(rttWinding);
        break;

    case PipelineType::PIP_SHADOW_DEPTH:
        // Depth-only pipeline (no color target); keeps the fragment stage for
        // alpha-mask discard. SG_PIXELFORMAT_NONE marks it depth-only in Sokol.
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
        }
        desc.face_winding = getFaceWinding(loadWinding);
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
        }
        desc.face_winding = getFaceWinding(loadWinding);
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
        }
        desc.face_winding = getFaceWinding(loadWinding);
        applyStencilAndColorMask(desc, false);
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
    case PipelineType::PIP_MSAA:
    case PipelineType::PIP_MSAA_NODEPTH:
        // the multisampled scene target is PIP_RTT's at its sample count
        desc.sample_count = (pipType == PipelineType::PIP_MSAA || pipType == PipelineType::PIP_MSAA_NODEPTH) ? msaaSamples : 1;
        desc.depth.write_enabled = loadState.depthWrite;
        desc.depth.compare = getCompareFunc(loadState.depthCompare);
        desc.depth.bias = loadState.depthBias;
        desc.depth.bias_slope_scale = loadState.depthBiasSlope;
        applyStencilAndColorMask(desc, true);
        applyBlend(desc.colors[0], true);
        // offscreen colour passes (cameras, editor views, effects) have a depth-stencil
        // attachment, see SokolFramebuffer
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
        }
        {
            // planar reflection mirrors handedness, reversing winding once more
            WindingOrder winding = rttWinding;
            if (pipType == PipelineType::PIP_RTT_INVERT){
                winding = (rttWinding == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
            }
            desc.face_winding = getFaceWinding(winding);
        }
        if (pipType == PipelineType::PIP_RTT_NODEPTH || pipType == PipelineType::PIP_MSAA_NODEPTH){
            desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
            desc.depth.write_enabled = false;
        }
        break;

    // a fullscreen effect into a target with a depth-only attachment (FramebufferDepth::DEPTH):
    // PIP_RTT's colour state; the depth is never tested or written, there is no stencil
    case PipelineType::PIP_EFFECT:
        desc.sample_count = 1;
        applyBlend(desc.colors[0], true);
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
        desc.depth.write_enabled = false;
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
        }
        desc.face_winding = getFaceWinding(rttWinding);
        break;

    // a fullscreen pass of the bloom chain: an SRGB8A8 target without depth, written whole
    case PipelineType::PIP_POST:
        desc.sample_count = 1;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_SRGB8A8;
        desc.colors[0].blend.enabled = false;
        desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
        desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
        desc.depth.write_enabled = false;
        desc.face_winding = getFaceWinding(rttWinding);
        break;

    // a fullscreen pass into a face and level of an RGBA8 texture (a pass with only a colour
    // attachment view, SokolCamera), written whole
    case PipelineType::PIP_RTT_COLORONLY:
        desc.sample_count = 1;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.colors[0].blend.enabled = false;
        desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
        desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
        desc.depth.write_enabled = false;
        desc.face_winding = getFaceWinding(rttWinding);
        break;
    }

    // a blend state set at load replaces the color pipelines' blending (the render state's
    // blend); the write mask stays as the pipeline type and the render state set it,
    // depth-only, G-buffer and bloom-chain (PIP_POST, never blended) passes ignore it
    if (blendOverride){
        switch (pipType){
        case PipelineType::PIP_DEFAULT:
        case PipelineType::PIP_DEFAULT_NODEPTH:
        case PipelineType::PIP_RTT:
        case PipelineType::PIP_RTT_NODEPTH:
        case PipelineType::PIP_RTT_INVERT:
        case PipelineType::PIP_MSAA:
        case PipelineType::PIP_MSAA_NODEPTH:
        case PipelineType::PIP_EFFECT:
            desc.colors[0].blend.enabled = blendState.enabled;
            desc.colors[0].blend.src_factor_rgb = getBlendFactor(blendState.srcFactorRGB);
            desc.colors[0].blend.dst_factor_rgb = getBlendFactor(blendState.dstFactorRGB);
            desc.colors[0].blend.op_rgb = getBlendOp(blendState.opRGB);
            desc.colors[0].blend.src_factor_alpha = getBlendFactor(blendState.srcFactorAlpha);
            desc.colors[0].blend.dst_factor_alpha = getBlendFactor(blendState.dstFactorAlpha);
            desc.colors[0].blend.op_alpha = getBlendOp(blendState.opAlpha);
            break;
        default:
            break;
        }
    }

    return desc;
}

sg_pipeline& SokolObject::pipelineOf(PipelineType pipType){
    const int index = pipelineIndex(pipType);
    return pipelines[index < PIPELINE_TYPES ? index : 0];
}

// An input the vertex shader declares with no buffer behind it reads zero on OpenGL but is
// undefined on Vulkan (and invalid on Metal and D3D11): a morph mesh with fewer targets than
// the shader declares drew nothing on NVIDIA once earlier draws had left garbage there.
// The callers bind every input (RenderSystem::bindUnfedMorphInputs); one left out is
// reported here, and counted for the tests.
void SokolObject::checkVertexInputs(){
    uint32_t unfed = 0;
    for (int slot = 0; slot < SG_MAX_VERTEX_ATTRIBUTES; slot++){
        if ((shaderInputs & (1u << slot)) && pipeline_desc.layout.attrs[slot].format == SG_VERTEXFORMAT_INVALID)
            unfed |= (1u << slot);
    }
    if (unfed){
        unfedVertexInputLoads++;
        Log::warn("A pipeline leaves vertex inputs of its shader (locations mask 0x%x) without a buffer: undefined on Vulkan", unfed);
    }
}

bool SokolObject::endLoad(uint16_t types, const RenderState& state, int samples){
    loadState = state;
    checkVertexInputs();
    // the PIP_MSAA* types' sample count, given by the RenderSystem loading the object (never
    // shared state: another scene or the editor's thumbnail thread loads with its own)
    msaaSamples = samples > 1 ? samples : 1;

    // creation order of the pipeline types
    static const PipelineType order[] = {
        PipelineType::PIP_DEPTH, PipelineType::PIP_ZPREPASS, PipelineType::PIP_ZPREPASS_RTT,
        PipelineType::PIP_SHADOW_DEPTH, PipelineType::PIP_GBUFFER,
        PipelineType::PIP_DEFAULT, PipelineType::PIP_DEFAULT_NODEPTH,
        PipelineType::PIP_RTT, PipelineType::PIP_RTT_NODEPTH, PipelineType::PIP_RTT_INVERT,
        PipelineType::PIP_EFFECT, PipelineType::PIP_POST,
        PipelineType::PIP_ZPREPASS_MSAA, PipelineType::PIP_MSAA, PipelineType::PIP_MSAA_NODEPTH,
        PipelineType::PIP_RTT_COLORONLY
    };

    for (PipelineType type : order){
        if (!(types & (int)type)){
            continue;
        }
        sg_pipeline& pipeline = pipelineOf(type);
        pipeline = makePipeline(makePipelineDesc(type), immediatePipelines);
        if (pipeline.id == SG_INVALID_ID){
            return false;
        }
    }

    return true;
}

bool SokolObject::endLoadDynamic(PrimitiveType primitiveType, IndexType indexType){
    checkVertexInputs();
    baseDesc = pipeline_desc;
    baseDesc.primitive_type = getPrimitiveType(primitiveType);
    switch (indexType){
    case IndexType::UINT16: baseDesc.index_type = SG_INDEXTYPE_UINT16; break;
    case IndexType::UINT32: baseDesc.index_type = SG_INDEXTYPE_UINT32; break;
    default: baseDesc.index_type = SG_INDEXTYPE_NONE; break;
    }
    baseDesc.label = "dynamic-pipeline";
    dynamicLoaded = baseDesc.shader.id != SG_INVALID_ID;
    dynamicBaseHash = SokolPipelineCache::baseHash(baseDesc);
    lastDynamicPipeline.id = SG_INVALID_ID;
    return dynamicLoaded;
}

bool SokolObject::loadAlike(const SokolObject& source, ShaderRender* shader){
    beginLoad(PrimitiveType::TRIANGLES);
    bind = source.bind;
    loadIndexBuffer = source.loadIndexBuffer;
    loadIndexOffset = source.loadIndexOffset;
    bufferToBindSlot = source.bufferToBindSlot;
    bindSlotIndex = source.bindSlotIndex;
    pipeline_desc = source.pipeline_desc;
    loadState = source.loadState;
    msaaSamples = source.msaaSamples;
    blendOverride = source.blendOverride;
    blendState = source.blendState;
    immediatePipelines = source.immediatePipelines;
    setShader(shader);
    checkVertexInputs();
    return pipeline_desc.shader.id != SG_INVALID_ID;
}

void SokolObject::shareBindings(const SokolObject& source){
    bind = source.bind;
}

bool SokolObject::beginAccumulationDraw(PipelineType colorType, bool mirrored){
    uniformsDropped = false; // a new draw: its uniforms come next
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    drawWaiting = false;
    if (pipeline_desc.shader.id == SG_INVALID_ID || sg_query_shader_state(pipeline_desc.shader) != SG_RESOURCESTATE_VALID){
        drawWaiting = pipeline_desc.shader.id != SG_INVALID_ID && sg_query_shader_state(pipeline_desc.shader) == SG_RESOURCESTATE_ALLOC;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }

    const int index = pipelineIndex(colorType);
    const uint8_t kind = mirrored ? VARIANT_ACCUMULATE_MIRRORED : VARIANT_ACCUMULATE;
    const SokolVariantRefs::Variant* held = variants.find(kind, (uint8_t)index);
    if (!held){
        sg_pipeline_desc desc = makePipelineDesc(colorType);
        desc.color_count = 2;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
        desc.colors[1].pixel_format = SG_PIXELFORMAT_R16F;
        // one blend state for both targets (GL and GLES3 have no other): the colour and the
        // weight sum, the alpha keeps the product of (1 - alpha)
        for (int c = 0; c < 2; c++){
            sg_blend_state& blend = desc.colors[c].blend;
            blend.enabled = true;
            blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.op_rgb = SG_BLENDOP_ADD;
            blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            blend.op_alpha = SG_BLENDOP_ADD;
            desc.colors[c].write_mask = SG_COLORMASK_RGBA;
        }
        desc.depth.write_enabled = false;
        desc.stencil = {};
        if (mirrored){
            desc.face_winding = (desc.face_winding == SG_FACEWINDING_CCW) ? SG_FACEWINDING_CW : SG_FACEWINDING_CCW;
        }
        desc.label = "oit-accumulation-pipeline";
        variants.add(kind, (uint8_t)index, SokolPipelineCache::acquireVariant(desc));
        held = variants.find(kind, (uint8_t)index);
    }
    if (!held || held->pipeline.id == SG_INVALID_ID)
        return false;
    const sg_resource_state state = sg_query_pipeline_state(held->pipeline);
    if (state != SG_RESOURCESTATE_VALID){
        drawWaiting = state == SG_RESOURCESTATE_ALLOC;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }
    if (!isDynamicWarm(held->pipeline)){
        drawWaiting = true;
        waitingDraws++;
        return false;
    }
    sg_apply_pipeline(held->pipeline);
    SokolPipelineCache::invalidateApplied();
    return true;
}

bool SokolObject::beginDraw(const PipelineState& state){
    uniformsDropped = false; // a new draw: its uniforms come next
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    drawWaiting = false;
    if (!dynamicLoaded || sg_query_shader_state(baseDesc.shader) != SG_RESOURCESTATE_VALID){
        drawWaiting = dynamicLoaded && sg_query_shader_state(baseDesc.shader) == SG_RESOURCESTATE_ALLOC;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }

    sg_pipeline pipeline;
    const uint32_t generation = SokolPipelineCache::generation();
    if (lastDynamicPipeline.id != SG_INVALID_ID && lastDynamicGeneration == generation && lastDynamicState == state){
        pipeline = lastDynamicPipeline;
    }else{
        pipeline = SokolPipelineCache::get(baseDesc, dynamicBaseHash, state, immediatePipelines);
        lastDynamicPipeline = pipeline;
        lastDynamicState = state;
        lastDynamicGeneration = generation;
    }
    // a failed creation is never applied (see beginDraw(PipelineType)), nor one still being
    // created (spec S32) or warmed
    if (pipeline.id == SG_INVALID_ID || sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID){
        drawWaiting = pipeline.id != SG_INVALID_ID && sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_ALLOC;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }
    if (!isDynamicWarm(pipeline)){
        drawWaiting = true;
        waitingDraws++;
        return false;
    }

    SokolPipelineCache::apply(pipeline);
    return true;
}

bool SokolObject::beginDraw(PipelineType pipType, bool disableFaceCulling, bool mirrored){
    uniformsDropped = false; // a new draw: its uniforms come next
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    sg_pipeline selectedPipeline = pipelineOf(pipType);
    drawWaiting = false;

    // Deferred resource creation can leave an allocated handle in FAILED state.
    // Never apply it: doing so would turn one creation error into a cascade of
    // invalid pipeline, uniform, binding, and draw validation failures. One still being
    // created (spec S32: off the render thread, or a queued command) only waits.
    if (selectedPipeline.id == SG_INVALID_ID ||
        sg_query_pipeline_state(selectedPipeline) != SG_RESOURCESTATE_VALID){
        drawWaiting = selectedPipeline.id != SG_INVALID_ID && sg_query_pipeline_state(selectedPipeline) == SG_RESOURCESTATE_ALLOC;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }
    // A pipeline outlives the shader it was built on when the shader goes before the objects
    // drawing with it load again (a shader reloaded or released while a mesh keeps its old
    // pipelines for a frame): sokol keeps such a pipeline valid, and applying it and its bindings
    // reads the dead shader (a crash in sg_apply_bindings without the validation layer). Such an
    // object draws nothing until its reload builds pipelines on the live shader.
    if (pipeline_desc.shader.id == SG_INVALID_ID ||
        sg_query_shader_state(pipeline_desc.shader) != SG_RESOURCESTATE_VALID){
        return false;
    }

#ifndef JASPERENGINE_EDITOR
    disableFaceCulling = false;
#endif

    const int index = pipelineIndex(pipType);
    if ((disableFaceCulling || mirrored) && index < PIPELINE_TYPES){
        const int kind = mirrored ? (disableFaceCulling ? VARIANT_MIRRORED_NO_CULL : VARIANT_MIRRORED) : VARIANT_NO_CULL;
        const bool sameAsPipeline = (kind == VARIANT_NO_CULL) && (noCullSame & (1u << index));
        const SokolVariantRefs::Variant* held = variants.find((uint8_t)kind, (uint8_t)index);
        if (!held && !sameAsPipeline){
            sg_pipeline_desc desc = makePipelineDesc(pipType);
            const bool culls = desc.cull_mode == SG_CULLMODE_BACK || desc.cull_mode == SG_CULLMODE_FRONT;
            if (disableFaceCulling){
                desc.cull_mode = SG_CULLMODE_NONE;
            }
            if (mirrored){
                // the winding of the front faces on screen turns with the mirror (sokol's
                // default winding is CW)
                desc.face_winding = (desc.face_winding == SG_FACEWINDING_CCW) ? SG_FACEWINDING_CW : SG_FACEWINDING_CCW;
            }
            if (kind == VARIANT_NO_CULL && !culls){
                // already culls nothing: the pipeline is its own variant
                noCullSame |= (uint16_t)(1u << index);
            }else{
                // shared by every object with the same description, an interned pipeline made
                // through the asynchronous creation (spec S32; a failed one is kept as invalid,
                // so it is not tried again each frame)
                const sg_pipeline variant = SokolPipelineCache::acquireVariant(desc);
                // the passes of the frame wait for theirs (setImmediatePipelines)
                if (immediatePipelines && !Engine::isAsyncThread() && SokolAsyncCreate::isPending(variant))
                    SokolAsyncCreate::waitFor(variant);
                variants.add((uint8_t)kind, (uint8_t)index, variant);
                held = variants.find((uint8_t)kind, (uint8_t)index);
            }
        }
        if (held && held->pipeline.id != SG_INVALID_ID && sg_query_pipeline_state(held->pipeline) == SG_RESOURCESTATE_VALID){
            selectedPipeline = held->pipeline;
        }else if (mirrored && held && held->pipeline.id != SG_INVALID_ID && sg_query_pipeline_state(held->pipeline) == SG_RESOURCESTATE_ALLOC){
            // a mirrored draw waits for its variant (the plain pipeline would cull its front faces)
            drawWaiting = true;
            waitingDraws++;
            return false;
        }
        // a variant that failed to build (or a no-cull one still being made) leaves the pipeline as it is
    }

    {
        const int index = pipelineIndex(pipType);
        if (index < PIPELINE_TYPES && !isWarm(selectedPipeline, warmedPipeline[index], warmKey[index])){
            drawWaiting = true;
            waitingDraws++;
            return false;
        }
    }

    sg_apply_pipeline(selectedPipeline);
    // the dynamic path's "applied last" no longer holds
    SokolPipelineCache::invalidateApplied();
    return true;
}

void SokolObject::applyUniformBlock(int slot, unsigned int count, void* data){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    if (slot != -1){
        // past the frame's uniform buffer (Vulkan, Metal, WebGPU): not applied, and the draw it
        // is for is skipped (SokolSystem logs it), not drawn with another draw's uniforms
        if (!SokolSystem::reserveUniformBytes(count)){
            uniformsDropped = true;
            return;
        }
        sg_apply_uniforms(slot, {data, count});
    }
}

bool SokolObject::draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    if (uniformsDropped){
        uniformsDropped = false;
        SokolSystem::countSkippedDraw();
        return false;
    }
    // past the frame's descriptor buffer (Vulkan): skipped (SokolSystem counts and logs it),
    // where sokol would drop it and every draw after it in the frame
    if (!SokolSystem::reserveDescriptorBytes())
        return false;
    sg_apply_bindings(bind);
    sg_draw(baseElement, vertexCount, instanceCount);
    return true;
}

void SokolObject::destroy(){
    // let go of its pipelines and their shared variants (resetPipelines): each is destroyed
    // with its last holder
    if (sg_isvalid()){
        for (sg_pipeline& pipeline : pipelines){
            destroyPipeline(pipeline);
        }
        for (Prewarmed& made : prewarmed){
            destroyPipeline(made.pipeline);
        }
    }
    prewarmed.clear();

    for (sg_pipeline& pipeline : pipelines){
        pipeline.id = SG_INVALID_ID;
    }
    resetPipelines();
    resetWarm();
    bind = {};
    pipeline_desc = {};
    // dynamic pipelines stay in SokolPipelineCache until their shader goes
    baseDesc = {};
    dynamicLoaded = false;
    dynamicBaseHash = 0;
    lastDynamicPipeline.id = SG_INVALID_ID;
    lastDynamicGeneration = 0;
    bindSlotIndex = 0;
    blendOverride = false;
    blendState = BlendState();
}
