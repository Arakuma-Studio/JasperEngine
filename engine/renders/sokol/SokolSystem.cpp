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
#include "SokolCmdQueue.h"
#include "SokolPipelineCache.h"
#include "render/SystemRender.h"
#include "Log.h"

using namespace jasperengine;

namespace {
    RenderFeatures features;
    bool featuresQueried = false;
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

    // diagnostics: the passes, draws and resources of each frame (getFrameStatsLine)
    const char* frameStats = std::getenv("JASPERENGINE_FRAME_STATS");
    if (frameStats && frameStats[0] != '\0' && frameStats[0] != '0'){
        sg_enable_stats();
    }

    SokolCmdQueue::start();
}

namespace{
    // the passes of the frame being drawn and of the last committed one (frame stats only)
    struct PassRecord{
        const char* label;
        int width;
        int height;
        int samples;
        // (JASPERENGINE_FRAME_STATS_ACTIONS) the colour's load action, the depth's store
        // action, and whether the pass has a depth attachment
        sg_load_action colorLoad;
        sg_store_action depthStore;
        bool depth;
    };
    std::vector<PassRecord> framePasses;
    std::vector<PassRecord> lastFramePasses;

    // JASPERENGINE_FRAME_STATS_ACTIONS=1: the pass list names each pass's colour load action and
    // depth store action (render-target hygiene: what a tiler loads and stores)
    bool passActionsListed(){
        static const bool listed = [](){
            const char* value = std::getenv("JASPERENGINE_FRAME_STATS_ACTIONS");
            return value && value[0] != '\0' && value[0] != '0';
        }();
        return listed;
    }
}

void SokolSystem::recordPass(const sg_pass& pass){
    if (!sg_stats_enabled())
        return;
    PassRecord record = {pass.label ? pass.label : "pass", 0, 0, 1,
                         pass.action.colors[0].load_action, pass.action.depth.store_action, true};
    if (pass.attachments.colors[0].id != SG_INVALID_ID || pass.attachments.depth_stencil.id != SG_INVALID_ID)
        record.depth = pass.attachments.depth_stencil.id != SG_INVALID_ID;
    sg_view view = pass.attachments.colors[0].id != SG_INVALID_ID ? pass.attachments.colors[0] : pass.attachments.depth_stencil;
    if (view.id != SG_INVALID_ID){
        sg_image image = sg_query_view_image(view);
        record.width = sg_query_image_width(image);
        record.height = sg_query_image_height(image);
        record.samples = sg_query_image_sample_count(image);
    }else{
        record.width = pass.swapchain.width;
        record.height = pass.swapchain.height;
        record.samples = pass.swapchain.sample_count > 0 ? pass.swapchain.sample_count : 1;
    }
    framePasses.push_back(record);
}

std::string SokolSystem::getFrameStatsLine(){
    if (!sg_stats_enabled())
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
    // the pass list: label and target size of each pass in order, repeats folded ("shadow
    // 2048x2048 x3"), "msaa N" for a multisampled target
    std::string list;
    for (size_t i = 0; i < lastFramePasses.size();){
        const PassRecord& record = lastFramePasses[i];
        size_t count = 1;
        const bool actions = passActionsListed();
        while (i + count < lastFramePasses.size() && std::strcmp(lastFramePasses[i + count].label, record.label) == 0 &&
               lastFramePasses[i + count].width == record.width && lastFramePasses[i + count].height == record.height &&
               lastFramePasses[i + count].samples == record.samples &&
               (!actions || (lastFramePasses[i + count].colorLoad == record.colorLoad &&
                             lastFramePasses[i + count].depthStore == record.depthStore &&
                             lastFramePasses[i + count].depth == record.depth))){
            count++;
        }
        char item[200];
        std::snprintf(item, sizeof(item), "%s%s %dx%d%s", list.empty() ? "" : ", ", record.label, record.width, record.height,
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
    if (sg_stats_enabled()){
        lastFramePasses.swap(framePasses);
        framePasses.clear();
    }
}

void SokolSystem::shutdown(){
    SokolCmdQueue::flush_commands();
    SokolCmdQueue::wait_for_flush();
    SokolCmdQueue::finish();
    SokolPipelineCache::clear();
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
        const sg_pixelformat_info rg16f = sg_query_pixelformat(SG_PIXELFORMAT_RG16F);
        const sg_pixelformat_info r16f = sg_query_pixelformat(SG_PIXELFORMAT_R16F);
        features.renderRGBA16F = rgba16f.render;
        features.blendRGBA16F = rgba16f.blend;
        features.filterRGBA16F = rgba16f.filter;
        features.renderRGBA32F = rgba32f.render;
        features.blendRGBA32F = rgba32f.blend;
        features.filterRGBA32F = rgba32f.filter;
        features.renderR32F = r32f.render;
        features.blendR32F = r32f.blend;
        features.filterR32F = r32f.filter;
        features.renderRG16F = rg16f.render;
        features.blendRG16F = rg16f.blend;
        features.filterRG16F = rg16f.filter;
        features.renderR16F = r16f.render;
        features.blendR16F = r16f.blend;
        features.filterR16F = r16f.filter;
        features.maxTextureSize2D = sg_query_limits().max_image_size_2d;
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
