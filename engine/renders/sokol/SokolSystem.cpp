// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolSystem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "System.h"
#include "sokol_gfx.h"
#include "sokol_gpu_timer.h"
#include "SokolCmdQueue.h"
#include "SokolPassPreview.h"
#include "SokolPassRecorder.h"
#include "SokolPipelineCache.h"
#include "SokolTraceHub.h"
#include "render/SystemRender.h"
#include "Log.h"
#if JASPERENGINE_PROFILER
#include "profiler/GpuMemory.h"
#endif

#include <chrono>

using namespace jasperengine;

namespace {
    RenderFeatures features;
    bool featuresQueried = false;
    // JASPERENGINE_FRAME_STATS asked for the per-frame line at setup
    bool frameStatsRequested = false;

    // GPU timing consumers (acquireGpuTiming) and the end of the current lease (leaseGpuTiming)
    int gpuTimingConsumers = 0;
    std::chrono::steady_clock::time_point gpuTimingLeaseEnd;

    bool gpuTimingWanted(){
        return gpuTimingConsumers > 0 || std::chrono::steady_clock::now() < gpuTimingLeaseEnd;
    }

#if JASPERENGINE_PROFILER
    gpumem::DriverMemory driverMemory(){
        gpumem::DriverMemory out;
        sgj_device_memory memory = {};
        if (sg_isvalid() && sgj_query_device_memory(&memory)){
            out.known = true;
            out.source = memory.source;
            out.usedBytes = memory.used_bytes;
            out.usedKnown = memory.used_known;
            out.budgetBytes = memory.budget_bytes;
            out.availableBytes = memory.available_bytes;
        }
        return out;
    }
#endif
}

void sokol_log(const char* tag,                // e.g. 'sg'
                    uint32_t log_level,             // 0=panic, 1=error, 2=warn, 3=info
                    uint32_t log_item_id,           // SG_LOGITEM_*
                    const char* message_or_null,    // a message string, may be nullptr in release mode
                    uint32_t line_nr,               // line number in sokol_gfx.h
                    const char* filename_or_null,   // source filename, may be nullptr in release mode
                    void* user_data){
    // The GL backend warns (and logs the resource name) when the driver strips an unused
    // uniform block or image-sampler. This is benign and expected for custom/simplified
    // shaders that don't use every binding their variant provides, so log it at debug only.
    if (log_item_id == SG_LOGITEM_GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER ||
        log_item_id == SG_LOGITEM_GL_IMAGE_SAMPLER_NAME_NOT_FOUND_IN_SHADER){
        Log::debug("%s\n", message_or_null);
        return;
    }
    // release builds of sokol carry no message: name the log item and its line instead
    char unnamed[96];
    if (!message_or_null){
        snprintf(unnamed, sizeof(unnamed), "sokol %s log item %u (line %u)", tag ? tag : "", log_item_id, line_nr);
        message_or_null = unnamed;
    }
    if (log_level == 0){
        Log::print("(PANIC): %s\n", message_or_null);
    }else if (log_level == 1){
        Log::error("%s\n", message_or_null);
    }else if (log_level == 2){
        Log::warn("%s\n", message_or_null);
    }else if (log_level == 3){
        Log::verbose("%s\n", message_or_null);
    }
}


void SokolSystem::setup(){
    /* setup sokol_gfx */
    sg_desc desc = {0};
    desc.buffer_pool_size = 8192; //default: 128
    desc.image_pool_size = 2048; //default: 128
    desc.sampler_pool_size = 2048; //default: 64
    desc.view_pool_size = 4096; //default: 256 (texture, storage-buffer and attachment views)
    desc.pipeline_pool_size = 8192; //default: 64
    desc.shader_pool_size = 1024; //default: 64 (one per ShaderType+properties variant)
    desc.environment = System::instance().getSokolEnvironment();
    desc.logger.func = sokol_log;

    sg_setup(&desc);

    // diagnostics: the passes, draws and resources of each frame (getFrameStatsLine); with the
    // profiler, sokol's counters are always on (get_render_stats' upload bytes)
    const char* frameStats = std::getenv("JASPERENGINE_FRAME_STATS");
    frameStatsRequested = frameStats && frameStats[0] != '\0' && frameStats[0] != '0';
    SokolPassRecorder::setFrameStatsListing(frameStatsRequested);
    bool sokolStats = frameStatsRequested;
#if JASPERENGINE_PROFILER
    sokolStats = true;
#endif
    if (sokolStats){
        sg_enable_stats();
    }

    // the GPU timer (sokol_gpu_timer.h): off until a consumer asks; JASPERENGINE_GPU_TIMER=off
    // makes it report "unavailable: disabled"
    const char* timer = std::getenv("JASPERENGINE_GPU_TIMER");
    sgj_gpu_timer_desc timerDesc = {};
    timerDesc.disabled = timer && (std::strcmp(timer, "off") == 0 || std::strcmp(timer, "0") == 0);
    sgj_gpu_timer_setup(&timerDesc);
    gpuTimingConsumers = 0;
    gpuTimingLeaseEnd = {};

    // the pass recorder and the VRAM tracker see every resource from here on
    SokolTraceHub::install(frameStatsRequested);
#if JASPERENGINE_PROFILER
    gpumem::setDriverQuery(driverMemory);
#endif

    SokolCmdQueue::start();
}

std::string SokolSystem::getFrameStatsLine(){
    if (!frameStatsRequested || !sg_stats_enabled())
        return std::string();
    const sg_stats stats = sg_query_stats();
    const sg_frame_stats& frame = stats.prev_frame;
    char line[512];
    std::snprintf(line, sizeof(line),
        "%u passes, %u draws, %u pipelines applied, %u bindings applied, %u uniform uploads (%u bytes); "
        "images %u alive (%u made, %u made last frame), pipelines %u alive (%u made), views %u alive (%u made)",
        frame.num_passes, frame.num_draw + frame.num_draw_ex, frame.num_apply_pipeline, frame.num_apply_bindings,
        frame.num_apply_uniforms, frame.size_apply_uniforms,
        stats.total.images.alive, stats.total.images.allocated, frame.images.allocated,
        stats.total.pipelines.alive, stats.total.pipelines.allocated,
        stats.total.views.alive, stats.total.views.allocated);
    // the pass list: label and target size of each SokolCamera pass in order
    const std::string list = SokolPassRecorder::getFrameStatsPassList();
    return std::string(line) + "; pass list: " + list;
}

void SokolSystem::commitQueue(){
    SokolCmdQueue::commit_commands();
}

void SokolSystem::executeQueue(){
    SokolCmdQueue::execute_commands();
}

void SokolSystem::commit(){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    sg_commit();
    // the GPU times of earlier commits that resolved meanwhile (never waits)
    sgj_commit_times times;
    while (sgj_gpu_timer_pop(&times)){
        SokolPassRecorder::onGpuTimes(times);
    }
    if (sgj_gpu_timer_enabled() && !gpuTimingWanted())
        sgj_gpu_timer_set_enabled(false);
}

void SokolSystem::shutdown(){
    SokolCmdQueue::flush_commands();
    SokolCmdQueue::wait_for_flush();
    SokolCmdQueue::finish();
    SokolPassPreview::destroy();
    SokolPipelineCache::clear();
    SokolTraceHub::uninstall();
#if JASPERENGINE_PROFILER
    gpumem::setDriverQuery(nullptr);
    gpumem::clearAll();
#endif
    sgj_gpu_timer_shutdown();
    sg_shutdown();
    featuresQueried = false;
    features = RenderFeatures();
}

const RenderFeatures& SokolSystem::getFeatures(){
    if (!featuresQueried && sg_isvalid()){
        const sg_features sgFeatures = sg_query_features();
        features.originTopLeft = sgFeatures.origin_top_left;
        features.msaaRGBA8 = sg_query_pixelformat(SG_PIXELFORMAT_RGBA8).msaa;
        features.msaaDepthStencil = sg_query_pixelformat(SG_PIXELFORMAT_DEPTH_STENCIL).msaa;
        const sg_pixelformat_info rgba16f = sg_query_pixelformat(SG_PIXELFORMAT_RGBA16F);
        const sg_pixelformat_info rgba32f = sg_query_pixelformat(SG_PIXELFORMAT_RGBA32F);
        const sg_pixelformat_info r32f = sg_query_pixelformat(SG_PIXELFORMAT_R32F);
        features.renderRGBA16F = rgba16f.render;
        features.blendRGBA16F = rgba16f.blend;
        features.filterRGBA16F = rgba16f.filter;
        features.renderRGBA32F = rgba32f.render;
        features.blendRGBA32F = rgba32f.blend;
        features.filterRGBA32F = rgba32f.filter;
        features.renderR32F = r32f.render;
        features.blendR32F = r32f.blend;
        features.filterR32F = r32f.filter;
        featuresQueried = true;
    }
    return features;
}

uint64_t SokolSystem::getPipelineCacheMisses(){
    return SokolPipelineCache::getMissCount();
}

uint64_t SokolSystem::getPipelineApplies(){
    return SokolPipelineCache::getApplyCount();
}

size_t SokolSystem::getPipelineCacheSize(){
    return SokolPipelineCache::size();
}

void SokolSystem::scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer){
    SokolCmdQueue::schedule_cleanup(cleanupFunc, cleanupData, numFramesToDefer);
}

void SokolSystem::addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data){
    SokolCmdQueue::add_command_custom(custom_cb, custom_data);
}

void SokolSystem::setViewTag(uint32_t tag){
    SokolPassRecorder::setViewTag(tag);
    sgj_gpu_timer_set_view_tag(tag);
}

uint32_t SokolSystem::getViewTag(){
    return SokolPassRecorder::getViewTag();
}

RenderStats SokolSystem::getRenderStats(uint32_t viewTag){
    RenderStats stats;
    stats.viewTag = viewTag;
    const PassList list = SokolPassRecorder::getPassList(viewTag, false);
    stats.frame = list.frame;
    stats.valid = !list.passes.empty();
    for (const PassRecord& pass : list.passes){
        if (!pass.executed)
            continue;
        stats.passes++;
        stats.draws += pass.draws;
        stats.pipelinesApplied += pass.pipelines;
        stats.bindingsApplied += pass.bindings;
        stats.uniformBytes += pass.uniformBytes;
    }
    if (sg_isvalid()){
        const sg_stats sgStats = sg_query_stats();
        if (sg_stats_enabled()){
            stats.imageUploadBytes = sgStats.prev_frame.size_update_image;
            stats.bufferUploadBytes = sgStats.prev_frame.size_update_buffer + sgStats.prev_frame.size_append_buffer;
        }
        stats.aliveImages = sgStats.total.images.alive;
        stats.aliveBuffers = sgStats.total.buffers.alive;
        stats.alivePipelines = sgStats.total.pipelines.alive;
        stats.aliveViews = sgStats.total.views.alive;
        stats.aliveShaders = sgStats.total.shaders.alive;
        stats.aliveSamplers = sgStats.total.samplers.alive;
    }
    stats.pipelineCacheMisses = SokolPipelineCache::getMissCount();
    stats.pipelineCacheApplies = SokolPipelineCache::getApplyCount();
    stats.pipelineCacheSize = SokolPipelineCache::size();
    const PassList timed = SokolPassRecorder::getPassList(viewTag, true);
    stats.gpuFrameMs = timed.gpuFrameMs;
    stats.gpuFrameValid = timed.gpuFrameValid;
    return stats;
}

PassList SokolSystem::getPassList(uint32_t viewTag, bool resolvedOnly){
    return SokolPassRecorder::getPassList(viewTag, resolvedOnly);
}

std::vector<uint32_t> SokolSystem::getRecordedViewTags(){
    return SokolPassRecorder::getRecordedViewTags();
}

void SokolSystem::setDrawListEnabled(bool enabled){
    SokolPassRecorder::setDrawListEnabled(enabled);
}

void SokolSystem::acquireGpuTiming(){
    gpuTimingConsumers++;
    sgj_gpu_timer_set_enabled(true);
}

void SokolSystem::releaseGpuTiming(){
    if (gpuTimingConsumers > 0)
        gpuTimingConsumers--;
    // switched off at the next commit unless a lease still runs
}

void SokolSystem::leaseGpuTiming(double seconds){
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
    if (end > gpuTimingLeaseEnd)
        gpuTimingLeaseEnd = end;
    sgj_gpu_timer_set_enabled(true);
}

GpuTimingState SokolSystem::getGpuTimingState(){
    GpuTimingState state;
    const char* reason = "";
    state.available = sgj_gpu_timer_state(&reason) == SGJ_TIMER_AVAILABLE;
    if (!state.available)
        state.reason = reason;
    state.passReason = sgj_gpu_timer_reason_name(sgj_gpu_timer_pass_reason());
    state.semantics = sgj_gpu_timer_semantics_name(sgj_gpu_timer_semantics());
    state.frameIsSum = sgj_gpu_timer_semantics() == SGJ_SEM_ELAPSED;
    state.enabled = sgj_gpu_timer_enabled();
    const sgj_gpu_timer_stats timerStats = sgj_gpu_timer_get_stats();
    state.resolvedCommits = timerStats.resolved_commits;
    state.droppedDisjoint = timerStats.dropped_disjoint;
    state.droppedOverflow = timerStats.dropped_overflow;
    state.droppedDisabled = timerStats.dropped_disabled;
    return state;
}

BackendFrameCounters SokolSystem::getBackendFrameCounters(){
    BackendFrameCounters counters;
    if (!sg_isvalid() || !sg_stats_enabled())
        return counters;
    const sg_frame_stats frame = sg_query_stats().prev_frame;
    counters.valid = true;
    counters.passes = frame.num_passes;
    counters.draws = frame.num_draw + frame.num_draw_ex;
    counters.pipelines = frame.num_apply_pipeline;
    counters.bindings = frame.num_apply_bindings;
    counters.uniformBytes = frame.size_apply_uniforms;
    counters.imageUploadBytes = frame.size_update_image;
    counters.bufferUploadBytes = frame.size_update_buffer + frame.size_append_buffer;
    return counters;
}

const char* SokolSystem::getBackendName(){
    if (!sg_isvalid())
        return "none";
    switch (sg_query_backend()){
        case SG_BACKEND_GLCORE: return "OpenGL 3.3 core";
        case SG_BACKEND_GLES3: return "OpenGL ES 3 / WebGL2";
        case SG_BACKEND_D3D11: return "Direct3D 11";
        case SG_BACKEND_METAL_IOS:
        case SG_BACKEND_METAL_MACOS:
        case SG_BACKEND_METAL_SIMULATOR: return "Metal";
        case SG_BACKEND_WGPU: return "WebGPU";
        case SG_BACKEND_VULKAN: return "Vulkan";
        case SG_BACKEND_DUMMY: return "dummy";
    }
    return "unknown";
}

std::string SokolSystem::getDeviceName(){
    return sg_isvalid() ? std::string(sgj_device_name()) : std::string();
}

void SokolSystem::setVulkanHostQueryReset(bool enabled){
    sgj_gpu_timer_set_vulkan_host_query_reset(enabled);
}
