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
#include "profiler/Profile.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace jasperengine;

namespace {
    // binds and draws the draw set up (its pipeline and uniforms applied): its bindings only when
    // they are not the ones applied last (SokolSystem); false when the frame's descriptor buffer
    // has no room for it (Vulkan: skipped and counted)
    // a draw that reads no buffer, image or sampler (a fullscreen triangle from gl_VertexIndex and
    // uniforms only): sokol's validation rejects an empty sg_apply_bindings, and such a pipeline
    // requires none
    bool sokolObjectBindingsEmpty(const sg_bindings& bind){
        if (bind.index_buffer.id != SG_INVALID_ID) return false;
        for (const sg_buffer& buffer : bind.vertex_buffers)
            if (buffer.id != SG_INVALID_ID) return false;
        for (const sg_view& view : bind.views)
            if (view.id != SG_INVALID_ID) return false;
        for (const sg_sampler& sampler : bind.samplers)
            if (sampler.id != SG_INVALID_ID) return false;
        return true;
    }

    bool bindAndDraw(const sg_bindings& bind, int baseElement, int numElements, int numInstances){
        const bool rebind = !sokolObjectBindingsEmpty(bind) && !SokolSystem::isBindingsApplied(bind);
        if (!SokolSystem::reserveDescriptorBytes(rebind))
            return false;
        if (rebind)
            SokolSystem::applyBindings(bind);
        SokolSystem::draw(baseElement, numElements, numInstances);
        return true;
    }

    // pipelines made and not destroyed yet (SokolObject::getLivePipelineCount)
    std::atomic<int> livePipelines(0);
    // loads with an unfed vertex input (SokolObject::getUnfedVertexInputLoads)
    std::atomic<int> unfedVertexInputLoads(0);
    // objects holding a load's working description (SokolObject::getLoadingDescCount)
    std::atomic<int> loadingDescs(0);
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
    shader = {};
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
    std::fill(std::begin(batchUniformSlots), std::end(batchUniformSlots), (int8_t)-1);
    resetPipelines();
    resetWarm();
}

void SokolObject::resetWarm(){
    warm.reset();
    dynamicWarm.clear();
}

// GL with a worker context (spec S32 R4): the first draw of a pipeline with this object's
// bindings waits for its warm draw on the worker, which makes the driver compile the program for
// this state there. warmed holds the pipeline confirmed, key the warm draw asked for
bool SokolObject::isWarm(sg_pipeline pipeline, int index, bool lodFade, sg_shader warmShader){
    if (immediatePipelines || !SokolAsyncCreate::warmDrawsActive())
        return true;
    WarmTable& table = warm.make();
    uint32_t& warmed = lodFade ? table.lodFadePipeline[index] : table.pipeline[index];
    uint64_t& key = lodFade ? table.lodFadeKey[index] : table.key[index];
    if (warmed == pipeline.id)
        return true;
    if (key == 0)
        key = SokolAsyncCreate::requestWarmDraw(pipeline, bind);
    if (!SokolAsyncCreate::isWarm(key) || !SokolAsyncCreate::beginFirstDraw(warmShader))
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
            if (!SokolAsyncCreate::isWarm(entry.second) || !SokolAsyncCreate::beginFirstDraw(shader))
                return false;
            entry.second = 0;
            return true;
        }
    }
    const uint64_t key = SokolAsyncCreate::requestWarmDraw(pipeline, bind);
    // (warm already: the first draw still waits while the worker warms the program)
    dynamicWarm.emplace_back(pipeline.id, key);
    if (!SokolAsyncCreate::isWarm(key) || !SokolAsyncCreate::beginFirstDraw(shader))
        return false;
    dynamicWarm.back().second = 0;
    return true;
}

// Releases this object's references to the shared pipeline variants (SokolPipelineCache
// destroys a variant with its last reference) and forgets which pipelines already cull nothing
void SokolObject::resetPipelines(){
    variants.clear();
    noCullSame = 0;
    lodFadeShaderId = SG_INVALID_ID;
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

int SokolObject::getLoadingDescCount(){
    return loadingDescs.load();
}

SokolObject::SokolObject(const SokolObject& rhs) = default;

SokolObject& SokolObject::operator=(const SokolObject& rhs) = default;

SokolObject::LoadingDesc::LoadingDesc(){
    loadingDescs++;
}

SokolObject::LoadingDesc::LoadingDesc(const LoadingDesc& other): desc(other.desc){
    loadingDescs++;
}

SokolObject::LoadingDesc::~LoadingDesc(){
    loadingDescs--;
}

// only from beginLoad to endLoad: made when a load writes it first
sg_pipeline_desc& SokolObject::loadingDesc(){
    return loadDesc.make().desc;
}

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

void SokolVariantRefs::removeFrom(uint8_t kind){
    const uint64_t context = SystemRender::getContextId();
    for (size_t i = 0; i < list.size();){
        if (list[i].kind >= kind){
            if (list[i].context == context)
                SokolPipelineCache::releaseVariant(list[i].pipeline);
            list.erase(list.begin() + (std::ptrdiff_t)i);
        }else{
            i++;
        }
    }
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
        // Title Art's light blends (Render.h): their own alpha, offscreen or not
        case BlendMode::LIGHT_PREMULTIPLIED:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            return;
        case BlendMode::MAX_PREMULTIPLIED:
            target.blend.op_rgb = SG_BLENDOP_MAX;
            target.blend.op_alpha = SG_BLENDOP_MAX;
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            target.blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            target.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
            return;
        case BlendMode::ERASE_PREMULTIPLIED:
            target.blend.src_factor_rgb = SG_BLENDFACTOR_ZERO;
            target.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            target.blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            target.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
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
    loadingDesc() = {};
    base.reset();
    shader = {};
    dynamicLoaded = false;
    dynamicBaseHash = 0;
    lastDynamicPipeline.id = SG_INVALID_ID;
    lastDynamicGeneration = 0;
    resetPipelines();

    loadingDesc().primitive_type = getPrimitiveType(primitiveType);

    bindSlotIndex = 0;
    bufferToBindSlot.clear();
    shaderInputs = 0;
    shaderInputSignature = 0;

    blendOverride = false;
    blendState = BlendState();

    std::fill(std::begin(batchUniformSlots), std::end(batchUniformSlots), (int8_t)-1);
    batchInstanceBindSlot = -1;
    batchLayout = BatchLayout::NONE;
    nextBatchVariant = nullptr;
    nextBatchObjectSlot = -1;
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
    loadingDesc().shader = shader->backend.get();
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
        loadingDesc().index_type = SG_INDEXTYPE_UINT16;
    }else if (dataType == AttributeDataType::UNSIGNED_INT){
        loadingDesc().index_type = SG_INDEXTYPE_UINT32;
    }else{
        loadingDesc().index_type = SG_INDEXTYPE_NONE;
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
        
        const BufferInfo info{vbuf.id, bufferOffset, stride, perInstance};
        if (bufferToBindSlot.count(info) == 0){
            bind.vertex_buffers[bindSlotIndex] = vbuf;
            bind.vertex_buffer_offsets[bindSlotIndex] = bufferOffset;
            bufferToBindSlot[info] = bindSlotIndex;

            loadingDesc().layout.buffers[bindSlotIndex].stride = stride;

            if (perInstance){
                loadingDesc().layout.buffers[bindSlotIndex].step_func = SG_VERTEXSTEP_PER_INSTANCE;
            }

            bindSlotIndex++;
        }

        size_t indexBuf = bufferToBindSlot[info];

        sg_pipeline_desc& desc = loadingDesc();
        desc.layout.attrs[slot].buffer_index = indexBuf;
        desc.layout.attrs[slot].offset = attrOffset;
        desc.layout.attrs[slot].format = getVertexFormat(elements, dataType, normalized);
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

void SokolObject::retargetVertexBuffer(uint32_t fromBufferId, sg_buffer toBuffer){
    if (fromBufferId == toBuffer.id)
        return;
    std::vector<std::pair<BufferInfo, size_t>> moved;
    for (auto it = bufferToBindSlot.begin(); it != bufferToBindSlot.end();){
        if (it->first.id == fromBufferId){
            moved.push_back(*it);
            it = bufferToBindSlot.erase(it);
        }else{
            ++it;
        }
    }
    for (std::pair<BufferInfo, size_t>& entry : moved){
        bind.vertex_buffers[entry.second] = toBuffer;
        bind.vertex_buffer_offsets[entry.second] = (int)entry.first.offset;
        entry.first.id = toBuffer.id;
        bufferToBindSlot[entry.first] = entry.second;
    }
}

void SokolObject::retargetIndexBuffer(uint32_t fromBufferId, sg_buffer toBuffer){
    if (loadIndexBuffer.id == fromBufferId)
        loadIndexBuffer = toBuffer;
    if (bind.index_buffer.id == fromBufferId)
        bind.index_buffer = toBuffer;
}

void SokolObject::getTextureBinding(std::pair<int, int> slot, uint32_t& view, uint32_t& sampler) const{
    view = (slot.first >= 0 && slot.first < SG_MAX_VIEW_BINDSLOTS) ? bind.views[slot.first].id : 0;
    sampler = (slot.second >= 0 && slot.second < SG_MAX_SAMPLER_BINDSLOTS) ? bind.samplers[slot.second].id : 0;
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
    case BlendOperation::MAX: return SG_BLENDOP_MAX;
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
    if (!base || target.id == SG_INVALID_ID || target.id == this->shader.id || sg_query_shader_state(target) != SG_RESOURCESTATE_VALID)
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
    sg_pipeline_desc desc = *base;

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
        // Mesh geometry pass with four color attachments (MRT):
        //   color[0] = packed depth (same as PIP_DEPTH, so SSAO/SSR depth is unchanged)
        //   color[1] = view-space normal (octahedral .rg, 8 bits) + roughness (.b) + metallic (.a)
        //   color[2] = linear base color (.rgb) + reflection source code (.a)
        //   color[3] = occlusion of the environment specular (.r) + the normal's next 8 bits (.gb)
        if (loadState.faceCulling){
            desc.cull_mode = getCullMode(loadState.cullingMode);
        }
        desc.face_winding = getFaceWinding(loadWinding);
        desc.sample_count = 1;
        desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        desc.depth.write_enabled = true;
        desc.color_count = 4;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.colors[1].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.colors[2].pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.colors[3].pixel_format = SG_PIXELFORMAT_RGBA8;
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
        if ((shaderInputs & (1u << slot)) && loadDesc && loadDesc->desc.layout.attrs[slot].format == SG_VERTEXFORMAT_INVALID)
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
    // the working description becomes the shared base the pipelines and variants are made from
    base = SokolPipelineCache::internBase(loadingDesc());
    shader = base->shader;
    loadDesc.reset();
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
    sg_pipeline_desc& desc = loadingDesc();
    desc.primitive_type = getPrimitiveType(primitiveType);
    switch (indexType){
    case IndexType::UINT16: desc.index_type = SG_INDEXTYPE_UINT16; break;
    case IndexType::UINT32: desc.index_type = SG_INDEXTYPE_UINT32; break;
    default: desc.index_type = SG_INDEXTYPE_NONE; break;
    }
    desc.label = "dynamic-pipeline";
    base = SokolPipelineCache::internBase(desc);
    shader = base->shader;
    loadDesc.reset();
    dynamicLoaded = shader.id != SG_INVALID_ID;
    dynamicBaseHash = SokolPipelineCache::baseHash(*base);
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
    if (source.base)
        loadingDesc() = *source.base;
    loadState = source.loadState;
    msaaSamples = source.msaaSamples;
    blendOverride = source.blendOverride;
    blendState = source.blendState;
    immediatePipelines = source.immediatePipelines;
    setShader(shader);
    checkVertexInputs();
    // no endLoad follows: the description is the base its variants are made from
    base = SokolPipelineCache::internBase(loadingDesc());
    this->shader = base->shader;
    loadDesc.reset();
    return this->shader.id != SG_INVALID_ID;
}

void SokolObject::shareBindings(const SokolObject& source){
    bind = source.bind;
}

bool SokolObject::beginAccumulationDraw(PipelineType colorType, bool mirrored){
    // a batcher's held draws first (SokolSystem::setDeferredFlush)
    SokolSystem::beforeForeignDraw();
    uniformsDropped = false; // a new draw: its uniforms come next
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    drawWaiting = false;
    if (!base || shader.id == SG_INVALID_ID || sg_query_shader_state(shader) != SG_RESOURCESTATE_VALID){
        drawWaiting = base && shader.id != SG_INVALID_ID && sg_query_shader_state(shader) == SG_RESOURCESTATE_ALLOC;
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
    SokolDrawBatch::flush();
    SokolSystem::applyPipeline(held->pipeline.id);
    return true;
}

bool SokolObject::beginDraw(const PipelineState& state){
    // a batcher's held draws first (SokolSystem::setDeferredFlush)
    SokolSystem::beforeForeignDraw();
    uniformsDropped = false; // a new draw: its uniforms come next
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    drawWaiting = false;
    if (!dynamicLoaded || sg_query_shader_state(shader) != SG_RESOURCESTATE_VALID){
        drawWaiting = dynamicLoaded && sg_query_shader_state(shader) == SG_RESOURCESTATE_ALLOC;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }

    sg_pipeline pipeline;
    const uint32_t generation = SokolPipelineCache::generation();
    if (lastDynamicPipeline.id != SG_INVALID_ID && lastDynamicGeneration == generation && lastDynamicState == state){
        pipeline = lastDynamicPipeline;
    }else{
        pipeline = SokolPipelineCache::get(*base, dynamicBaseHash, state, immediatePipelines);
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

    // (drawn directly: what a batch holds goes first)
    SokolDrawBatch::flush();
    lastPipelineIndex = -1;
    SokolPipelineCache::apply(pipeline);
    return true;
}

// The pipeline of a type for a draw: the type's own, or a shared variant (mirrored, no cull, LOD
// fade) made on the first draw that asks for it. WAIT while a variant the draw cannot do without
// is being made; FAIL for a LOD fade variant that cannot be made (the caller draws without fading)
namespace {
    bool overdrawDraws = false;
}

void SokolObject::setOverdraw(bool overdraw){
#ifdef JASPERENGINE_EDITOR
    overdrawDraws = overdraw;
#else
    (void)overdraw;
#endif
}

SokolObject::Selection SokolObject::selectPipeline(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader, sg_pipeline& selected){
    const int index = pipelineIndex(pipType);
    if (overdrawDraws && index < PIPELINE_TYPES){
        // every layer adds its heat (the colour the draw's material gives), nothing hides another
        const SokolVariantRefs::Variant* held = variants.find(VARIANT_OVERDRAW, (uint8_t)index);
        if (!held){
            sg_pipeline_desc desc = makePipelineDesc(pipType);
            desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
            desc.depth.write_enabled = false;
            desc.cull_mode = SG_CULLMODE_NONE;
            desc.stencil.enabled = false;
            for (int c = 0; c < std::max(desc.color_count, 1); c++){
                desc.colors[c].blend.enabled = true;
                desc.colors[c].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
                desc.colors[c].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
                desc.colors[c].blend.op_rgb = SG_BLENDOP_ADD;
                desc.colors[c].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
                desc.colors[c].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
                desc.colors[c].blend.op_alpha = SG_BLENDOP_ADD;
            }
            variants.add(VARIANT_OVERDRAW, (uint8_t)index, SokolPipelineCache::acquireVariant(desc));
            held = variants.find(VARIANT_OVERDRAW, (uint8_t)index);
        }
        if (held && held->pipeline.id != SG_INVALID_ID && sg_query_pipeline_state(held->pipeline) == SG_RESOURCESTATE_VALID){
            selected = held->pipeline;
            return Selection::READY;
        }
        return Selection::WAIT;
    }
    if (!disableFaceCulling && !mirrored && !lodFadeShader)
        return Selection::READY;
    if (index >= PIPELINE_TYPES)
        return lodFadeShader ? Selection::FAIL : Selection::READY;

    sg_shader fadeShader = {SG_INVALID_ID};
    if (lodFadeShader){
        fadeShader = lodFadeShader->backend.get();
        if (fadeShader.id == SG_INVALID_ID || sg_query_shader_state(fadeShader) != SG_RESOURCESTATE_VALID)
            return Selection::FAIL;
        // made on another fade shader (its variant was built again since): those go
        if (fadeShader.id != lodFadeShaderId){
            variants.removeFrom(VARIANT_LOD_FADE);
            lodFadeShaderId = fadeShader.id;
            if (warm){
                std::fill(std::begin(warm->lodFadePipeline), std::end(warm->lodFadePipeline), 0u);
                std::fill(std::begin(warm->lodFadeKey), std::end(warm->lodFadeKey), 0ull);
            }
        }
    }

    // 0 plain, 1 mirrored, 2 no cull, 3 both: the variant kinds, from VARIANT_LOD_FADE for a fade
    const int modifier = mirrored ? (disableFaceCulling ? 3 : 1) : (disableFaceCulling ? 2 : 0);
    const int kind = lodFadeShader ? VARIANT_LOD_FADE + modifier
                                   : (modifier == 1 ? VARIANT_MIRRORED : (modifier == 2 ? VARIANT_NO_CULL : VARIANT_MIRRORED_NO_CULL));
    const bool sameAsPipeline = (kind == VARIANT_NO_CULL) && (noCullSame & (1u << index));
    const SokolVariantRefs::Variant* held = variants.find((uint8_t)kind, (uint8_t)index);
    if (!held && !sameAsPipeline){
        sg_pipeline_desc desc = makePipelineDesc(pipType);
        const bool culls = desc.cull_mode == SG_CULLMODE_BACK || desc.cull_mode == SG_CULLMODE_FRONT;
        if (lodFadeShader){
            desc.shader = fadeShader;
        }
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
        selected = held->pipeline;
        return Selection::READY;
    }
    const bool making = held && held->pipeline.id != SG_INVALID_ID && sg_query_pipeline_state(held->pipeline) == SG_RESOURCESTATE_ALLOC;
    if (lodFadeShader){
        // a fading draw has no stand-in (the plain pipeline would draw both levels whole)
        return making ? Selection::WAIT : Selection::FAIL;
    }
    if (mirrored && making){
        // a mirrored draw waits for its variant (the plain pipeline would cull its front faces)
        return Selection::WAIT;
    }
    // a variant that failed to build (or a no-cull one still being made) leaves the pipeline as it is
    return Selection::READY;
}

bool SokolObject::beginDraw(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader){
    // a batcher's held draws first (SokolSystem::setDeferredFlush)
    SokolSystem::beforeForeignDraw();
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
    if (!base || shader.id == SG_INVALID_ID ||
        sg_query_shader_state(shader) != SG_RESOURCESTATE_VALID){
        return false;
    }

#ifndef JASPERENGINE_EDITOR
    disableFaceCulling = false;
#endif

    const Selection selection = selectPipeline(pipType, disableFaceCulling, mirrored, lodFadeShader, selectedPipeline);
    if (selection != Selection::READY){
        drawWaiting = selection == Selection::WAIT;
        waitingDraws += drawWaiting ? 1 : 0;
        return false;
    }

    {
        const int index = pipelineIndex(pipType);
        const sg_shader warmShader = lodFadeShader ? lodFadeShader->backend.get() : shader;
        if (index < PIPELINE_TYPES && !isWarm(selectedPipeline, index, lodFadeShader != nullptr, warmShader)){
            drawWaiting = true;
            waitingDraws++;
            return false;
        }
    }

    appliedPipeline = selectedPipeline.id;
    lastPipelineIndex = pipelineIndex(pipType);
    lastPipelinePlain = selectedPipeline.id == pipelineOf(pipType).id;
    if (SokolDrawBatch::isRecording()){
        // held until its draw: it may join a batch (SokolDrawBatch)
        SokolDrawBatch::beginHeld(this, selectedPipeline);
        return true;
    }
    if (SokolDrawBatch::isEnabled())
        SokolDrawBatch::applyPipeline(selectedPipeline);
    else
        SokolSystem::applyPipeline(selectedPipeline.id);
    return true;
}

bool SokolObject::isLodFadeReady(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader){
    if (!lodFadeShader)
        return false;
    sg_pipeline selected = pipelineOf(pipType);
    if (selected.id == SG_INVALID_ID || sg_query_pipeline_state(selected) != SG_RESOURCESTATE_VALID)
        return false;
#ifndef JASPERENGINE_EDITOR
    disableFaceCulling = false;
#endif
    if (selectPipeline(pipType, disableFaceCulling, mirrored, lodFadeShader, selected) != Selection::READY)
        return false;
    const int index = pipelineIndex(pipType);
    return index < PIPELINE_TYPES && isWarm(selected, index, true, lodFadeShader->backend.get());
}

uint32_t SokolObject::getAppliedPipelineId() const{
    return appliedPipeline;
}

void SokolObject::applyUniformBlock(int slot, unsigned int count, void* data){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    if (slot != -1){
        if (SokolDrawBatch::holdUniforms(this, slot, count, data))
            return;
        if (SokolDrawBatch::isEnabled()){
            SokolDrawBatch::flush();
            if (!SokolDrawBatch::applyUniforms(slot, data, count))
                uniformsDropped = true;
            return;
        }
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
    // held (SokolDrawBatch): drawn and counted when its batch is
    if (SokolDrawBatch::holdDraw(this, baseElement, vertexCount, instanceCount))
        return false;
    nextBatchVariant = nullptr;
    if (uniformsDropped){
        uniformsDropped = false;
        SokolSystem::countSkippedDraw();
        return false;
    }
    SokolDrawBatch::flush();
    // past the frame's descriptor buffer (Vulkan): skipped (SokolSystem counts and logs it),
    // where sokol would drop it and every draw after it in the frame
    return bindAndDraw(bind, (int)baseElement, (int)vertexCount, (int)instanceCount);
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
    loadDesc.reset();
    // dynamic pipelines stay in SokolPipelineCache until their shader goes
    base.reset();
    shader = {};
    dynamicLoaded = false;
    dynamicBaseHash = 0;
    lastDynamicPipeline.id = SG_INVALID_ID;
    lastDynamicGeneration = 0;
    bindSlotIndex = 0;
    blendOverride = false;
    blendState = BlendState();
}

// ---------------------------------------------------------------- automatic GPU instancing (S55)

namespace {
    // vectors an instance takes in each layout (includes/object_params.glsl, object_depth_params.glsl)
    int batchVectors(BatchLayout layout){
        switch (layout){
        case BatchLayout::MESH: return 10;
        case BatchLayout::DEPTH: return 8;
        default: return 0;
        }
    }

    // the object block both layouts read: model, normal or view-projection, mvp
    constexpr unsigned int OBJECT_BLOCK_BYTES = 3 * 16 * sizeof(float);

    // a draw from its beginDraw to its draw: what it applies, copied
    struct HeldUniform{
        int slot;
        unsigned int size;
        size_t offset;
    };

    struct HeldDraw{
        SokolObject* object = nullptr;
        sg_pipeline pipeline = {};
        int pipelineIndex = -1;
        bool plain = false;
        sg_primitive_type primitive = SG_PRIMITIVETYPE_TRIANGLES;
        sg_bindings bind = {};
        std::vector<HeldUniform> uniforms;
        std::vector<uint8_t> bytes;
        unsigned int base = 0;
        unsigned int count = 0;
        unsigned int instances = 0;
        SokolObject* variant = nullptr;
        uint32_t variantPipeline = SG_INVALID_ID;  // its pipeline of this draw's type
        int objectSlot = -1;
        float lodFade = 0.0f;

        void reset(SokolObject* drawing, sg_pipeline drawPipeline){
            object = drawing;
            pipeline = drawPipeline;
            uniforms.clear();
            bytes.clear();
            variant = nullptr;
            objectSlot = -1;
            lodFade = 0.0f;
        }

        // the bytes last applied to the object block (null when it was not)
        const uint8_t* objectBlock() const{
            const uint8_t* block = nullptr;
            for (const HeldUniform& uniform : uniforms){
                if (uniform.slot == objectSlot)
                    block = (uniform.size == OBJECT_BLOCK_BYTES) ? bytes.data() + uniform.offset : nullptr;
            }
            return block;
        }
    };

    struct BatchState{
        bool enabled = false;
        bool recording = false;
        // the draw being set up, and the held batch: its first draw, its instances (the vectors
        // of BatchLayout) and their object blocks (drawn one by one when the batch cannot be)
        HeldDraw current;
        bool currentOpen = false;
        HeldDraw head;
        bool headOpen = false;
        uint32_t heldInstances = 0;
        std::vector<float> instanceData;
        std::vector<uint8_t> objectBlocks;
        // the instance stream buffer: its size, what the frame appended and asked for
        sg_buffer stream = {};
        uint64_t streamContext = 0;
        size_t streamSize = 0;
        size_t streamUsed = 0;
        size_t streamNeeded = 0;
        // the bytes each uniform slot holds, valid in the pipeline epoch they were applied in
        struct Slot{
            uint32_t epoch = 0;
            std::vector<uint8_t> bytes;
        };
        Slot slots[SG_MAX_UNIFORMBLOCK_BINDSLOTS];
        SokolDrawBatch::FrameStats frame;
        SokolDrawBatch::FrameStats last;
    };

    BatchState& batchState(){
        static BatchState state;
        return state;
    }

    constexpr size_t STREAM_MIN_BYTES = 256 * 1024;
    constexpr size_t STREAM_MAX_BYTES = 64 * 1024 * 1024;

    uint64_t trianglesOf(sg_primitive_type primitive, unsigned int count){
        if (primitive == SG_PRIMITIVETYPE_TRIANGLES)
            return count / 3;
        if (primitive == SG_PRIMITIVETYPE_TRIANGLE_STRIP && count > 2)
            return count - 2;
        return 0;
    }

    // a held draw's instance (vectors appended to out) from its object block; false when the
    // block cannot be one (a model matrix that is not affine: its last row is read as 0 0 0 1)
    bool packInstance(BatchLayout layout, const uint8_t* block, float lodFade, std::vector<float>& out){
        float m[48];
        std::memcpy(m, block, sizeof(m));
        const float* model = m;
        if (model[3] != 0.0f || model[7] != 0.0f || model[11] != 0.0f || model[15] != 1.0f)
            return false;
        // the model's first three rows
        for (int row = 0; row < 3; row++){
            out.push_back(model[row]);
            out.push_back(model[4 + row]);
            out.push_back(model[8 + row]);
            out.push_back(model[12 + row]);
        }
        const float* mvp = m + 32;
        if (layout == BatchLayout::MESH){
            // the normal matrix's first three columns (what a direction reads), the fade after the first
            const float* normal = m + 16;
            for (int column = 0; column < 3; column++){
                out.push_back(normal[column * 4]);
                out.push_back(normal[column * 4 + 1]);
                out.push_back(normal[column * 4 + 2]);
                out.push_back(column == 0 ? lodFade : 0.0f);
            }
            out.insert(out.end(), mvp, mvp + 16);
        }else{
            out.insert(out.end(), mvp, mvp + 16);
            out.push_back(lodFade);
            out.push_back(0.0f);
            out.push_back(0.0f);
            out.push_back(0.0f);
        }
        return true;
    }

    // the stream buffer for this frame's appends (made, or made bigger, between frames)
    bool ensureStream(BatchState& state){
        const uint64_t context = SystemRender::getContextId();
        if (state.stream.id != SG_INVALID_ID && state.streamContext != context){
            // the device it was made on is gone (and its handle with it)
            state.stream.id = SG_INVALID_ID;
            state.streamSize = 0;
        }
        if (state.stream.id != SG_INVALID_ID && sg_query_buffer_state(state.stream) == SG_RESOURCESTATE_VALID)
            return true;
        state.streamSize = std::max(state.streamSize, STREAM_MIN_BYTES);
        sg_buffer_desc desc = {};
        desc.size = state.streamSize;
        desc.usage.vertex_buffer = true;
        desc.usage.stream_update = true;
        desc.label = "auto-instancing";
        {
            JE_GPU_ALLOC_SCOPE(Uniforms, "Automatic instancing");
            state.stream = sg_make_buffer(&desc);
        }
        state.streamContext = context;
        if (state.stream.id == SG_INVALID_ID || sg_query_buffer_state(state.stream) != SG_RESOURCESTATE_VALID){
            state.stream.id = SG_INVALID_ID;
            return false;
        }
        return true;
    }

    // bytes in the stream buffer: their offset, -1 when they do not fit (the buffer grows for the
    // next frame) or the frame's stream budget refused them
    int appendToStream(BatchState& state, const void* data, size_t bytes){
        // (sokol keeps each append 4-byte aligned)
        state.streamNeeded += (bytes + 3) & ~(size_t)3;
        if (!ensureStream(state) || state.streamUsed + bytes > state.streamSize)
            return -1;
        const int offset = SokolSystem::appendBuffer(state.stream, {data, bytes});
        if (offset < 0)
            return -1;
        state.streamUsed = (size_t)offset + bytes;
        return offset;
    }

    // the held batch's instances
    int appendInstances(BatchState& state){
        const size_t bytes = state.instanceData.size() * sizeof(float);
        const int offset = appendToStream(state, state.instanceData.data(), bytes);
        if (offset >= 0)
            state.frame.instanceBytes += bytes;
        return offset;
    }

    // the same blocks with the same bytes but the object block's; else the slots that differ
    bool sameUniforms(const HeldDraw& a, const HeldDraw& b, uint32_t& differing){
        if (a.uniforms.size() != b.uniforms.size()){
            differing |= 0x80000000u;
            return false;
        }
        for (size_t i = 0; i < a.uniforms.size(); i++){
            const HeldUniform& ua = a.uniforms[i];
            const HeldUniform& ub = b.uniforms[i];
            if (ua.slot != ub.slot || ua.size != ub.size){
                differing |= 0x80000000u;
                return false;
            }
            if (ua.slot != a.objectSlot && std::memcmp(a.bytes.data() + ua.offset, b.bytes.data() + ub.offset, ua.size) != 0){
                differing |= 1u << ua.slot;
                return false;
            }
        }
        return true;
    }

    // b can be an instance of a's batch; else the reason is counted
    bool batchesWith(const HeldDraw& a, const HeldDraw& b, SokolDrawBatch::FrameStats& stats){
        if (a.pipeline.id != b.pipeline.id || a.pipelineIndex != b.pipelineIndex || a.objectSlot != b.objectSlot ||
                a.variantPipeline != b.variantPipeline){
            stats.splitPipeline++;
            return false;
        }
        if (a.base != b.base || a.count != b.count){
            stats.splitElements++;
            return false;
        }
        if (std::memcmp(&a.bind, &b.bind, sizeof(sg_bindings)) != 0){
            stats.splitBindings++;
            stats.splitBindingParts |= (std::memcmp(a.bind.vertex_buffers, b.bind.vertex_buffers, sizeof(a.bind.vertex_buffers)) ? 1u : 0u) |
                (std::memcmp(a.bind.vertex_buffer_offsets, b.bind.vertex_buffer_offsets, sizeof(a.bind.vertex_buffer_offsets)) ? 2u : 0u) |
                (a.bind.index_buffer.id != b.bind.index_buffer.id || a.bind.index_buffer_offset != b.bind.index_buffer_offset ? 4u : 0u) |
                (std::memcmp(a.bind.views, b.bind.views, sizeof(a.bind.views)) ? 8u : 0u) |
                (std::memcmp(a.bind.samplers, b.bind.samplers, sizeof(a.bind.samplers)) ? 16u : 0u);
            return false;
        }
        if (!sameUniforms(a, b, stats.splitUniformSlots)){
            stats.splitUniforms++;
            return false;
        }
        return true;
    }

    // a held draw, drawn as it would have been
    void drawHeld(const HeldDraw& draw){
        SokolDrawBatch::applyPipeline(draw.pipeline);
        bool dropped = false;
        for (const HeldUniform& uniform : draw.uniforms){
            if (!SokolDrawBatch::applyUniforms(uniform.slot, draw.bytes.data() + uniform.offset, uniform.size))
                dropped = true;
        }
        if (dropped){
            SokolSystem::countSkippedDraw();
            return;
        }
        if (!bindAndDraw(draw.bind, (int)draw.base, (int)draw.count, (int)draw.instances))
            return;
        Engine::addDrawStats(draw.instances, trianglesOf(draw.primitive, draw.count) * (draw.instances > 0 ? draw.instances : 1),
                            (uint64_t)draw.count * (draw.instances > 0 ? draw.instances : 1));
    }
}

void SokolDrawBatch::setEnabled(bool enabled){
    BatchState& state = batchState();
    if (state.enabled == enabled)
        return;
    flush();
    state.enabled = enabled;
    state.recording = false;
    invalidate();
}

bool SokolDrawBatch::isEnabled(){
    return batchState().enabled;
}

void SokolDrawBatch::begin(){
    BatchState& state = batchState();
    if (state.enabled)
        state.recording = true;
}

void SokolDrawBatch::end(){
    BatchState& state = batchState();
    if (!state.recording)
        return;
    flush();
    state.recording = false;
    state.currentOpen = false;
}

bool SokolDrawBatch::isRecording(){
    return batchState().recording;
}

void SokolDrawBatch::invalidate(){
    SokolSystem::forgetAppliedState();
}

void SokolDrawBatch::applyPipeline(sg_pipeline pipeline){
    if (!SokolPipelineCache::apply(pipeline))
        batchState().frame.pipelinesKept++;
}

bool SokolDrawBatch::applyUniforms(int slot, const void* data, unsigned int size){
    BatchState& state = batchState();
    BatchState::Slot& held = state.slots[slot];
    const uint32_t epoch = SokolSystem::appliedEpoch();
    if (held.epoch == epoch && held.bytes.size() == size && std::memcmp(held.bytes.data(), data, size) == 0){
        state.frame.uniformBytesKept += size;
        return true;
    }
    // past the frame's uniform buffer (Vulkan, Metal, WebGPU): refused, the draw it is for skipped
    if (!SokolSystem::reserveUniformBytes(size))
        return false;
    sg_apply_uniforms(slot, {data, size});
    held.epoch = epoch;
    held.bytes.assign((const uint8_t*)data, (const uint8_t*)data + size);
    return true;
}

void SokolDrawBatch::beginHeld(SokolObject* object, sg_pipeline pipeline){
    BatchState& state = batchState();
    state.current.reset(object, pipeline);
    state.current.pipelineIndex = object->lastPipelineIndex;
    state.current.plain = object->lastPipelinePlain;
    state.current.primitive = object->base ? object->base->primitive_type : _SG_PRIMITIVETYPE_DEFAULT;
    state.currentOpen = true;
}

bool SokolDrawBatch::holdUniforms(SokolObject* object, int slot, unsigned int size, const void* data){
    BatchState& state = batchState();
    if (!state.recording)
        return false;
    if (!state.currentOpen || state.current.object != object){
        // applied outside a held draw: what is held is drawn first
        flush();
        return false;
    }
    HeldDraw& draw = state.current;
    draw.uniforms.push_back({slot, size, draw.bytes.size()});
    draw.bytes.insert(draw.bytes.end(), (const uint8_t*)data, (const uint8_t*)data + size);
    return true;
}

bool SokolDrawBatch::holdDraw(SokolObject* object, unsigned int base, unsigned int count, unsigned int instances){
    BatchState& state = batchState();
    if (!state.recording || !state.currentOpen || state.current.object != object)
        return false;
    state.currentOpen = false;
    HeldDraw& draw = state.current;
    draw.bind = object->bind;
    draw.base = base;
    draw.count = count;
    draw.instances = instances;
    draw.variant = object->nextBatchVariant;
    draw.objectSlot = object->nextBatchObjectSlot;
    draw.lodFade = object->nextBatchLodFade;
    object->nextBatchVariant = nullptr;
    draw.variantPipeline = (draw.variant && draw.pipelineIndex >= 0 && draw.pipelineIndex < SokolObject::PIPELINE_TYPES)
        ? draw.variant->pipelines[draw.pipelineIndex].id : (uint32_t)SG_INVALID_ID;

    // one instance of the plain pipeline whose variant was linked and whose object block can be
    // an instance's
    const uint8_t* block = nullptr;
    const bool batchable = draw.variant && draw.variant->batchLayout != BatchLayout::NONE && draw.plain &&
                           draw.instances <= 1 && draw.objectSlot >= 0 && draw.pipelineIndex >= 0 &&
                           (block = draw.objectBlock()) != nullptr;
    const size_t instanceFirst = state.instanceData.size();
    if (batchable)
        state.frame.offeredDraws++;
    if (batchable && state.headOpen && batchesWith(state.head, draw, state.frame)){
        if (packInstance(draw.variant->batchLayout, block, draw.lodFade, state.instanceData)){
            state.objectBlocks.insert(state.objectBlocks.end(), block, block + OBJECT_BLOCK_BYTES);
            state.heldInstances++;
            return true;
        }
        state.instanceData.resize(instanceFirst);
    }
    flush();
    if (batchable){
        state.instanceData.clear();
        if (packInstance(draw.variant->batchLayout, block, draw.lodFade, state.instanceData)){
            std::swap(state.head, state.current);
            state.headOpen = true;
            state.heldInstances = 1;
            state.objectBlocks.assign(block, block + OBJECT_BLOCK_BYTES);
            // (its draws are one instance each: drawn with N)
            state.head.instances = 1;
            return true;
        }
        state.instanceData.clear();
    }
    drawHeld(draw);
    return true;
}

void SokolDrawBatch::flush(){
    BatchState& state = batchState();
    if (!state.headOpen)
        return;
    state.headOpen = false;
    HeldDraw& head = state.head;
    const uint32_t count = state.heldInstances;
    if (count == 1){
        drawHeld(head);
        return;
    }

    SokolObject* variant = head.variant;
    sg_bindings bindings = head.bind;
    sg_pipeline pipeline = {};
    int offset = -1;
    if (ensureStream(state)){
        bindings.vertex_buffers[variant->batchInstanceBindSlot] = state.stream;
        bindings.vertex_buffer_offsets[variant->batchInstanceBindSlot] = 0;
        if (variant->batchPipeline(head.pipelineIndex, bindings, pipeline))
            offset = appendInstances(state);
    }
    if (offset < 0){
        // its variant is not ready, or its instances do not fit this frame: one by one
        const HeldUniform* objectUniform = nullptr;
        for (const HeldUniform& uniform : head.uniforms){
            if (uniform.slot == head.objectSlot)
                objectUniform = &uniform;
        }
        for (uint32_t i = 0; i < count; i++){
            if (objectUniform)
                std::memcpy(head.bytes.data() + objectUniform->offset, state.objectBlocks.data() + (size_t)i * OBJECT_BLOCK_BYTES, OBJECT_BLOCK_BYTES);
            drawHeld(head);
        }
        return;
    }
    bindings.vertex_buffer_offsets[variant->batchInstanceBindSlot] = offset;

    SokolDrawBatch::applyPipeline(pipeline);
    bool dropped = false;
    for (const HeldUniform& uniform : head.uniforms){
        if (uniform.slot == head.objectSlot)
            continue;
        const int slot = variant->batchUniformSlots[uniform.slot];
        if (slot < 0)
            continue;
        if (!SokolDrawBatch::applyUniforms(slot, head.bytes.data() + uniform.offset, uniform.size))
            dropped = true;
    }
    if (dropped){
        SokolSystem::countSkippedDraw();
        return;
    }
    if (!bindAndDraw(bindings, (int)head.base, (int)head.count, (int)count))
        return;
    Engine::addDrawStats(count, trianglesOf(head.primitive, head.count) * count, (uint64_t)head.count * count);
    state.frame.instancedDraws++;
    state.frame.batchedInstances += count;
}

void SokolDrawBatch::endFrame(){
    BatchState& state = batchState();
    state.last = state.frame;
    state.frame = FrameStats();
    // a frame that asked for more than the buffer holds: bigger from the next one
    if (state.streamNeeded > state.streamSize && state.streamSize < STREAM_MAX_BYTES){
        size_t size = std::max(state.streamSize, STREAM_MIN_BYTES);
        while (size < state.streamNeeded && size < STREAM_MAX_BYTES)
            size *= 2;
        if (state.stream.id != SG_INVALID_ID && state.streamContext == SystemRender::getContextId() && sg_isvalid()){
            sg_destroy_buffer(state.stream);
            SokolSystem::forgetAppliedBindings();
        }
        state.stream.id = SG_INVALID_ID;
        state.streamSize = std::min(size, STREAM_MAX_BYTES);
    }
    state.streamUsed = 0;
    state.streamNeeded = 0;
}

const SokolDrawBatch::FrameStats& SokolDrawBatch::getLastFrameStats(){
    return batchState().last;
}

int SokolDrawBatch::appendStream(const void* data, size_t bytes){
    if (!data || bytes == 0)
        return -1;
    return appendToStream(batchState(), data, bytes);
}

sg_buffer SokolDrawBatch::streamBuffer(){
    return batchState().stream;
}

void SokolDrawBatch::shutdown(){
    BatchState& state = batchState();
    if (state.stream.id != SG_INVALID_ID && state.streamContext == SystemRender::getContextId() && sg_isvalid()){
        sg_destroy_buffer(state.stream);
        SokolSystem::forgetAppliedBindings();
    }
    state.stream.id = SG_INVALID_ID;
    state.streamSize = 0;
    state.headOpen = false;
    state.currentOpen = false;
    state.recording = false;
    for (BatchState::Slot& slot : state.slots)
        slot = BatchState::Slot();
}

void SokolObject::addBatchInstanceAttributes(const int* locations, int count, BatchLayout layout){
    const int vectors = batchVectors(layout);
    batchLayout = (count == vectors) ? layout : BatchLayout::NONE;
    if (batchLayout == BatchLayout::NONE)
        return;
    // one bind slot whatever the backend (an interleaved buffer), after every other buffer: the
    // frame's instance stream, bound by the batch that draws (SokolDrawBatch::flush; a load may
    // run on a thread that does not make buffers)
    batchInstanceBindSlot = (int)bindSlotIndex;
    const unsigned int stride = (unsigned int)(vectors * 4 * sizeof(float));
    sg_pipeline_desc& desc = loadingDesc();
    for (int v = 0; v < count; v++){
        const int slot = locations[v];
        // a vector the variant does not read (the compiler left it out: a depth program reads
        // no model rows) keeps its place in the stride
        if (slot < 0)
            continue;
        if (slot >= SG_MAX_VERTEX_ATTRIBUTES){
            batchLayout = BatchLayout::NONE;
            return;
        }
        desc.layout.attrs[slot].buffer_index = batchInstanceBindSlot;
        desc.layout.attrs[slot].offset = (int)(v * 4 * sizeof(float));
        desc.layout.attrs[slot].format = SG_VERTEXFORMAT_FLOAT4;
    }
    bind.vertex_buffers[batchInstanceBindSlot].id = SG_INVALID_ID;
    bind.vertex_buffer_offsets[batchInstanceBindSlot] = 0;
    desc.layout.buffers[batchInstanceBindSlot].stride = (int)stride;
    desc.layout.buffers[batchInstanceBindSlot].step_func = SG_VERTEXSTEP_PER_INSTANCE;
    bindSlotIndex++;
}

namespace {
    const ShaderUniformBlock* findBlock(const ShaderData& data, const std::string& name){
        for (const ShaderStage& stage : data.stages){
            for (const ShaderUniformBlock& block : stage.uniformblocks){
                if (block.name == name)
                    return &block;
            }
        }
        return nullptr;
    }

    // the same textures, samplers and storage buffers at the same slots
    bool sameResources(const ShaderData& a, const ShaderData& b){
        if (a.stages.size() != b.stages.size())
            return false;
        for (size_t s = 0; s < a.stages.size(); s++){
            const ShaderStage& sa = a.stages[s];
            const ShaderStage& sb = b.stages[s];
            if (sa.textures.size() != sb.textures.size() || sa.samplers.size() != sb.samplers.size() ||
                    sa.storagebuffers.size() != sb.storagebuffers.size())
                return false;
            for (size_t i = 0; i < sa.textures.size(); i++){
                if (sa.textures[i].name != sb.textures[i].name || sa.textures[i].slot != sb.textures[i].slot)
                    return false;
            }
            for (size_t i = 0; i < sa.samplers.size(); i++){
                if (sa.samplers[i].name != sb.samplers[i].name || sa.samplers[i].slot != sb.samplers[i].slot)
                    return false;
            }
            for (size_t i = 0; i < sa.storagebuffers.size(); i++){
                if (sa.storagebuffers[i].name != sb.storagebuffers[i].name || sa.storagebuffers[i].slot != sb.storagebuffers[i].slot)
                    return false;
            }
        }
        return true;
    }
}

bool SokolObject::linkBatchVariant(const SokolObject& source, const ShaderData& sourceShader, const ShaderData& variantShader,
                                   const std::string& objectBlock){
    std::fill(std::begin(batchUniformSlots), std::end(batchUniformSlots), (int8_t)-1);
    batchLinkFailure = nullptr;
    auto fail = [this](const char* reason){
        batchLayout = BatchLayout::NONE;
        batchLinkFailure = reason;
        return false;
    };
    if (batchLayout == BatchLayout::NONE || batchInstanceBindSlot < 0)
        return fail("no instance attributes");
    if (!base || !source.base)
        return fail("not loaded");
    // the object block goes per instance (a shader without the switch keeps it: no batches)
    if (!findBlock(sourceShader, objectBlock))
        return fail("the source has no object block");
    if (findBlock(variantShader, objectBlock))
        return fail("the variant keeps the object block");
    // every other block of the source at a slot of the variant, of the same size, and no other
    size_t variantBlocks = 0;
    for (const ShaderStage& stage : variantShader.stages)
        variantBlocks += stage.uniformblocks.size();
    size_t matched = 0;
    for (const ShaderStage& stage : sourceShader.stages){
        for (const ShaderUniformBlock& block : stage.uniformblocks){
            if (block.slot >= SG_MAX_UNIFORMBLOCK_BINDSLOTS)
                return fail("a uniform slot past sokol's");
            if (block.name == objectBlock)
                continue;
            const ShaderUniformBlock* own = findBlock(variantShader, block.name);
            if (!own || own->sizeBytes != block.sizeBytes || own->slot >= SG_MAX_UNIFORMBLOCK_BINDSLOTS)
                return fail("a uniform block differs");
            batchUniformSlots[block.slot] = (int8_t)own->slot;
            matched++;
        }
    }
    if (matched != variantBlocks)
        return fail("the variant has another uniform block");
    if (!sameResources(sourceShader, variantShader))
        return fail("other textures, samplers or storage buffers");
    // the source's vertex buffers, at the same slots, then the instance stream
    if ((size_t)batchInstanceBindSlot != source.bindSlotIndex)
        return fail("another number of vertex buffers");
    for (size_t slot = 0; slot < source.bindSlotIndex; slot++){
        if (bind.vertex_buffers[slot].id != source.bind.vertex_buffers[slot].id ||
                bind.vertex_buffer_offsets[slot] != source.bind.vertex_buffer_offsets[slot] ||
                base->layout.buffers[slot].stride != source.base->layout.buffers[slot].stride ||
                base->layout.buffers[slot].step_func != source.base->layout.buffers[slot].step_func)
            return fail("other vertex buffers");
    }
    if (base->index_type != source.base->index_type || base->primitive_type != source.base->primitive_type)
        return fail("another index or primitive type");
    return true;
}

const char* SokolObject::getBatchLinkFailure() const{
    return batchLinkFailure;
}

bool SokolObject::isBatchVariant() const{
    return batchLayout != BatchLayout::NONE;
}

void SokolObject::setNextDrawBatch(SokolObject* variant, int objectSlot, float lodFade){
    nextBatchVariant = (variant && variant->isBatchVariant()) ? variant : nullptr;
    nextBatchObjectSlot = objectSlot;
    nextBatchLodFade = lodFade;
}

bool SokolObject::batchPipeline(int index, const sg_bindings& bindings, sg_pipeline& pipeline){
    if (index < 0 || index >= PIPELINE_TYPES)
        return false;
    pipeline = pipelines[index];
    if (pipeline.id == SG_INVALID_ID || sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID)
        return false;
    if (!base || shader.id == SG_INVALID_ID || sg_query_shader_state(shader) != SG_RESOURCESTATE_VALID)
        return false;
    // (the GL warm draw with the batch's bindings: its textures are the source's)
    const sg_bindings own = bind;
    bind = bindings;
    const bool warm = isWarm(pipeline, index, false, shader);
    bind = own;
    return warm;
}

bool SokolObject::bindStreamVertexBuffer(uint32_t fromBufferId, const void* data, size_t bytes){
    const int offset = SokolDrawBatch::appendStream(data, bytes);
    if (offset < 0)
        return false;
    replaceVertexBuffer(fromBufferId, SokolDrawBatch::streamBuffer(), (size_t)offset);
    return true;
}

uint32_t SokolObject::getVertexBufferId() const{
    return bind.vertex_buffers[0].id;
}

uint32_t SokolObject::getIndexBufferId() const{
    return bind.index_buffer.id;
}
