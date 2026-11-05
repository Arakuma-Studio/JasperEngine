// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolPipelineCache.h"

#include "Engine.h"
#include "Log.h"
#include "SokolAsyncCreate.h"
#include "SokolCmdQueue.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace jasperengine;

namespace {

    struct PipelineCacheEntry {
        sg_pipeline pipeline;
        sg_shader shader;
        uint64_t baseHash;
        PipelineState state;
    };

    // main thread in practice (UI drawing); the mutex covers a shader destroyed by a loader
    // thread while the main thread draws
    // never destroyed: an object holding a shared variant may let go of it in a destructor that
    // runs at exit (SokolObject)
    std::mutex& cacheMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::unordered_map<uint64_t, PipelineCacheEntry>& entries(){
        static std::unordered_map<uint64_t, PipelineCacheEntry> map;
        return map;
    }

    // an interned pipeline: the description it was made from (its label owned here) and the
    // references SokolObjects (and the warm-up) hold
    struct InternEntry {
        sg_pipeline pipeline;
        sg_pipeline_desc desc;
        std::string label;
        int refs;
    };

    std::unordered_map<uint64_t, InternEntry>& interned(){
        static std::unordered_map<uint64_t, InternEntry> map;
        return map;
    }

    // the interned pipelines held as variants (acquireVariant): pipeline id -> references
    // (never destroyed, as cacheMutex: a variant may be let go of at exit)
    std::unordered_map<uint32_t, int>& variantRefs(){
        static auto* map = new std::unordered_map<uint32_t, int>();
        return *map;
    }

    // pipeline id -> its key in interned()
    std::unordered_map<uint32_t, uint64_t>& internedKeys(){
        static std::unordered_map<uint32_t, uint64_t> map;
        return map;
    }

    // interned pipelines whose last reference went (key, pipeline id, the collect() round):
    // destroyed by the next round unless acquired again meanwhile, so an object loaded again in
    // the same frame (a variant swap, a reload) keeps its pipeline instead of creating it anew
    struct Retiring {
        uint64_t key;
        uint32_t pipeline;
        uint64_t round;
    };
    std::vector<Retiring>& retiring(){
        static std::vector<Retiring> list;
        return list;
    }
    uint64_t collectRound = 0;

    std::function<void(const sg_pipeline_desc&)>& recorder(){
        static std::function<void(const sg_pipeline_desc&)> function;
        return function;
    }

    // the pipelines reported since the recorder was set: each once, a hit as well as a miss (a
    // pipeline made before the recording started is still one the recorded run uses)
    std::unordered_set<uint32_t>& recorded(){
        static std::unordered_set<uint32_t> ids;
        return ids;
    }

    void record(sg_pipeline pipeline, const sg_pipeline_desc& desc){
        if (recorder() && pipeline.id != SG_INVALID_ID && recorded().insert(pipeline.id).second)
            recorder()(desc);
    }

    uint64_t missCount = 0;
    uint64_t applyCount = 0;
    uint32_t lastApplied = SG_INVALID_ID;
    uint32_t lastAppliedEpoch = 1;
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

    sg_stencil_op cacheToStencilOp(StencilOp op){
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
        case BlendMode::SUBTRACT_PREMULTIPLIED:
            blend.op_rgb = SG_BLENDOP_REVERSE_SUBTRACT;
            blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
            blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
            blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
            break;
        }
        blend.enabled = true;
    }

    void destroyCachedPipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID || !sg_isvalid())
            return;
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_pipeline(pipeline);
        }else{
            SokolAsyncCreate::destroyPipeline(pipeline);
        }
    }

    sg_pipeline createPipeline(const sg_pipeline_desc& desc, bool immediate){
        // off the render thread where creation compiles (spec S32): pending until then
        if (Engine::isAsyncThread())
            return SokolCmdQueue::add_command_make_pipeline(desc);
        return immediate ? SokolAsyncCreate::makePipelineNow(desc) : SokolAsyncCreate::makePipeline(desc);
    }

    // an immediate caller's pipeline that is still being created (the warm-up made it, or a
    // content object first) is waited for
    void finishNow(sg_pipeline pipeline, bool immediate){
        if (immediate && !Engine::isAsyncThread() && SokolAsyncCreate::isPending(pipeline))
            SokolAsyncCreate::waitFor(pipeline);
    }

    template<typename T>
    uint64_t mix(uint64_t h, T value){
        return fnv(h, &value, sizeof(value));
    }

    uint64_t mixFloat(uint64_t h, float value){
        // -0 and 0 are the same state
        return mix(h, value == 0.0f ? 0.0f : value);
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
        case ColorFormat::RG16F: desc.colors[0].pixel_format = SG_PIXELFORMAT_RG16F; break;
        case ColorFormat::R16F: desc.colors[0].pixel_format = SG_PIXELFORMAT_R16F; break;
        default: desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8; break;
        }
        switch (state.target.depth){
        case DepthStencilFormat::NONE: desc.depth.pixel_format = SG_PIXELFORMAT_NONE; break;
        case DepthStencilFormat::DEPTH: desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH; break;
        case DepthStencilFormat::DEPTH_STENCIL: desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL; break;
        }
        desc.sample_count = state.target.sampleCount > 0 ? state.target.sampleCount : 1;
    }

    if (state.target.colorless && !state.target.swapchain){
        desc.color_count = 0;
        desc.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
    }else{
        desc.colors[0].write_mask = toColorMask(state.writeMask);
        setBlend(desc.colors[0].blend, state.blend);
    }

    desc.depth.compare = toCompare(state.depthCompare);
    desc.depth.write_enabled = state.depthWrite;

    if (state.stencil.enabled){
        desc.stencil.enabled = true;
        sg_stencil_face_state face = {};
        face.compare = toCompare(state.stencil.compare);
        face.fail_op = cacheToStencilOp(state.stencil.failOp);
        face.depth_fail_op = cacheToStencilOp(state.stencil.depthFailOp);
        face.pass_op = cacheToStencilOp(state.stencil.passOp);
        desc.stencil.front = face;
        desc.stencil.back = face;
        desc.stencil.read_mask = state.stencil.readMask;
        desc.stencil.write_mask = state.stencil.writeMask;
        desc.stencil.ref = state.stencil.ref;
    }

    desc.cull_mode = state.cullBack ? SG_CULLMODE_BACK : SG_CULLMODE_NONE;

    return desc;
}

uint64_t SokolPipelineCache::descHash(const sg_pipeline_desc& d){
    uint64_t h = 14695981039346656037ull;
    h = mix(h, d.compute);
    h = mix(h, d.shader.id);
    for (const sg_vertex_buffer_layout_state& b : d.layout.buffers){
        h = mix(h, b.stride); h = mix(h, (int)b.step_func); h = mix(h, b.step_rate);
    }
    for (const sg_vertex_attr_state& a : d.layout.attrs){
        h = mix(h, a.buffer_index); h = mix(h, a.offset); h = mix(h, (int)a.format);
    }
    h = mix(h, (int)d.depth.pixel_format); h = mix(h, (int)d.depth.compare); h = mix(h, d.depth.write_enabled);
    h = mixFloat(h, d.depth.bias); h = mixFloat(h, d.depth.bias_slope_scale); h = mixFloat(h, d.depth.bias_clamp);
    h = mix(h, d.stencil.enabled);
    for (const sg_stencil_face_state* face : {&d.stencil.front, &d.stencil.back}){
        h = mix(h, (int)face->compare); h = mix(h, (int)face->fail_op); h = mix(h, (int)face->depth_fail_op); h = mix(h, (int)face->pass_op);
    }
    h = mix(h, d.stencil.read_mask); h = mix(h, d.stencil.write_mask); h = mix(h, d.stencil.ref);
    h = mix(h, d.color_count);
    for (const sg_color_target_state& c : d.colors){
        h = mix(h, (int)c.pixel_format); h = mix(h, (int)c.write_mask); h = mix(h, c.blend.enabled);
        h = mix(h, (int)c.blend.src_factor_rgb); h = mix(h, (int)c.blend.dst_factor_rgb); h = mix(h, (int)c.blend.op_rgb);
        h = mix(h, (int)c.blend.src_factor_alpha); h = mix(h, (int)c.blend.dst_factor_alpha); h = mix(h, (int)c.blend.op_alpha);
    }
    h = mix(h, (int)d.primitive_type); h = mix(h, (int)d.index_type); h = mix(h, (int)d.cull_mode);
    h = mix(h, (int)d.face_winding); h = mix(h, d.sample_count);
    h = mixFloat(h, d.blend_color.r); h = mixFloat(h, d.blend_color.g); h = mixFloat(h, d.blend_color.b); h = mixFloat(h, d.blend_color.a);
    h = mix(h, d.alpha_to_coverage_enabled);
    return h;
}

bool SokolPipelineCache::descEqual(const sg_pipeline_desc& a, const sg_pipeline_desc& b){
    if (a.compute != b.compute || a.shader.id != b.shader.id)
        return false;
    for (int i = 0; i < SG_MAX_VERTEXBUFFER_BINDSLOTS; i++){
        const sg_vertex_buffer_layout_state& x = a.layout.buffers[i];
        const sg_vertex_buffer_layout_state& y = b.layout.buffers[i];
        if (x.stride != y.stride || x.step_func != y.step_func || x.step_rate != y.step_rate)
            return false;
    }
    for (int i = 0; i < SG_MAX_VERTEX_ATTRIBUTES; i++){
        const sg_vertex_attr_state& x = a.layout.attrs[i];
        const sg_vertex_attr_state& y = b.layout.attrs[i];
        if (x.buffer_index != y.buffer_index || x.offset != y.offset || x.format != y.format)
            return false;
    }
    if (a.depth.pixel_format != b.depth.pixel_format || a.depth.compare != b.depth.compare ||
            a.depth.write_enabled != b.depth.write_enabled || a.depth.bias != b.depth.bias ||
            a.depth.bias_slope_scale != b.depth.bias_slope_scale || a.depth.bias_clamp != b.depth.bias_clamp)
        return false;
    auto sameFace = [](const sg_stencil_face_state& x, const sg_stencil_face_state& y){
        return x.compare == y.compare && x.fail_op == y.fail_op && x.depth_fail_op == y.depth_fail_op && x.pass_op == y.pass_op;
    };
    if (a.stencil.enabled != b.stencil.enabled || !sameFace(a.stencil.front, b.stencil.front) ||
            !sameFace(a.stencil.back, b.stencil.back) || a.stencil.read_mask != b.stencil.read_mask ||
            a.stencil.write_mask != b.stencil.write_mask || a.stencil.ref != b.stencil.ref)
        return false;
    if (a.color_count != b.color_count)
        return false;
    for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++){
        const sg_color_target_state& x = a.colors[i];
        const sg_color_target_state& y = b.colors[i];
        if (x.pixel_format != y.pixel_format || x.write_mask != y.write_mask || x.blend.enabled != y.blend.enabled ||
                x.blend.src_factor_rgb != y.blend.src_factor_rgb || x.blend.dst_factor_rgb != y.blend.dst_factor_rgb ||
                x.blend.op_rgb != y.blend.op_rgb || x.blend.src_factor_alpha != y.blend.src_factor_alpha ||
                x.blend.dst_factor_alpha != y.blend.dst_factor_alpha || x.blend.op_alpha != y.blend.op_alpha)
            return false;
    }
    return a.primitive_type == b.primitive_type && a.index_type == b.index_type && a.cull_mode == b.cull_mode &&
           a.face_winding == b.face_winding && a.sample_count == b.sample_count &&
           a.blend_color.r == b.blend_color.r && a.blend_color.g == b.blend_color.g &&
           a.blend_color.b == b.blend_color.b && a.blend_color.a == b.blend_color.a &&
           a.alpha_to_coverage_enabled == b.alpha_to_coverage_enabled;
}

sg_pipeline SokolPipelineCache::acquire(const sg_pipeline_desc& desc, bool immediate){
    const uint64_t hash = descHash(desc);
    uint64_t key = hash;
    std::lock_guard<std::mutex> lock(cacheMutex());
    auto& map = interned();
    // another description on the same key is a hash collision: the next key
    for (;;){
        auto it = map.find(key);
        if (it == map.end())
            break;
        InternEntry& entry = it->second;
        if (descEqual(entry.desc, desc) && sg_query_pipeline_state(entry.pipeline) != SG_RESOURCESTATE_INVALID){
            // (one retiring comes back: collect() leaves it)
            entry.refs++;
            if (recorder())
                record(entry.pipeline, entry.desc);
            finishNow(entry.pipeline, immediate);
            return entry.pipeline;
        }
        key++;
    }
    InternEntry entry;
    entry.desc = desc;
    entry.label = desc.label ? desc.label : "";
    entry.desc.label = nullptr;
    entry.refs = 1;
    sg_pipeline_desc create = desc;
    create.label = entry.label.empty() ? nullptr : entry.label.c_str();
    entry.pipeline = createPipeline(create, immediate);
    missCount++;
    record(entry.pipeline, desc);
    if (entry.pipeline.id == SG_INVALID_ID)
        return entry.pipeline;
    const sg_pipeline pipeline = entry.pipeline;
    internedKeys()[pipeline.id] = key;
    map.emplace(key, std::move(entry));
    return pipeline;
}

void SokolPipelineCache::release(sg_pipeline pipeline){
    if (pipeline.id == SG_INVALID_ID)
        return;
    std::lock_guard<std::mutex> lock(cacheMutex());
    auto keyIt = internedKeys().find(pipeline.id);
    if (keyIt == internedKeys().end())
        return;
    auto& map = interned();
    auto it = map.find(keyIt->second);
    if (it == map.end() || it->second.pipeline.id != pipeline.id){
        internedKeys().erase(keyIt);
        return;
    }
    if (--it->second.refs > 0)
        return;
    // destroyed by the next collect() round unless acquired again before
    retiring().push_back(Retiring{keyIt->second, pipeline.id, collectRound});
}

void SokolPipelineCache::collect(){
    std::lock_guard<std::mutex> lock(cacheMutex());
    const uint64_t round = collectRound++;
    auto& list = retiring();
    if (list.empty())
        return;
    auto& map = interned();
    size_t kept = 0;
    for (size_t i = 0; i < list.size(); i++){
        const Retiring item = list[i];
        auto it = map.find(item.key);
        // gone with its shader, or another pipeline on the key now
        if (it == map.end() || it->second.pipeline.id != item.pipeline)
            continue;
        // acquired again
        if (it->second.refs > 0)
            continue;
        // released since the last round: one more frame
        if (item.round >= round){
            list[kept++] = item;
            continue;
        }
        pipelineGeneration++;
        if (item.pipeline == lastApplied)
            lastApplied = SG_INVALID_ID;
        destroyCachedPipeline(it->second.pipeline);
        internedKeys().erase(item.pipeline);
        map.erase(it);
    }
    list.resize(kept);
}

bool SokolPipelineCache::retain(sg_pipeline pipeline){
    std::lock_guard<std::mutex> lock(cacheMutex());
    auto keyIt = internedKeys().find(pipeline.id);
    if (keyIt == internedKeys().end())
        return false;
    auto it = interned().find(keyIt->second);
    if (it == interned().end() || it->second.pipeline.id != pipeline.id)
        return false;
    it->second.refs++;
    return true;
}

void SokolPipelineCache::setRecorder(std::function<void(const sg_pipeline_desc& desc)> function){
    std::lock_guard<std::mutex> lock(cacheMutex());
    recorder() = std::move(function);
    recorded().clear();
}

size_t SokolPipelineCache::internedSize(){
    std::lock_guard<std::mutex> lock(cacheMutex());
    return interned().size();
}

sg_pipeline SokolPipelineCache::get(const sg_pipeline_desc& base, const PipelineState& state){
    return get(base, baseHashOf(base), state, false);
}

uint64_t SokolPipelineCache::baseHash(const sg_pipeline_desc& base){
    return baseHashOf(base);
}

uint32_t SokolPipelineCache::generation(){
    return pipelineGeneration.load();
}

sg_pipeline SokolPipelineCache::get(const sg_pipeline_desc& base, uint64_t baseHash, const PipelineState& state, bool immediate){
    uint64_t key = baseHash ^ state.hash();

    std::lock_guard<std::mutex> lock(cacheMutex());
    auto& map = entries();
    // a different (shader, layout, state) on the same key is a hash collision: the next key
    for (;;){
        auto it = map.find(key);
        if (it == map.end())
            break;
        const PipelineCacheEntry& entry = it->second;
        if (entry.shader.id == base.shader.id && entry.baseHash == baseHash && entry.state == state){
            if (recorder() && !recorded().count(entry.pipeline.id))
                record(entry.pipeline, makeDesc(base, state));
            finishNow(entry.pipeline, immediate);
            return entry.pipeline;
        }
        key++;
    }

    if (Engine::isAsyncThread()){
        Log::error("Dynamic pipelines are created on the main thread only");
        sg_pipeline invalid = {SG_INVALID_ID};
        return invalid;
    }

    sg_pipeline_desc desc = makeDesc(base, state);
    // off the render thread where creation compiles (spec S32): pending (not drawn) until then
    sg_pipeline pipeline = immediate ? SokolAsyncCreate::makePipelineNow(desc) : SokolAsyncCreate::makePipeline(desc);
    missCount++;
    record(pipeline, desc);
    if (pipeline.id == SG_INVALID_ID)
        return pipeline;

    map[key] = PipelineCacheEntry{pipeline, base.shader, baseHash, state};
    return pipeline;
}

void SokolPipelineCache::releaseShader(sg_shader shader){
    if (shader.id == SG_INVALID_ID)
        return;

    std::lock_guard<std::mutex> lock(cacheMutex());
    pipelineGeneration++;
    // interned pipelines on it go too, referenced or not: a holder sees an invalid pipeline and
    // does not draw until it loads again (SokolObject::beginDraw)
    auto& internMap = interned();
    for (auto it = internMap.begin(); it != internMap.end();){
        if (it->second.desc.shader.id == shader.id){
            if (it->second.pipeline.id == lastApplied)
                lastApplied = SG_INVALID_ID;
            destroyCachedPipeline(it->second.pipeline);
            internedKeys().erase(it->second.pipeline.id);
            variantRefs().erase(it->second.pipeline.id);
            it = internMap.erase(it);
        }else{
            ++it;
        }
    }
    auto& map = entries();
    for (auto it = map.begin(); it != map.end();){
        if (it->second.shader.id == shader.id){
            if (it->second.pipeline.id == lastApplied)
                lastApplied = SG_INVALID_ID;
            destroyCachedPipeline(it->second.pipeline);
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
        destroyCachedPipeline(pair.second.pipeline);
    }
    map.clear();
    for (auto& pair : interned()){
        destroyCachedPipeline(pair.second.pipeline);
    }
    interned().clear();
    internedKeys().clear();
    retiring().clear();
    variantRefs().clear();
    lastApplied = SG_INVALID_ID;
}

// The variants are interned pipelines like SokolObject's own (spec S32 R3): made through the
// asynchronous creation (pending until ready, as any interned pipeline) and shared by equal
// descriptions. variantRefs() only counts which of them are held as variants.
sg_pipeline SokolPipelineCache::acquireVariant(const sg_pipeline_desc& desc){
    const sg_pipeline pipeline = acquire(desc);
    if (pipeline.id == SG_INVALID_ID)
        return pipeline;
    std::lock_guard<std::mutex> lock(cacheMutex());
    variantRefs()[pipeline.id]++;
    return pipeline;
}

void SokolPipelineCache::retainVariant(sg_pipeline pipeline){
    // (after shutdown, clear() destroyed them all: also at exit, when the maps may be gone)
    if (pipeline.id == SG_INVALID_ID || !sg_isvalid())
        return;
    if (!retain(pipeline))
        return;
    std::lock_guard<std::mutex> lock(cacheMutex());
    variantRefs()[pipeline.id]++;
}

void SokolPipelineCache::releaseVariant(sg_pipeline pipeline){
    // (after shutdown, clear() destroyed them all: also at exit, when the maps may be gone)
    if (pipeline.id == SG_INVALID_ID || !sg_isvalid())
        return;
    {
        std::lock_guard<std::mutex> lock(cacheMutex());
        auto it = variantRefs().find(pipeline.id);
        if (it == variantRefs().end())
            return;
        if (--it->second <= 0)
            variantRefs().erase(it);
    }
    release(pipeline);
}

size_t SokolPipelineCache::variantCount(){
    std::lock_guard<std::mutex> lock(cacheMutex());
    return variantRefs().size();
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
    lastAppliedEpoch++;
    applyCount++;
    return true;
}

void SokolPipelineCache::invalidateApplied(){
    lastApplied = SG_INVALID_ID;
    lastAppliedEpoch++;
}

uint32_t SokolPipelineCache::appliedEpoch(){
    return lastAppliedEpoch;
}

uint32_t SokolPipelineCache::appliedPipeline(){
    return lastApplied;
}

uint64_t SokolPipelineCache::getMissCount(){
    return missCount;
}

uint64_t SokolPipelineCache::getApplyCount(){
    return applyCount;
}
