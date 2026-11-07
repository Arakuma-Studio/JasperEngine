// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolPassRecorder.h"
#include "SokolPassPreview.h"

#include "profiler/FrameTimes.h"
#include "render/SystemRender.h"
#include "sokol_gfx.h"
#include "sokol_gpu_timer.h"
#if JASPERENGINE_PROFILER
#include "SokolVramTracker.h"
#include "profiler/Profiler.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace jasperengine;

namespace {

    struct PipelineInfo {
        sg_primitive_type primitive = SG_PRIMITIVETYPE_TRIANGLES;
        uint32_t shader = 0;
        char label[RENDER_STATS_LABEL_LEN] = {};
    };

    struct ShaderInfo {
        char label[RENDER_STATS_LABEL_LEN] = {};
    };

    // one commit: its passes and (when recorded) draws
    struct CommitRecord {
        uint64_t commit = 0;
        uint64_t frame = 0;
        uint32_t viewTag = 0;
        bool timed = false;         // the GPU timer was on: its times will arrive (or be dropped)
        bool resolved = false;      // they arrived
        double gpuFrameMs = 0.0;
        bool gpuFrameValid = false;
        bool gpuFrameIsSum = false;
        uint32_t droppedPasses = 0;
        std::vector<PassRecord> passes;
        std::vector<DrawRecord> draws;
    };

    // the JASPERENGINE_FRAME_STATS record of a SokolCamera pass (what it printed before S15)
    struct ListedPass {
        std::string label;
        int width = 0;
        int height = 0;
        int samples = 1;
        sg_load_action colorLoad = _SG_LOADACTION_DEFAULT;
        sg_store_action depthStore = _SG_STOREACTION_DEFAULT;
        bool depth = true;
    };

    // the commits of the last frames: an editor frame commits once per view it draws
    constexpr size_t COMMIT_RING = 48;

    struct State {
        bool enabled = false;
        uint32_t viewTag = SystemRender::FRAME_STATS_FRAME;
        bool listNext = false;
        bool hideNext = false;
        bool lastEnded = false;
        bool frameStatsListing = false;
        bool drawList = false;
        // the next draw's batch annotation (annotateNextDraw)
        uint16_t nextBatchItems = 0;
        uint8_t nextBatchBreak = 0;
        std::unordered_map<uint32_t, PipelineInfo> pipelines;
        std::unordered_map<uint32_t, ShaderInfo> shaders;
        const PipelineInfo* currentPipeline = nullptr;

        CommitRecord current;
        bool inPass = false;
        std::chrono::steady_clock::time_point passStart;
        // the attachments of each pass are queried only while someone reads pass lists (the
        // queries cost a few microseconds a pass: the idle editor's frame would pay them)
        std::chrono::steady_clock::time_point detailUntil{};

        CommitRecord ring[COMMIT_RING];
        size_t ringHead = 0;        // the next write
        size_t ringCount = 0;

        std::vector<ListedPass> listed;
        std::vector<ListedPass> lastListed;
    };

    State& state(){
        static State s;
        return s;
    }

    bool detailWanted(const State& s, std::chrono::steady_clock::time_point now){
        if (s.drawList || now < s.detailUntil || sgj_gpu_timer_enabled() || SokolPassPreview::isRequested())
            return true;
    #if JASPERENGINE_PROFILER
        return profiler::isEnabled();
    #else
        return false;
    #endif
    }

    // JASPERENGINE_FRAME_STATS_ACTIONS=1: the pass list names each pass's colour load action and
    // depth store action (render-target hygiene: what a tiler loads and stores)
    bool passActionsListed(){
        static const bool listed = [](){
            const char* value = std::getenv("JASPERENGINE_FRAME_STATS_ACTIONS");
            return value && value[0] != '\0' && value[0] != '0';
        }();
        return listed;
    }

    void copyLabel(char* out, const char* label, const char* fallback){
        const char* text = (label && label[0]) ? label : fallback;
        std::strncpy(out, text, RENDER_STATS_LABEL_LEN - 1);
        out[RENDER_STATS_LABEL_LEN - 1] = '\0';
    }

    PassLoadAction loadAction(sg_load_action action){
        switch (action){
            case SG_LOADACTION_LOAD: return PassLoadAction::Load;
            case SG_LOADACTION_DONTCARE: return PassLoadAction::DontCare;
            default: return PassLoadAction::Clear;
        }
    }

    PassStoreAction storeAction(sg_store_action action){
        return action == SG_STOREACTION_STORE ? PassStoreAction::Store : PassStoreAction::DontCare;
    }

    // the image a colour, resolve or depth view renders into, at its mip level and slice
    void fillAttachment(PassAttachment& out, sg_view view, int kind){
        if (view.id == SG_INVALID_ID)
            return;
        const sg_view_desc desc = sg_query_view_desc(view);
        const sg_image_view_desc& part = kind == 0 ? desc.color_attachment : (kind == 1 ? desc.resolve_attachment : desc.depth_stencil_attachment);
        const sg_image image = sg_query_view_image(view);
        out.used = true;
        out.image = image.id;
        out.mip = part.mip_level;
        out.slice = part.slice;
        out.width = std::max(1, sg_query_image_width(image) >> part.mip_level);
        out.height = std::max(1, sg_query_image_height(image) >> part.mip_level);
        out.samples = std::max(1, sg_query_image_sample_count(image));
        out.format = sgj_pixel_format_name((int)sg_query_image_pixelformat(image));
        const sg_image_type type = sg_query_image_type(image);
        out.cube = type == SG_IMAGETYPE_CUBE;
        out.array = type == SG_IMAGETYPE_ARRAY;
        #if JASPERENGINE_PROFILER
        copyLabel(out.label, SokolVramTracker::imageLabel(image.id), "");
        #endif
    }

    uint64_t trianglesOf(sg_primitive_type primitive, int elements, int instances){
        const uint64_t count = (uint64_t)std::max(instances, 1);
        switch (primitive){
            case SG_PRIMITIVETYPE_TRIANGLES: return (uint64_t)(elements / 3) * count;
            case SG_PRIMITIVETYPE_TRIANGLE_STRIP: return (uint64_t)std::max(elements - 2, 0) * count;
            default: return 0;
        }
    }

    // GPU times of commits older than `commit` that never came: the timer dropped them
    void dropPendingBefore(State& s, uint64_t commit){
        for (size_t i = 0; i < s.ringCount; i++){
            CommitRecord& record = s.ring[(s.ringHead + COMMIT_RING - 1 - i) % COMMIT_RING];
            if (record.commit >= commit || !record.timed || record.resolved)
                continue;
            record.timed = false;
            for (PassRecord& pass : record.passes)
                pass.gpuPending = false;
        }
    }

    // a view's commits in one frame, oldest first
    std::vector<const CommitRecord*> commitsOf(const State& s, uint32_t viewTag, uint64_t frame){
        std::vector<const CommitRecord*> out;
        for (size_t i = 0; i < s.ringCount; i++){
            const CommitRecord& record = s.ring[(s.ringHead + COMMIT_RING - s.ringCount + i) % COMMIT_RING];
            if (record.frame == frame && record.viewTag == viewTag)
                out.push_back(&record);
        }
        return out;
    }

    bool allResolved(const std::vector<const CommitRecord*>& commits){
        for (const CommitRecord* record : commits){
            if (record->timed && !record->resolved)
                return false;
        }
        return true;
    }

    bool anyTimed(const std::vector<const CommitRecord*>& commits){
        for (const CommitRecord* record : commits){
            if (record->resolved)
                return true;
        }
        return false;
    }

}

void SokolPassRecorder::setEnabled(bool enabled){
    state().enabled = enabled;
}

bool SokolPassRecorder::isEnabled(){
    return state().enabled;
}

void SokolPassRecorder::setViewTag(uint32_t tag){
    state().viewTag = tag;
}

uint32_t SokolPassRecorder::getViewTag(){
    return state().viewTag;
}

void SokolPassRecorder::listPass(const sg_pass& pass){
    State& s = state();
    s.listNext = true;
    // the Frame Debugger's preview blit is the editor's, not the frame's
    if (s.hideNext || !s.frameStatsListing || !sg_stats_enabled())
        return;
    // the frame stats line's record, made from the pass SokolCamera begins, as before the recorder
    ListedPass record;
    record.label = pass.label ? pass.label : "pass";
    record.colorLoad = pass.action.colors[0].load_action;
    record.depthStore = pass.action.depth.store_action;
    if (pass.attachments.colors[0].id != SG_INVALID_ID || pass.attachments.depth_stencil.id != SG_INVALID_ID)
        record.depth = pass.attachments.depth_stencil.id != SG_INVALID_ID;
    const sg_view view = pass.attachments.colors[0].id != SG_INVALID_ID ? pass.attachments.colors[0] : pass.attachments.depth_stencil;
    if (view.id != SG_INVALID_ID){
        const sg_image image = sg_query_view_image(view);
        record.width = sg_query_image_width(image);
        record.height = sg_query_image_height(image);
        record.samples = sg_query_image_sample_count(image);
    }else{
        record.width = pass.swapchain.width;
        record.height = pass.swapchain.height;
        record.samples = pass.swapchain.sample_count > 0 ? pass.swapchain.sample_count : 1;
    }
    s.listed.push_back(std::move(record));
}

void SokolPassRecorder::hideNextPass(){
    state().hideNext = true;
}

const PassRecord* SokolPassRecorder::lastEndedPass(){
    State& s = state();
    return (s.enabled && s.lastEnded && !s.current.passes.empty()) ? &s.current.passes.back() : nullptr;
}

void SokolPassRecorder::setFrameStatsListing(bool listing){
    state().frameStatsListing = listing;
}

void SokolPassRecorder::setDrawListEnabled(bool enabled){
    state().drawList = enabled;
}

bool SokolPassRecorder::isDrawListEnabled(){
    return state().drawList;
}

void SokolPassRecorder::onMakePipeline(uint32_t pipeline, const sg_pipeline_desc* desc){
    State& s = state();
    if (!desc || pipeline == SG_INVALID_ID)
        return;
    PipelineInfo& info = s.pipelines[pipeline];
    info.primitive = desc->primitive_type;
    info.shader = desc->shader.id;
    copyLabel(info.label, desc->label, "pipeline");
    s.currentPipeline = nullptr;
}

void SokolPassRecorder::onDestroyPipeline(uint32_t pipeline){
    State& s = state();
    s.pipelines.erase(pipeline);
    s.currentPipeline = nullptr;
}

void SokolPassRecorder::onMakeShader(uint32_t shader, const sg_shader_desc* desc){
    if (!desc || shader == SG_INVALID_ID)
        return;
    copyLabel(state().shaders[shader].label, desc->label, "shader");
}

void SokolPassRecorder::onDestroyShader(uint32_t shader){
    state().shaders.erase(shader);
}

void SokolPassRecorder::onBeginPass(const sg_pass* pass){
    State& s = state();
    const bool listedPass = s.listNext;
    const bool hiddenPass = s.hideNext;
    s.listNext = false;
    s.hideNext = false;
    s.lastEnded = false;
    if (!pass)
        return;

    if (!s.enabled)
        return;
    const auto now = std::chrono::steady_clock::now();
    const bool detail = detailWanted(s, now);
    PassRecord record;
    record.index = (uint32_t)s.current.passes.size();
    record.viewTag = s.viewTag;
    record.listed = listedPass;
    record.hidden = hiddenPass;
    record.detailed = detail;
    copyLabel(record.label, pass->label, "pass");
    record.compute = pass->compute;
    for (int i = 0; i < RENDER_STATS_MAX_COLORS; i++){
        if (detail){
            fillAttachment(record.colors[i], pass->attachments.colors[i], 0);
            fillAttachment(record.resolves[i], pass->attachments.resolves[i], 1);
        }else{
            record.colors[i].used = pass->attachments.colors[i].id != SG_INVALID_ID;
            record.resolves[i].used = pass->attachments.resolves[i].id != SG_INVALID_ID;
        }
        if (record.colors[i].used)
            record.numColors = i + 1;
        record.colorLoad[i] = loadAction(pass->action.colors[i].load_action);
        record.colorStore[i] = storeAction(pass->action.colors[i].store_action);
    }
    if (detail)
        fillAttachment(record.depth, pass->attachments.depth_stencil, 2);
    else
        record.depth.used = pass->attachments.depth_stencil.id != SG_INVALID_ID;
    record.depthLoad = loadAction(pass->action.depth.load_action);
    record.depthStore = storeAction(pass->action.depth.store_action);
    record.stencilLoad = loadAction(pass->action.stencil.load_action);
    record.stencilStore = storeAction(pass->action.stencil.store_action);
    const PassAttachment& target = record.numColors > 0 ? record.colors[0] : record.depth;
    if (target.used && !detail){
        // sizes and samples come with the detail
    }else if (target.used){
        record.width = target.width;
        record.height = target.height;
        record.samples = target.samples;
    }else if (!record.compute){
        record.swapchain = true;
        record.width = pass->swapchain.width;
        record.height = pass->swapchain.height;
        record.samples = pass->swapchain.sample_count > 0 ? pass->swapchain.sample_count : 1;
    }
    record.firstDraw = (uint32_t)s.current.draws.size();
    s.current.passes.push_back(record);
    s.inPass = true;
    s.passStart = now;
}

void SokolPassRecorder::onApplyPipeline(uint32_t pipeline){
    State& s = state();
    if (!s.enabled || !s.inPass)
        return;
    s.current.passes.back().pipelines++;
    auto it = s.pipelines.find(pipeline);
    s.currentPipeline = it != s.pipelines.end() ? &it->second : nullptr;
}

void SokolPassRecorder::onApplyBindings(){
    State& s = state();
    if (s.enabled && s.inPass)
        s.current.passes.back().bindings++;
}

void SokolPassRecorder::onApplyUniforms(size_t bytes){
    State& s = state();
    if (s.enabled && s.inPass)
        s.current.passes.back().uniformBytes += (uint32_t)bytes;
}

void SokolPassRecorder::annotateNextDraw(uint16_t items, uint8_t reason){
    State& s = state();
    s.nextBatchItems = items;
    s.nextBatchBreak = reason;
}

void SokolPassRecorder::onDraw(int baseElement, int numElements, int numInstances){
    State& s = state();
    // (an annotation is for the one draw after it)
    const uint16_t batchItems = s.nextBatchItems;
    const uint8_t batchBreak = s.nextBatchBreak;
    s.nextBatchItems = 0;
    s.nextBatchBreak = 0;
    if (!s.enabled || !s.inPass)
        return;
    PassRecord& pass = s.current.passes.back();
    pass.draws++;
    pass.instances += (uint32_t)std::max(numInstances, 1);
    pass.elements += (uint64_t)std::max(numElements, 0);
    const sg_primitive_type primitive = s.currentPipeline ? s.currentPipeline->primitive : SG_PRIMITIVETYPE_TRIANGLES;
    pass.triangles += trianglesOf(primitive, numElements, numInstances);
    if (s.drawList){
        DrawRecord draw;
        if (s.currentPipeline){
            std::memcpy(draw.pipeline, s.currentPipeline->label, RENDER_STATS_LABEL_LEN);
            auto shader = s.shaders.find(s.currentPipeline->shader);
            if (shader != s.shaders.end())
                std::memcpy(draw.shader, shader->second.label, RENDER_STATS_LABEL_LEN);
        }
        draw.elements = (uint32_t)std::max(numElements, 0);
        draw.instances = (uint32_t)std::max(numInstances, 1);
        draw.baseElement = (uint32_t)std::max(baseElement, 0);
        draw.batchItems = batchItems;
        draw.batchBreak = batchBreak;
        s.current.draws.push_back(draw);
        pass.numDraws++;
    }
}

void SokolPassRecorder::onEndPass(){
    State& s = state();
    if (!s.enabled || !s.inPass)
        return;
    PassRecord& pass = s.current.passes.back();
    pass.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.passStart).count();
    pass.executed = sgj_pass_valid();
    s.inPass = false;
    s.lastEnded = true;
    s.currentPipeline = nullptr;
}

void SokolPassRecorder::onCommit(){
    State& s = state();
    s.lastListed.swap(s.listed);
    s.listed.clear();
    if (!s.enabled)
        return;
    s.inPass = false;
    s.lastEnded = false;
    CommitRecord& current = s.current;
    current.commit = sgj_commit_index();
    current.frame = FrameTimes::currentFrame();
    current.viewTag = s.viewTag;
    current.timed = sgj_gpu_timer_enabled() && !current.passes.empty();
    current.resolved = false;
    current.gpuFrameMs = 0.0;
    current.gpuFrameValid = false;
    current.droppedPasses = 0;
    for (PassRecord& pass : current.passes){
        pass.commit = current.commit;
        pass.viewTag = current.viewTag;
        pass.gpuPending = current.timed && pass.executed && !pass.compute;
    }
    // into the ring, reusing the evicted record's storage
    CommitRecord& slot = s.ring[s.ringHead];
    std::swap(slot, current);
    s.ringHead = (s.ringHead + 1) % COMMIT_RING;
    s.ringCount = std::min(s.ringCount + 1, COMMIT_RING);
    current.passes.clear();
    current.draws.clear();
}

void SokolPassRecorder::onGpuTimes(const sgj_commit_times& times){
    State& s = state();
    dropPendingBefore(s, times.commit_index);
    CommitRecord* record = nullptr;
    for (size_t i = 0; i < s.ringCount && !record; i++){
        CommitRecord& candidate = s.ring[(s.ringHead + COMMIT_RING - 1 - i) % COMMIT_RING];
        if (candidate.commit == times.commit_index)
            record = &candidate;
    }
    if (!record)
        return;
    // the timer saw the passes sokol began, in the same order
    uint32_t timed = 0;
    for (PassRecord& pass : record->passes){
        if (!pass.executed || pass.compute){
            pass.gpuPending = false;
            continue;
        }
        pass.gpuPending = false;
        if (timed >= times.num_passes)
            continue;
        const sgj_pass_time& result = times.passes[timed++];
        const bool same = std::strncmp(result.label, pass.label, RENDER_STATS_LABEL_LEN - 1) == 0;
        pass.gpuValid = same && result.valid;
        pass.gpuMs = pass.gpuValid ? result.gpu_ms : 0.0;
    }
    record->resolved = true;
    record->gpuFrameMs = times.frame_gpu_ms;
    record->gpuFrameValid = times.frame_valid;
    record->gpuFrameIsSum = times.frame_is_sum;
    record->droppedPasses = times.dropped_passes;

#if JASPERENGINE_PROFILER
    // the profiler's frame (its GPU lane and the game frame's GPU time)
    std::vector<profiler::GpuPassSample> samples;
    samples.reserve(record->passes.size());
    for (const PassRecord& pass : record->passes){
        if (!pass.executed || pass.compute || pass.hidden)
            continue;
        profiler::GpuPassSample sample;
        sample.label = pass.label;
        sample.gpuMs = pass.gpuMs;
        sample.valid = pass.gpuValid;
        sample.cpuMs = pass.cpuMs;
        sample.draws = pass.draws;
        samples.push_back(std::move(sample));
    }
    profiler::recordGpuFrame(record->frame, record->viewTag, record->commit, samples.data(), samples.size(), times.frame_gpu_ms,
                             times.frame_valid, record->viewTag == SystemRender::FRAME_STATS_FRAME);
#endif

    // the game frame's GPU time, once its commits are all in
    if (record->viewTag == SystemRender::FRAME_STATS_FRAME){
        double gpuMs = 0.0;
        if (getFrameGpuMs(record->viewTag, record->frame, gpuMs))
            FrameTimes::setGpuMs(record->frame, gpuMs, times.frame_is_sum);
    }
}

bool SokolPassRecorder::getFrameGpuMs(uint32_t viewTag, uint64_t frame, double& gpuMs){
    const State& s = state();
    const std::vector<const CommitRecord*> commits = commitsOf(s, viewTag, frame);
    if (commits.empty() || !allResolved(commits) || !anyTimed(commits))
        return false;
    gpuMs = 0.0;
    bool valid = false;
    for (const CommitRecord* record : commits){
        if (record->resolved && record->gpuFrameValid){
            gpuMs += record->gpuFrameMs;
            valid = true;
        }
    }
    return valid;
}

PassList SokolPassRecorder::getPassList(uint32_t viewTag, bool resolvedOnly){
    // a reader: the next frames' passes keep their attachments for a while
    state().detailUntil = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const State& s = state();
    PassList list;
    list.viewTag = viewTag;
    // the frames that drew this view, newest first; the frame still being drawn may get more
    const uint64_t current = FrameTimes::currentFrame();
    std::vector<uint64_t> frames;
    for (size_t i = 0; i < s.ringCount; i++){
        const CommitRecord& record = s.ring[(s.ringHead + COMMIT_RING - 1 - i) % COMMIT_RING];
        if (record.viewTag != viewTag || (record.frame >= current && FrameTimes::hasExternalFrames()))
            continue;
        if (std::find(frames.begin(), frames.end(), record.frame) == frames.end())
            frames.push_back(record.frame);
    }
    if (frames.empty())
        return list;
    uint64_t chosen = frames.front();
    if (resolvedOnly){
        for (uint64_t frame : frames){
            const std::vector<const CommitRecord*> commits = commitsOf(s, viewTag, frame);
            if (allResolved(commits) && anyTimed(commits)){
                chosen = frame;
                break;
            }
        }
    }
    list.frame = chosen;
    bool anyGpu = false;
    for (const CommitRecord* record : commitsOf(s, viewTag, chosen)){
        list.commits.push_back(record->commit);
        const uint32_t drawBase = (uint32_t)list.draws.size();
        for (const PassRecord& pass : record->passes){
            if (pass.hidden)
                continue;
            PassRecord copy = pass;
            copy.index = (uint32_t)list.passes.size();
            copy.firstDraw += drawBase;
            list.gpuPending = list.gpuPending || copy.gpuPending;
            list.passes.push_back(copy);
        }
        list.draws.insert(list.draws.end(), record->draws.begin(), record->draws.end());
        list.droppedPasses += record->droppedPasses;
        if (record->resolved && record->gpuFrameValid){
            list.gpuFrameMs += record->gpuFrameMs;
            anyGpu = true;
        }
    }
    list.gpuFrameValid = anyGpu && !list.gpuPending;
    return list;
}

std::vector<uint32_t> SokolPassRecorder::getRecordedViewTags(){
    const State& s = state();
    std::vector<uint32_t> tags;
    for (size_t i = 0; i < s.ringCount; i++){
        const CommitRecord& record = s.ring[(s.ringHead + COMMIT_RING - 1 - i) % COMMIT_RING];
        if (std::find(tags.begin(), tags.end(), record.viewTag) == tags.end())
            tags.push_back(record.viewTag);
    }
    return tags;
}

std::string SokolPassRecorder::getFrameStatsPassList(){
    const std::vector<ListedPass>& passes = state().lastListed;
    std::string list;
    const bool actions = passActionsListed();
    for (size_t i = 0; i < passes.size();){
        const ListedPass& record = passes[i];
        size_t count = 1;
        while (i + count < passes.size() && passes[i + count].label == record.label &&
               passes[i + count].width == record.width && passes[i + count].height == record.height &&
               passes[i + count].samples == record.samples &&
               (!actions || (passes[i + count].colorLoad == record.colorLoad &&
                             passes[i + count].depthStore == record.depthStore &&
                             passes[i + count].depth == record.depth))){
            count++;
        }
        char item[200];
        std::snprintf(item, sizeof(item), "%s%s %dx%d%s", list.empty() ? "" : ", ", record.label.c_str(), record.width, record.height,
                      record.samples > 1 ? (record.samples == 4 ? " msaa 4" : " msaa 2") : "");
        list += item;
        if (actions){
            // sokol's defaults: colour load CLEAR, depth store DONTCARE
            const char* load = record.colorLoad == SG_LOADACTION_LOAD ? "load" : (record.colorLoad == SG_LOADACTION_DONTCARE ? "dontcare" : "clear");
            const char* depth = !record.depth ? "no depth" : (record.depthStore == SG_STOREACTION_STORE ? "depth store" : "depth discard");
            list += std::string(" {") + load + ", " + depth + "}";
        }
        if (count > 1){
            list += " x" + std::to_string(count);
        }
        i += count;
    }
    return list;
}

void SokolPassRecorder::clear(){
    State& s = state();
    s.pipelines.clear();
    s.shaders.clear();
    s.currentPipeline = nullptr;
    s.current.passes.clear();
    s.current.draws.clear();
    s.inPass = false;
    for (CommitRecord& record : s.ring){
        record.passes.clear();
        record.draws.clear();
    }
    s.ringHead = 0;
    s.ringCount = 0;
    s.listed.clear();
    s.lastListed.clear();
    s.listNext = false;
}
