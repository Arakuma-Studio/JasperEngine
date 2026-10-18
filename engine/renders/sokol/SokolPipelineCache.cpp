// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolPipelineCache.h"

#include "Engine.h"
#include "Log.h"
#include "SokolCmdQueue.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace jasperengine;

namespace {

    struct Entry {
        sg_pipeline pipeline;
        sg_shader shader;
        uint64_t baseHash;
        PipelineState state;
    };

    // main thread in practice (UI drawing); the mutex covers a shader destroyed by a loader
    // thread while the main thread draws
    std::mutex& cacheMutex(){
        static std::mutex mutex;
        return mutex;
    }

    std::unordered_map<uint64_t, Entry>& entries(){
        static std::unordered_map<uint64_t, Entry> map;
        return map;
    }

    uint64_t missCount = 0;
    uint64_t applyCount = 0;
    uint32_t lastApplied = SG_INVALID_ID;
    std::atomic<uint32_t> pipelineGeneration{1};

    uint64_t fnv(uint64_t h, const void* data, size_t size){
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++){
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // what the base contributes: shader, vertex layout, index and primitive type (all of
    // sg_vertex_layout_state is 4-byte fields, so its bytes carry no padding)
    uint64_t baseHashOf(const sg_pipeline_desc& base){
        uint64_t h = 14695981039346656037ull;
        h = fnv(h, &base.shader.id, sizeof(base.shader.id));
        h = fnv(h, &base.layout, sizeof(base.layout));
        const int32_t index = (int32_t)base.index_type;
        const int32_t primitive = (int32_t)base.primitive_type;
        h = fnv(h, &index, sizeof(index));
        h = fnv(h, &primitive, sizeof(primitive));
        return h;
    }

    sg_compare_func toCompare(CompareFunc func){
        switch (func){
        case CompareFunc::NEVER: return SG_COMPAREFUNC_NEVER;
        case CompareFunc::LESS: return SG_COMPAREFUNC_LESS;
        case CompareFunc::EQUAL: return SG_COMPAREFUNC_EQUAL;
        case CompareFunc::LESS_EQUAL: return SG_COMPAREFUNC_LESS_EQUAL;
        case CompareFunc::GREATER: return SG_COMPAREFUNC_GREATER;
        case CompareFunc::NOT_EQUAL: return SG_COMPAREFUNC_NOT_EQUAL;
        case CompareFunc::GREATER_EQUAL: return SG_COMPAREFUNC_GREATER_EQUAL;
        case CompareFunc::ALWAYS: return SG_COMPAREFUNC_ALWAYS;
        }
        return SG_COMPAREFUNC_ALWAYS;
    }

    sg_stencil_op toStencilOp(StencilOp op){
        switch (op){
        case StencilOp::KEEP: return SG_STENCILOP_KEEP;
        case StencilOp::ZERO: return SG_STENCILOP_ZERO;
        case StencilOp::REPLACE: return SG_STENCILOP_REPLACE;
        case StencilOp::INCR_CLAMP: return SG_STENCILOP_INCR_CLAMP;
        case StencilOp::DECR_CLAMP: return SG_STENCILOP_DECR_CLAMP;
        case StencilOp::INVERT: return SG_STENCILOP_INVERT;
        case StencilOp::INCR_WRAP: return SG_STENCILOP_INCR_WRAP;
        case StencilOp::DECR_WRAP: return SG_STENCILOP_DECR_WRAP;
        }
        return SG_STENCILOP_KEEP;
    }

    sg_color_mask toColorMask(ColorWriteMask mask){
        const uint8_t bits = (uint8_t)mask;
        // sokol's 0 is "default" (RGBA): no channel is its own value
        if (bits == 0)
            return SG_COLORMASK_NONE;
        int result = 0;
        if (bits & (uint8_t)ColorWriteMask::R) result |= SG_COLORMASK_R;
        if (bits & (uint8_t)ColorWriteMask::G) result |= SG_COLORMASK_G;
        if (bits & (uint8_t)ColorWriteMask::B) result |= SG_COLORMASK_B;
        if (bits & (uint8_t)ColorWriteMask::A) result |= SG_COLORMASK_A;
        return (sg_color_mask)result;
    }

    void setBlend(sg_blend_state& blend, BlendMode mode){
        blend.op_rgb = SG_BLENDOP_ADD;
        blend.op_alpha = SG_BLENDOP_ADD;
        switch (mode){
        case BlendMode::NONE:
            blend.enabled = false;
            return;
        case BlendMode::ALPHA:
            blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::PREMULTIPLIED:
            blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        // the fixed pipeline types' straight blends (RenderState::blend), alpha as ALPHA
        case BlendMode::ADDITIVE:
            blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::MULTIPLY:
            blend.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ZERO;
            blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::ADDITIVE_PREMULTIPLIED:
            blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
            break;
        case BlendMode::SCREEN_PREMULTIPLIED:
            blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            break;
        case BlendMode::MULTIPLY_PREMULTIPLIED:
            blend.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            blend.src_factor_alpha = SG_BLENDFACTOR_DST_COLOR;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        }
        blend.enabled = true;
    }

    void destroyPipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID || !sg_isvalid())
            return;
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_pipeline(pipeline);
        }else{
            sg_destroy_pipeline(pipeline);
        }
    }

}

sg_pipeline_desc SokolPipelineCache::makeDesc(const sg_pipeline_desc& base, const PipelineState& state){
    // only what the key covers comes from the base
    sg_pipeline_desc desc = {};
    desc.shader = base.shader;
    desc.layout = base.layout;
    desc.primitive_type = base.primitive_type;
    desc.index_type = base.index_type;
    desc.label = base.label;

    desc.color_count = 1;
    if (!state.target.swapchain){
        switch (state.target.color){
        case ColorFormat::RED: desc.colors[0].pixel_format = SG_PIXELFORMAT_R8; break;
        case ColorFormat::SRGB8A8: desc.colors[0].pixel_format = SG_PIXELFORMAT_SRGB8A8; break;
        case ColorFormat::RGBA16F: desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F; break;
        case ColorFormat::RGBA32F: desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA32F; break;
        case ColorFormat::R32F: desc.colors[0].pixel_format = SG_PIXELFORMAT_R32F; break;
        default: desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8; break;
        }
        switch (state.target.depth){
        case DepthStencilFormat::NONE: desc.depth.pixel_format = SG_PIXELFORMAT_NONE; break;
        case DepthStencilFormat::DEPTH: desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH; break;
        case DepthStencilFormat::DEPTH_STENCIL: desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL; break;
        }
        desc.sample_count = state.target.sampleCount > 0 ? state.target.sampleCount : 1;
    }

    desc.colors[0].write_mask = toColorMask(state.writeMask);
    setBlend(desc.colors[0].blend, state.blend);

    desc.depth.compare = toCompare(state.depthCompare);
    desc.depth.write_enabled = state.depthWrite;

    if (state.stencil.enabled){
        desc.stencil.enabled = true;
        sg_stencil_face_state face = {};
        face.compare = toCompare(state.stencil.compare);
        face.fail_op = toStencilOp(state.stencil.failOp);
        face.depth_fail_op = toStencilOp(state.stencil.depthFailOp);
        face.pass_op = toStencilOp(state.stencil.passOp);
        desc.stencil.front = face;
        desc.stencil.back = face;
        desc.stencil.read_mask = state.stencil.readMask;
        desc.stencil.write_mask = state.stencil.writeMask;
        desc.stencil.ref = state.stencil.ref;
    }

    desc.cull_mode = state.cullBack ? SG_CULLMODE_BACK : SG_CULLMODE_NONE;

    return desc;
}

sg_pipeline SokolPipelineCache::get(const sg_pipeline_desc& base, const PipelineState& state){
    return get(base, baseHashOf(base), state);
}

uint64_t SokolPipelineCache::baseHash(const sg_pipeline_desc& base){
    return baseHashOf(base);
}

uint32_t SokolPipelineCache::generation(){
    return pipelineGeneration.load();
}

sg_pipeline SokolPipelineCache::get(const sg_pipeline_desc& base, uint64_t baseHash, const PipelineState& state){
    uint64_t key = baseHash ^ state.hash();

    std::lock_guard<std::mutex> lock(cacheMutex());
    auto& map = entries();
    // a different (shader, layout, state) on the same key is a hash collision: the next key
    for (;;){
        auto it = map.find(key);
        if (it == map.end())
            break;
        const Entry& entry = it->second;
        if (entry.shader.id == base.shader.id && entry.baseHash == baseHash && entry.state == state)
            return entry.pipeline;
        key++;
    }

    if (Engine::isAsyncThread()){
        Log::error("Dynamic pipelines are created on the main thread only");
        sg_pipeline invalid = {SG_INVALID_ID};
        return invalid;
    }

    sg_pipeline_desc desc = makeDesc(base, state);
    sg_pipeline pipeline = sg_make_pipeline(desc);
    missCount++;
    if (pipeline.id == SG_INVALID_ID)
        return pipeline;

    map[key] = Entry{pipeline, base.shader, baseHash, state};
    return pipeline;
}

void SokolPipelineCache::releaseShader(sg_shader shader){
    if (shader.id == SG_INVALID_ID)
        return;

    std::lock_guard<std::mutex> lock(cacheMutex());
    pipelineGeneration++;
    auto& map = entries();
    for (auto it = map.begin(); it != map.end();){
        if (it->second.shader.id == shader.id){
            if (it->second.pipeline.id == lastApplied)
                lastApplied = SG_INVALID_ID;
            destroyPipeline(it->second.pipeline);
            it = map.erase(it);
        }else{
            ++it;
        }
    }
}

void SokolPipelineCache::clear(){
    std::lock_guard<std::mutex> lock(cacheMutex());
    pipelineGeneration++;
    auto& map = entries();
    for (auto& pair : map){
        destroyPipeline(pair.second.pipeline);
    }
    map.clear();
    lastApplied = SG_INVALID_ID;
}

size_t SokolPipelineCache::size(){
    std::lock_guard<std::mutex> lock(cacheMutex());
    return entries().size();
}

bool SokolPipelineCache::apply(sg_pipeline pipeline){
    if (pipeline.id == lastApplied)
        return false;
    sg_apply_pipeline(pipeline);
    lastApplied = pipeline.id;
    applyCount++;
    return true;
}

void SokolPipelineCache::invalidateApplied(){
    lastApplied = SG_INVALID_ID;
}

uint64_t SokolPipelineCache::getMissCount(){
    return missCount;
}

uint64_t SokolPipelineCache::getApplyCount(){
    return applyCount;
}
