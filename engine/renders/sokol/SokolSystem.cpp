// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolSystem.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "System.h"
#include "Engine.h"
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

// release builds of sokol carry no message, the log item's name still says what failed
#define _SG_LOGITEM_XMACRO(item, msg) #item,
static const char* sokolLogItems[] = { _SG_LOG_ITEMS };
#undef _SG_LOGITEM_XMACRO

// (sokol.cpp, where sokol's state is visible) slots of a resource pool in use and free
extern "C" void jasperengine_sg_pool_usage(int kind, int* used, int* free);

namespace {
    // the pools, in GpuResourceKind order
    const char* const poolNames[GPU_RESOURCE_KINDS] = {"buffer", "image", "sampler", "view", "pipeline", "shader"};
    const char* const poolPlurals[GPU_RESOURCE_KINDS] = {"buffers", "images", "samplers", "views", "pipelines", "shaders"};
    int poolSizes[GPU_RESOURCE_KINDS] = {};

    // full-pool events since setup, the first of each logged; the pools full since the last
    // hasResourcePoolRoom (any thread makes resources: the loading thread too)
    std::atomic<uint64_t> poolExhaustions[GPU_RESOURCE_KINDS];
    std::atomic<bool> poolExhaustionLogged[GPU_RESOURCE_KINDS];
    std::atomic<uint32_t> poolsFull{0};

    // the frame's uniform blocks (reserveUniformBytes; draws happen on the main thread)
    uint64_t uniformLimit = 0;
    uint64_t uniformFrameBytes = 0;   // applied this frame
    uint64_t uniformFrameNeeded = 0;  // asked for this frame, refused ones too
    uint64_t uniformLastFrame = 0;
    uint64_t uniformPeak = 0;
    uint64_t uniformFrameSkipped = 0;
    uint64_t uniformSkippedDraws = 0;
    bool uniformOverflowLogged = false;

    // the backends whose uniform blocks go in sg_desc.uniform_buffer_size per frame (GL sets
    // them with glUniform, D3D11 updates a constant buffer per block)
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL) || defined(SOKOL_WGPU)
    constexpr bool uniformBufferLimited = true;
#else
    constexpr bool uniformBufferLimited = false;
#endif

    int poolOfLogItem(uint32_t item){
        switch (item){
        case SG_LOGITEM_BUFFER_POOL_EXHAUSTED: return (int)GpuResourceKind::BUFFER;
        case SG_LOGITEM_IMAGE_POOL_EXHAUSTED: return (int)GpuResourceKind::IMAGE;
        case SG_LOGITEM_SAMPLER_POOL_EXHAUSTED: return (int)GpuResourceKind::SAMPLER;
        case SG_LOGITEM_VIEW_POOL_EXHAUSTED: return (int)GpuResourceKind::VIEW;
        case SG_LOGITEM_PIPELINE_POOL_EXHAUSTED: return (int)GpuResourceKind::PIPELINE;
        case SG_LOGITEM_SHADER_POOL_EXHAUSTED: return (int)GpuResourceKind::SHADER;
        default: return -1;
        }
    }
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
    // A full pool: one clear error per pool (each failed object would log one, every frame
    // its load is tried); the failures are counted, and their loads give up until there is
    // room (SystemRender::hasResourcePoolRoom)
    const int pool = poolOfLogItem(log_item_id);
    if (pool >= 0){
        poolExhaustions[pool]++;
        poolsFull |= (1u << pool);
        if (!poolExhaustionLogged[pool].exchange(true)){
            Log::error("GPU %s pool full: all %d %s are in use. Objects that need one are not made "
                       "(meshes are not drawn) until some are released; raise Engine::GpuResourceLimits::%s "
                       "(Engine::setGpuResourceLimits before the engine starts) for more\n",
                       poolNames[pool], poolSizes[pool], poolPlurals[pool], poolPlurals[pool]);
        }
        return;
    }
    // release builds of sokol carry no message: name the log item and its line instead
    char unnamed[160];
    if (!message_or_null){
        if (log_item_id < sizeof(sokolLogItems) / sizeof(sokolLogItems[0])){
            snprintf(unnamed, sizeof(unnamed), "sokol %s %s (line %u)", tag ? tag : "", sokolLogItems[log_item_id], line_nr);
        }else{
            snprintf(unnamed, sizeof(unnamed), "sokol %s log item %u (line %u)", tag ? tag : "", log_item_id, line_nr);
        }
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
    // the pool sizes (sokol's defaults: 128 buffers and images, 64 samplers, 256 views, 64
    // pipelines and shaders): Engine::GpuResourceLimits says what they hold and cost
    const Engine::GpuResourceLimits& limits = Engine::getGpuResourceLimits();
    sg_desc desc = {0};
    desc.buffer_pool_size = limits.buffers;
    desc.image_pool_size = limits.images;
    desc.sampler_pool_size = limits.samplers;
    desc.view_pool_size = limits.views; // texture, storage-buffer and attachment views
    desc.pipeline_pool_size = limits.pipelines;
    desc.shader_pool_size = limits.shaders; // one per ShaderType+properties variant
    desc.uniform_buffer_size = limits.uniformBytes; // per frame (Vulkan, Metal, WebGPU): reserveUniformBytes
    desc.environment = System::instance().getSokolEnvironment();
    desc.logger.func = sokol_log;

    const int sizes[GPU_RESOURCE_KINDS] = {limits.buffers, limits.images, limits.samplers, limits.views, limits.pipelines, limits.shaders};
    for (int kind = 0; kind < GPU_RESOURCE_KINDS; kind++){
        poolSizes[kind] = sizes[kind];
        poolExhaustions[kind] = 0;
        poolExhaustionLogged[kind] = false;
    }
    poolsFull = 0;
    uniformLimit = (uint64_t)limits.uniformBytes;
    uniformFrameBytes = 0;
    uniformFrameNeeded = 0;
    uniformLastFrame = 0;
    uniformPeak = 0;
    uniformFrameSkipped = 0;
    uniformSkippedDraws = 0;
    uniformOverflowLogged = false;

    sg_setup(&desc);

    // diagnostics: the passes, draws and resources of each frame (getFrameStatsLine)
    const char* frameStats = std::getenv("JASPERENGINE_FRAME_STATS");
    if (frameStats && frameStats[0] != '\0' && frameStats[0] != '0'){
        sg_enable_stats();
    }

    SokolCmdQueue::start();
}

int SokolSystem::getResourcesInUse(int kind){
    if (kind < 0 || kind >= GPU_RESOURCE_KINDS || !sg_isvalid())
        return 0;
    int used = 0;
    int free = 0;
    jasperengine_sg_pool_usage(kind, &used, &free);
    return used;
}

int SokolSystem::getResourceLimit(int kind){
    return (kind >= 0 && kind < GPU_RESOURCE_KINDS) ? poolSizes[kind] : 0;
}

const char* SokolSystem::getResourceKindName(int kind){
    return (kind >= 0 && kind < GPU_RESOURCE_KINDS) ? poolNames[kind] : "";
}

uint64_t SokolSystem::getResourcePoolExhaustions(int kind){
    return (kind >= 0 && kind < GPU_RESOURCE_KINDS) ? poolExhaustions[kind].load() : 0;
}

bool SokolSystem::hasResourcePoolRoom(){
    const uint32_t full = poolsFull.load();
    if (full == 0)
        return true;
    if (!sg_isvalid())
        return false;
    for (int kind = 0; kind < GPU_RESOURCE_KINDS; kind++){
        if (!(full & (1u << kind)))
            continue;
        int used = 0;
        int free = 0;
        jasperengine_sg_pool_usage(kind, &used, &free);
        if (free == 0)
            return false;
    }
    poolsFull &= ~full;
    return true;
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

namespace{
    bool passOpen = false;
    uint64_t nestedPasses = 0;

    struct PendingInitialization{
        sg_attachments attachments;
        bool loadDepthStencil;
    };
    std::vector<PendingInitialization> pendingInitializations;

    bool attachmentsAlive(const sg_attachments& attachments){
        bool any = false;
        const auto alive = [&any](sg_view view){
            if (view.id == SG_INVALID_ID)
                return true;
            any = true;
            return sg_query_view_state(view) == SG_RESOURCESTATE_VALID;
        };
        for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++){
            if (!alive(attachments.colors[i]) || !alive(attachments.resolves[i]))
                return false;
        }
        return alive(attachments.depth_stencil) && any;
    }

    void runInitialization(const sg_attachments& attachments, bool loadDepthStencil){
        sg_pass pass = {};
        pass.attachments = attachments;
        // the colours are cleared (sokol's default action) and kept
        for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++)
            pass.action.colors[i].store_action = SG_STOREACTION_STORE;
        // a depth-stencil borrowed from another framebuffer (FramebufferDesc::shareDepthStencil)
        // was initialised by its owner and may hold that one's content (an HTML UI clip mask)
        const sg_load_action depthLoad = loadDepthStencil ? SG_LOADACTION_LOAD : SG_LOADACTION_CLEAR;
        pass.action.depth.load_action = depthLoad;
        pass.action.depth.store_action = SG_STOREACTION_STORE;
        pass.action.stencil.load_action = depthLoad;
        pass.action.stencil.store_action = SG_STOREACTION_STORE;
        SokolSystem::beginPass(pass);
        SokolSystem::endPass();
    }
}

void SokolSystem::beginPass(const sg_pass& pass){
    if (passOpen){
        nestedPasses++;
        Log::error("Render pass '%s' begun inside another: passes do not nest (an engine bug)", pass.label ? pass.label : "pass");
    }
    sg_begin_pass(pass);
    passOpen = true;
    // a new pass has no pipeline applied (the dynamic path's "applied last")
    SokolPipelineCache::invalidateApplied();
}

void SokolSystem::endPass(){
    sg_end_pass();
    passOpen = false;
    if (pendingInitializations.empty())
        return;
    // the targets created during that pass; each one's pass ends here again (none is pending then)
    std::vector<PendingInitialization> pending;
    pending.swap(pendingInitializations);
    for (const PendingInitialization& initialization : pending){
        if (attachmentsAlive(initialization.attachments))
            runInitialization(initialization.attachments, initialization.loadDepthStencil);
    }
}

uint64_t SokolSystem::getNestedPassCount(){
    return nestedPasses;
}

void SokolSystem::initializeAttachments(const sg_attachments& attachments, bool loadDepthStencil){
    if (passOpen){
        pendingInitializations.push_back({attachments, loadDepthStencil});
        return;
    }
    runInitialization(attachments, loadDepthStencil);
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

    // the frame's uniform blocks: the first frame needing more than the buffer is logged
    if (uniformFrameNeeded > uniformLimit && !uniformOverflowLogged){
        uniformOverflowLogged = true;
        if (uniformBufferLimited){
            Log::error("GPU uniform buffer full: a frame needed %llu bytes of uniform blocks, the per-frame buffer holds %llu; "
                       "%llu draws past it were skipped (this frame; later ones are counted, not logged). Raise "
                       "Engine::GpuResourceLimits::uniformBytes (Engine::setGpuResourceLimits before the engine starts)\n",
                       (unsigned long long)uniformFrameNeeded, (unsigned long long)uniformLimit, (unsigned long long)uniformFrameSkipped);
        }else{
            Log::warn("A frame needed %llu bytes of uniform blocks, more than Engine::GpuResourceLimits::uniformBytes (%llu): "
                      "drawn here (this backend has no per-frame uniform buffer), dropped on Vulkan, Metal and WebGPU\n",
                      (unsigned long long)uniformFrameNeeded, (unsigned long long)uniformLimit);
        }
    }
    uniformLastFrame = uniformFrameNeeded;
    uniformPeak = std::max(uniformPeak, uniformFrameNeeded);
    uniformFrameBytes = 0;
    uniformFrameNeeded = 0;
    uniformFrameSkipped = 0;
}

bool SokolSystem::reserveUniformBytes(unsigned int size){
    // sokol rounds every block up to the worst-case alignment (256 bytes) of these backends
    const uint64_t rounded = ((uint64_t)size + 255u) & ~(uint64_t)255u;
    uniformFrameNeeded += rounded;
    if (uniformBufferLimited && uniformFrameBytes + rounded > uniformLimit)
        return false;
    uniformFrameBytes += rounded;
    return true;
}

void SokolSystem::countSkippedDraw(){
    uniformFrameSkipped++;
    uniformSkippedDraws++;
}

uint64_t SokolSystem::getUniformBytesLastFrame(){
    return uniformLastFrame;
}

uint64_t SokolSystem::getUniformBytesPeak(){
    return uniformPeak;
}

uint64_t SokolSystem::getUniformBytesLimit(){
    return uniformLimit;
}

bool SokolSystem::hasUniformBufferLimit(){
    return uniformBufferLimited;
}

uint64_t SokolSystem::getUniformSkippedDraws(){
    return uniformSkippedDraws;
}

void SokolSystem::shutdown(){
    SokolCmdQueue::flush_commands();
    SokolCmdQueue::wait_for_flush();
    SokolCmdQueue::finish();
    SokolPipelineCache::clear();
    pendingInitializations.clear();
    passOpen = false;
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
