// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolSystem.h"

#include <cstdio>
#include <cstdlib>

#include "System.h"
#include "sokol_gfx.h"
#include "SokolCmdQueue.h"
#include "Log.h"

using namespace jasperengine;

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
    return std::string(line);
}

void SokolSystem::commitQueue(){
    SokolCmdQueue::commit_commands();
}

void SokolSystem::executeQueue(){
    SokolCmdQueue::execute_commands();
}

void SokolSystem::commit(){
    sg_commit();
}

void SokolSystem::shutdown(){
    SokolCmdQueue::flush_commands();
    SokolCmdQueue::wait_for_flush();
    SokolCmdQueue::finish();
    sg_shutdown();
}

void SokolSystem::scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer){
    SokolCmdQueue::schedule_cleanup(cleanupFunc, cleanupData, numFramesToDefer);
}

void SokolSystem::addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data){
    SokolCmdQueue::add_command_custom(custom_cb, custom_data);
}
