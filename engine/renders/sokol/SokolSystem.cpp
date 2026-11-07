// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolSystem.h"
#include "SokolAsyncCreate.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "System.h"
#include "Engine.h"
#include "sokol_gfx.h"
#include "sokol_gpu_timer.h"
#include "SokolCmdQueue.h"
#include "SokolObject.h"
#include "SokolPassPreview.h"
#include "SokolPassRecorder.h"
#include "SokolPipelineCache.h"
#include "SokolTraceHub.h"
#include "render/SystemRender.h"
#include "SokolTexture.h"
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

// release builds of sokol carry no message, the log item's name still says what failed
#define _SG_LOGITEM_XMACRO(item, msg) #item,
static const char* sokolLogItems[] = { _SG_LOG_ITEMS };
#undef _SG_LOGITEM_XMACRO

// (sokol.cpp, where sokol's state is visible) slots of a resource pool in use and free
extern "C" void jasperengine_sg_pool_usage(int kind, int* used, int* free);
extern "C" void jasperengine_sg_wait_gpu_idle(void);
// (sokol.cpp) Vulkan's per-frame buffers: 0 stream staging, 1 uniforms, 2 descriptors
extern "C" bool jasperengine_sg_vk_frame_buffer_usage(int which, uint32_t* used, uint32_t* size, bool* overflown);
extern "C" bool jasperengine_sg_vk_next_draw_descriptors(uint32_t limit, bool bindings, uint32_t* need, uint32_t* used, uint32_t* size);
// (sokol.cpp) the draw being set up is still valid (a failed sg_apply_bindings or uniform block turns
// it off until the next sg_apply_pipeline)
extern "C" bool jasperengine_sg_next_draw_valid(void);
// (sokol.cpp) Vulkan: an upload past the stream staging buffer, through the copy staging path
extern "C" bool jasperengine_sg_vk_update_buffer_alone(sg_buffer buf_id, const void* data, size_t size);
extern "C" bool jasperengine_sg_vk_update_image_alone(sg_image img_id, const sg_image_data* data);

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

    uint64_t roundUp(uint64_t value, uint64_t align){
        return align > 1 ? (value + align - 1) / align * align : value;
    }

    // A per-frame GPU buffer of sokol (Engine::GpuResourceLimits): what the frame put in it
    // (rounded as sokol places items), what it asked for (the refused items too), the items
    // skipped since setup, and the first frame that needed more, logged once. Draws and uploads
    // happen on the thread that commits the frame.
    struct FrameBudget{
        const char* name;      // in the log: "uniform buffer"
        const char* contents;  // "uniform blocks"
        const char* items;     // what is skipped past it: "draws"
        const char* setting;   // the GpuResourceLimits field
        bool backendBuffer = false;  // this backend has the buffer (it is enforced)
        uint64_t limit = 0;
        bool enforced = false;
        uint64_t frameUsed = 0;
        uint64_t frameNeeded = 0;
        uint64_t frameSkipped = 0;
        uint64_t lastFrame = 0;
        uint64_t peak = 0;
        uint64_t skipped = 0;
        bool overflowLogged = false;
        // An item larger than the whole limit (reserveAlone) never fits beside others: it is made
        // alone, in a frame that took nothing before it, and fills that frame's budget (what comes
        // after it waits for the next frame). One that finds the frame begun waits for the next,
        // which keeps the budget for it (aloneWaiting). Before S55 R6 it was refused forever.
        uint64_t frameAlone = 0;     // the bytes of the item made alone this frame
        uint64_t frameDelayed = 0;   // the bytes of the items turned away for a later frame: not lost
        bool aloneWaiting = false;   // one was turned away: the frame keeps the budget for it
        bool aloneAsked = false;     // one asked this frame
        bool aloneLogged = false;

        FrameBudget(const char* name, const char* contents, const char* items, const char* setting):
            name(name), contents(contents), items(items), setting(setting){}

        void setup(uint64_t bytes, bool hasBuffer){
            backendBuffer = hasBuffer;
            limit = bytes;
            enforced = hasBuffer;
            frameUsed = frameNeeded = frameSkipped = lastFrame = peak = skipped = 0;
            overflowLogged = false;
            frameAlone = frameDelayed = 0;
            aloneWaiting = aloneAsked = aloneLogged = false;
        }

        // an item placed at the end of what the frame took, the end then rounded up to `align`
        // (sokol's shared buffers); false where enforced and it goes past the limit (not taken),
        // or while the frame keeps the budget for an item larger than the limit (reserveAlone)
        bool reserve(uint64_t size, uint64_t align){
            frameNeeded = roundUp(frameNeeded + size, align);
            if (enforced && (frameUsed + size > limit || aloneWaiting || frameAlone > 0))
                return false;
            frameUsed = roundUp(frameUsed + size, align);
            return true;
        }

        // an item larger than the limit: true when it is made now, alone (the frame took nothing
        // before it); it then fills the frame's budget
        bool reserveAlone(uint64_t size){
            aloneAsked = true;
            if (frameUsed > 0 || frameAlone > 0){
                aloneWaiting = true;
                frameDelayed = roundUp(frameDelayed + size, 16);
                return false;
            }
            aloneWaiting = false;
            frameAlone = size;
            frameUsed = limit;
            if (!aloneLogged){
                aloneLogged = true;
                Log::warn("An upload of %llu bytes is larger than the GPU %s (%llu bytes a frame): it is made alone in "
                          "its own frame, waiting for the GPU (slower). Raise Engine::GpuResourceLimits::%s "
                          "(Engine::setGpuResourceLimits before the engine starts)\n",
                          (unsigned long long)size, name, (unsigned long long)limit, setting);
            }
            return true;
        }

        void skip(){
            frameSkipped++;
            skipped++;
        }

        // a frame committed: the first one that needed more than the limit is logged
        void endFrame(){
            if (frameNeeded > limit && !overflowLogged){
                overflowLogged = true;
                if (enforced){
                    Log::error("GPU %s full: a frame needed %llu bytes of %s, the per-frame buffer holds %llu; "
                               "%llu %s past it were skipped (this frame; later ones are counted, not logged). Raise "
                               "Engine::GpuResourceLimits::%s (Engine::setGpuResourceLimits before the engine starts)\n",
                               name, (unsigned long long)frameNeeded, contents, (unsigned long long)limit,
                               (unsigned long long)frameSkipped, items, setting);
                }else{
                    Log::warn("A frame needed %llu bytes of %s, more than Engine::GpuResourceLimits::%s (%llu): "
                              "fine on this backend (it has no per-frame %s), %s past it are skipped on Vulkan\n",
                              (unsigned long long)frameNeeded, contents, setting, (unsigned long long)limit, name, items);
                }
            }
            lastFrame = frameNeeded + frameAlone + frameDelayed;
            peak = std::max(peak, lastFrame);
            frameUsed = frameNeeded = frameSkipped = frameAlone = frameDelayed = 0;
            // the item turned away is asked for again by its owner next frame; one not asked for
            // again (its data went, or became smaller) keeps nothing waiting
            if (!aloneAsked)
                aloneWaiting = false;
            aloneAsked = false;
        }
    };

    // the frame's uniform blocks (reserveUniformBytes): sg_desc.uniform_buffer_size on Vulkan,
    // Metal and WebGPU (GL sets them with glUniform, D3D11 updates a constant buffer per block)
    FrameBudget uniformBudget("uniform buffer", "uniform blocks", "draws", "uniformBytes");
    // the frame's uploads into STREAM buffers and images (reserveStreamBytes): Vulkan stages them
    // in sg_desc.vulkan.stream_staging_buffer_size; the other backends write the resource
    FrameBudget streamBudget("stream staging buffer", "STREAM buffer and texture uploads", "uploads", "streamStagingBytes");
    // the frame's descriptor sets (reserveDescriptorBytes): Vulkan's sg_desc.vulkan.descriptor_buffer_size
    FrameBudget descriptorBudget("descriptor buffer", "descriptor sets", "draws", "descriptorBytes");
    // descriptor bytes of the draws skipped this frame (what it needed on top of what it took)
    uint64_t descriptorFrameSkippedBytes = 0;

#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL) || defined(SOKOL_WGPU)
    constexpr bool uniformBufferLimited = true;
#else
    constexpr bool uniformBufferLimited = false;
#endif
#if defined(SOKOL_VULKAN)
    constexpr bool vulkanBuffers = true;
#else
    constexpr bool vulkanBuffers = false;
#endif

    FrameBudget* budgetOf(int kind){
        switch (kind){
        case SokolSystem::UNIFORM_BUDGET: return &uniformBudget;
        case SokolSystem::STREAM_BUDGET: return &streamBudget;
        case SokolSystem::DESCRIPTOR_BUDGET: return &descriptorBudget;
        default: return nullptr;
        }
    }

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
    // uniform block, image-sampler or vertex attribute. This is benign and expected for
    // custom/simplified shaders that don't use every binding their variant provides (an unlit
    // library shader on a mesh with normals and tangents: the converted particle quads), so log
    // it at debug only.
    if (log_item_id == SG_LOGITEM_GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER ||
        log_item_id == SG_LOGITEM_GL_IMAGE_SAMPLER_NAME_NOT_FOUND_IN_SHADER ||
        log_item_id == SG_LOGITEM_GL_VERTEX_ATTRIBUTE_NOT_FOUND_IN_SHADER){
        // the warning itself carries no message in release builds: the name follows it (info)
        if (message_or_null) Log::debug("%s\n", message_or_null);
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
    // a call that failed can leave sokol skipping draws until the next sg_apply_pipeline
    if (log_level <= 1 && !Engine::isAsyncThread())
        SokolSystem::forgetAppliedState();
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
    desc.vulkan.stream_staging_buffer_size = limits.streamStagingBytes; // per frame: reserveStreamBytes
    desc.vulkan.descriptor_buffer_size = limits.descriptorBytes; // per frame: reserveDescriptorBytes
    desc.wgpu.bindgroups_cache_size = limits.bindGroupsCacheSize;
    desc.environment = System::instance().getSokolEnvironment();
    desc.logger.func = sokol_log;

    const int sizes[GPU_RESOURCE_KINDS] = {limits.buffers, limits.images, limits.samplers, limits.views, limits.pipelines, limits.shaders};
    for (int kind = 0; kind < GPU_RESOURCE_KINDS; kind++){
        poolSizes[kind] = sizes[kind];
        poolExhaustions[kind] = 0;
        poolExhaustionLogged[kind] = false;
    }
    poolsFull = 0;
    uniformBudget.setup((uint64_t)limits.uniformBytes, uniformBufferLimited);
    streamBudget.setup((uint64_t)limits.streamStagingBytes, vulkanBuffers);
    descriptorBudget.setup((uint64_t)limits.descriptorBytes, vulkanBuffers);
    descriptorFrameSkippedBytes = 0;

    sg_setup(&desc);
    // a new context: nothing of an earlier one is applied
    forgetAppliedState();

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

    // shaders and pipelines created off the render thread from here on (spec S32)
    SokolAsyncCreate::setup();
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
    // what a batch holds belongs to the pass that ends (SokolDrawBatch: its scope never spans one)
    SokolDrawBatch::flush();
    sg_begin_pass(pass);
    passOpen = true;
    // a new pass has no pipeline, uniforms or bindings applied
    forgetAppliedState();
}

void SokolSystem::endPass(){
    SokolDrawBatch::flush();
    sg_end_pass();
    passOpen = false;
    forgetAppliedState();
    // the Frame Debugger's preview of the pass that ended, when it asked for it (S15; its blit is
    // a pass of its own, which does not preview itself)
    SokolPassPreview::afterEndPass();
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
    // no device (a headless frame: tests running Engine::systemUpdate without graphics): nothing
    // to commit (sokol asserts on it in debug builds)
    if (!sg_isvalid())
        return;
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    // what the frame took of Vulkan's descriptor buffer (sokol's own count: every draw's sets),
    // read before sg_commit submits it
    uint32_t descriptorUsed = 0, descriptorSize = 0;
    bool descriptorOverflown = false;
    if (jasperengine_sg_vk_frame_buffer_usage(2, &descriptorUsed, &descriptorSize, &descriptorOverflown)){
        descriptorBudget.frameUsed = descriptorUsed;
        descriptorBudget.frameNeeded = (uint64_t)descriptorUsed + descriptorFrameSkippedBytes;
    }
    descriptorFrameSkippedBytes = 0;

    sg_commit();
    // the GPU times of earlier commits that resolved meanwhile (never waits)
    sgj_commit_times times;
    while (sgj_gpu_timer_pop(&times)){
        SokolPassRecorder::onGpuTimes(times);
    }
    if (sgj_gpu_timer_enabled() && !gpuTimingWanted())
        sgj_gpu_timer_set_enabled(false);

    uniformBudget.endFrame();
    streamBudget.endFrame();
    descriptorBudget.endFrame();
    SokolDrawBatch::endFrame();
}

bool SokolSystem::reserveUniformBytes(unsigned int size){
    // sokol rounds every block up to the worst-case alignment (256 bytes) of these backends
    return uniformBudget.reserve(roundUp(size, 256), 256);
}

void SokolSystem::countSkippedDraw(){
    uniformBudget.skip();
}

bool SokolSystem::reserveStreamBytes(const uint64_t* sizes, int count){
    // all or nothing (the mip levels of an image): sokol's staging buffer takes each at the next
    // 16-byte boundary, and an upload it cannot take whole is dropped
    FrameBudget saved = streamBudget;
    for (int i = 0; i < count; i++){
        if (sizes[i] == 0)
            continue;
        if (!streamBudget.reserve(sizes[i], 16)){
            const uint64_t needed = streamBudget.frameNeeded;
            streamBudget = saved;
            // what the frame asked for counts the refused upload whole
            streamBudget.frameNeeded = needed;
            for (int j = i + 1; j < count; j++)
                streamBudget.frameNeeded = roundUp(streamBudget.frameNeeded + sizes[j], 16);
            streamBudget.skip();
            return false;
        }
    }
    return true;
}

namespace{
    bool streamUpdated(sg_buffer buffer){
        return sg_query_buffer_state(buffer) == SG_RESOURCESTATE_VALID && sg_query_buffer_usage(buffer).stream_update;
    }
}

// an upload larger than the whole stream budget (it can never fit beside others): alone in its own
// frame (FrameBudget::reserveAlone); Vulkan makes it through the copy staging path, which it fits in
// chunks, the other backends write the resource as always
bool SokolSystem::updateBuffer(sg_buffer buffer, const sg_range& data){
    // (a held draw may read it: drawn before)
    SokolDrawBatch::flush();
    // sokol may write another of the buffer's backend buffers (dynamic and stream ones rotate):
    // the same handle in a draw's bindings names other memory
    forgetAppliedBindings();
    if (streamUpdated(buffer)){
        const uint64_t size = data.size;
        if (streamBudget.enforced && size > streamBudget.limit){
            if (!streamBudget.reserveAlone(size)){
                streamBudget.skip();
                return false;
            }
            if (vulkanBuffers)
                return jasperengine_sg_vk_update_buffer_alone(buffer, data.ptr, data.size);
        }else if (!reserveStreamBytes(&size, 1)){
            return false;
        }
    }
    sg_update_buffer(buffer, data);
    return true;
}

int SokolSystem::appendBuffer(sg_buffer buffer, const sg_range& data){
    // (as updateBuffer)
    forgetAppliedBindings();
    if (streamUpdated(buffer)){
        const uint64_t size = data.size;
        if (!reserveStreamBytes(&size, 1))
            return -1;
    }
    return sg_append_buffer(buffer, data);
}

bool SokolSystem::updateImage(sg_image image, const sg_image_data& data){
    SokolDrawBatch::flush();
    // (as updateBuffer)
    forgetAppliedBindings();
    if (sg_query_image_state(image) == SG_RESOURCESTATE_VALID && sg_query_image_usage(image).stream_update){
        // sokol stages the image's mip levels (each slice of one together)
        const int mips = std::min(std::max(sg_query_image_num_mipmaps(image), 1), (int)SG_MAX_MIPMAPS);
        uint64_t sizes[SG_MAX_MIPMAPS] = {};
        uint64_t total = 0;
        for (int mip = 0; mip < mips; mip++){
            sizes[mip] = data.mip_levels[mip].size;
            total = roundUp(total + sizes[mip], 16);
        }
        // (as updateBuffer: larger than the whole budget, alone in its own frame)
        if (streamBudget.enforced && total > streamBudget.limit){
            if (!streamBudget.reserveAlone(total)){
                streamBudget.skip();
                return false;
            }
            if (vulkanBuffers)
                return jasperengine_sg_vk_update_image_alone(image, &data);
        }else if (!reserveStreamBytes(sizes, mips)){
            return false;
        }
    }
    sg_update_image(image, data);
    return true;
}

bool SokolSystem::reserveDescriptorBytes(bool bindings){
    if (!descriptorBudget.enforced)
        return true;
    uint32_t need = 0, used = 0, size = 0;
    const uint32_t limit = (uint32_t)std::min<uint64_t>(descriptorBudget.limit, UINT32_MAX);
    if (jasperengine_sg_vk_next_draw_descriptors(limit, bindings, &need, &used, &size))
        return true;
    descriptorFrameSkippedBytes += need;
    descriptorBudget.skip();
    return false;
}

namespace {
    // what the current pass applied last (SokolSystem::applyPipeline, applyBindings)
    struct AppliedDrawState {
        uint32_t pipeline = SG_INVALID_ID;
        uint32_t epoch = 1;
        bool bindingsApplied = false;
        sg_bindings bindings = {};
    };

    AppliedDrawState& appliedDrawState(){
        static AppliedDrawState state;
        return state;
    }
}

bool SokolSystem::applyPipeline(uint32_t pipelineId){
    AppliedDrawState& state = appliedDrawState();
    if (pipelineId == state.pipeline)
        return false;
    // recorded before the call: an error inside it forgets them again (sokol_log)
    state.pipeline = pipelineId;
    state.epoch++;
    // sokol wants the bindings again after every sg_apply_pipeline
    state.bindingsApplied = false;
    sg_apply_pipeline({pipelineId});
    Engine::addSetPassCall();
    return true;
}

bool SokolSystem::isBindingsApplied(const sg_bindings& bindings){
    const AppliedDrawState& state = appliedDrawState();
    return state.bindingsApplied && std::memcmp(&state.bindings, &bindings, sizeof(sg_bindings)) == 0;
}

void SokolSystem::applyBindings(const sg_bindings& bindings){
    AppliedDrawState& state = appliedDrawState();
    state.bindings = bindings;
    state.bindingsApplied = true;
    sg_apply_bindings(&bindings);
    // a binding that is not ready (a buffer still being created) turns the pipeline's later draws
    // off in sokol until it is applied again: the next draw applies both
    if (!jasperengine_sg_next_draw_valid())
        forgetAppliedState();
}

void SokolSystem::draw(int baseElement, int numElements, int numInstances){
    sg_draw(baseElement, numElements, numInstances);
    if (!jasperengine_sg_next_draw_valid())
        forgetAppliedState();
}

void SokolSystem::forgetAppliedState(){
    AppliedDrawState& state = appliedDrawState();
    state.pipeline = SG_INVALID_ID;
    state.epoch++;
    state.bindingsApplied = false;
}

void SokolSystem::forgetAppliedBindings(){
    appliedDrawState().bindingsApplied = false;
}

uint32_t SokolSystem::appliedEpoch(){
    return appliedDrawState().epoch;
}

uint32_t SokolSystem::appliedPipeline(){
    return appliedDrawState().pipeline;
}

uint64_t SokolSystem::getBudgetLastFrame(int kind){
    const FrameBudget* budget = budgetOf(kind);
    return budget ? budget->lastFrame : 0;
}

uint64_t SokolSystem::getBudgetPeak(int kind){
    const FrameBudget* budget = budgetOf(kind);
    return budget ? budget->peak : 0;
}

uint64_t SokolSystem::getBudgetLimit(int kind){
    const FrameBudget* budget = budgetOf(kind);
    return budget ? budget->limit : 0;
}

bool SokolSystem::isBudgetEnforced(int kind){
    const FrameBudget* budget = budgetOf(kind);
    return budget && budget->enforced;
}

uint64_t SokolSystem::getBudgetSkipped(int kind){
    const FrameBudget* budget = budgetOf(kind);
    return budget ? budget->skipped : 0;
}

void SokolSystem::overrideBudget(int kind, uint64_t limit, bool enforced){
    FrameBudget* budget = budgetOf(kind);
    if (!budget)
        return;
    budget->limit = limit;
    budget->enforced = enforced;
    budget->overflowLogged = false;
    budget->aloneWaiting = false;
}

void SokolSystem::restoreBudgets(){
    const Engine::GpuResourceLimits& limits = Engine::getGpuResourceLimits();
    const uint64_t configured[3] = {(uint64_t)limits.uniformBytes, (uint64_t)limits.streamStagingBytes, (uint64_t)limits.descriptorBytes};
    for (int kind = 0; kind < 3; kind++){
        FrameBudget* budget = budgetOf(kind);
        budget->limit = configured[kind];
        budget->enforced = budget->backendBuffer;
        budget->aloneWaiting = false;
    }
}

bool SokolSystem::getVulkanFrameBufferUsage(int which, uint64_t& used, uint64_t& size){
    uint32_t u = 0, z = 0;
    bool overflown = false;
    const bool vulkan = jasperengine_sg_vk_frame_buffer_usage(which, &u, &z, &overflown);
    used = u;
    size = z;
    return vulkan;
}

void SokolSystem::waitGpuIdle(){
    jasperengine_sg_wait_gpu_idle();
}

void SokolSystem::shutdown(){
    // commands an async loader committed after the last frame (the Project window's
    // thumbnail job, an editor closing right after it opened): executed first, as the next
    // frame would have, or flush_commands waits forever for the render semaphore they hold
    SokolCmdQueue::execute_commands();
    // and what the upload budget left for later frames
    SokolCmdQueue::drain();
    SokolCmdQueue::flush_commands();
    SokolCmdQueue::wait_for_flush();
    SokolCmdQueue::finish();
    // creations end (or are cancelled) before the pipelines and shaders go
    SokolAsyncCreate::shutdown();
    SokolAsyncCreate::forgetWarmDraws();
    SokolPassPreview::destroy();
    SokolDrawBatch::shutdown();
    SokolPipelineCache::clear();
    SokolTraceHub::uninstall();
#if JASPERENGINE_PROFILER
    gpumem::setDriverQuery(nullptr);
    gpumem::clearAll();
#endif
    sgj_gpu_timer_shutdown();
    pendingInitializations.clear();
    passOpen = false;
    forgetAppliedState();
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
        features.msaaRGBA16F = rgba16f.msaa;
        features.msaaR16F = r16f.msaa;
        features.maxTextureSize2D = sg_query_limits().max_image_size_2d;
        for (size_t f = 0; f < kGpuTextureFormatCount; f++)
            features.textureFormats.set((GpuTextureFormat)f, SokolTexture::isFormatSupported((GpuTextureFormat)f));
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

std::string SokolSystem::getApiVersion(){
    return sg_isvalid() ? std::string(sgj_api_version()) : std::string();
}

void SokolSystem::setVulkanHostQueryReset(bool enabled){
    sgj_gpu_timer_set_vulkan_host_query_reset(enabled);
}
