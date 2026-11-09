// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The backends of the GPU timer extension (sokol_gpu_timer.h). Included by sokol.cpp right
// after sokol_gfx.h with SOKOL_IMPL, so sokol's state (_sg) is visible, like
// jasperengine_sg_max_msaa_samples. Every function runs on the thread that calls sokol.
//
// A ring of SGJ_RING_SLOTS commit slots holds the queries of the commits in flight. A slot opens
// at the first pass of a commit, records up to SGJ_MAX_PASSES_PER_COMMIT passes and closes in the
// pre-commit hook; every pre-commit hook then reads the slots whose results are available
// (never waiting) into a queue of resolved commits that sgj_gpu_timer_pop drains. sokol keeps two
// frames in flight (SG_NUM_INFLIGHT_FRAMES), so a slot is idle on the GPU long before the ring
// comes back to it.
//
//   GL family  one GL_TIME_ELAPSED query per pass (sokol passes never nest), polled with
//              GL_QUERY_RESULT_AVAILABLE; GLES3 and WebGL2 check GL_GPU_DISJOINT_EXT every commit.
//   D3D11      a TIMESTAMP_DISJOINT query per commit and two TIMESTAMP queries per pass, read with
//              D3D11_ASYNC_GETDATA_DONOTFLUSH.
//   Vulkan     a timestamp query pool per slot (two queries per pass), written with
//              vkCmdWriteTimestamp2 inside the dynamic rendering instance, read without
//              VK_QUERY_RESULT_WAIT_BIT and reset from the host (hostQueryReset).
//   Metal      a MTLCounterSampleBuffer per slot attached to every render pass descriptor
//              (stage-boundary samples), resolved after the command buffer completed; without
//              counter sampling only the command buffer's GPU time.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#if defined(_SOKOL_ANY_GL)
    #if defined(__EMSCRIPTEN__)
        #include <emscripten/html5.h>
    #elif defined(__ANDROID__)
        #include <EGL/egl.h>
    #endif
    #ifndef GL_TIME_ELAPSED
        #define GL_TIME_ELAPSED 0x88BF
    #endif
    #ifndef GL_QUERY_COUNTER_BITS
        #define GL_QUERY_COUNTER_BITS 0x8864
    #endif
    #ifndef GL_QUERY_RESULT
        #define GL_QUERY_RESULT 0x8866
    #endif
    #ifndef GL_QUERY_RESULT_AVAILABLE
        #define GL_QUERY_RESULT_AVAILABLE 0x8867
    #endif
    #ifndef GL_GPU_DISJOINT_EXT
        #define GL_GPU_DISJOINT_EXT 0x8FBB
    #endif
#endif

#if defined(SOKOL_D3D11)
    #include <dxgi1_4.h>
#endif

extern "C" {
_sg_jasper_hooks_t _sg_jasper_hooks = {};
}

#define SGJ_RING_SLOTS 8
#define SGJ_LABEL_LEN 48
// resolved commits waiting for sgj_gpu_timer_pop (older ones are dropped as overflow)
#define SGJ_RESOLVED_SLOTS 16
// a commit not resolved after this many later commits is dropped (overflow)
#define SGJ_MAX_AGE (SGJ_RING_SLOTS - 1)

namespace {

#if defined(SOKOL_METAL)
// written by the command buffer's completion handler (another thread); shared with the block,
// so a handler that runs after shutdown writes into memory it still owns
struct sgj_mtl_completion {
    std::atomic<bool> done{false};
    double gpu_start = 0.0;
    double gpu_end = 0.0;
};
// samples per pass: start/end of the vertex and of the fragment stage
#define SGJ_MTL_SAMPLES_PER_PASS 4
#endif

struct sgj_slot {
    bool in_flight = false;
    uint64_t commit_index = 0;
    uint32_t view_tag = 0;
    uint32_t num_passes = 0;
    uint32_t dropped_passes = 0;
    uint32_t age = 0;
    char labels[SGJ_MAX_PASSES_PER_COMMIT][SGJ_LABEL_LEN];
    bool pass_issued[SGJ_MAX_PASSES_PER_COMMIT];   // its queries were issued (false: invalid pass)
    #if defined(_SOKOL_ANY_GL)
    GLuint queries[SGJ_MAX_PASSES_PER_COMMIT];
    #elif defined(SOKOL_D3D11)
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* stamps[2 * SGJ_MAX_PASSES_PER_COMMIT];
    #elif defined(SOKOL_VULKAN)
    VkQueryPool pool = VK_NULL_HANDLE;
    #elif defined(SOKOL_METAL)
    id samples = nil;   // id<MTLCounterSampleBuffer> (macOS 11, iOS 14)
    std::shared_ptr<sgj_mtl_completion> completion;
    #endif
};

struct sgj_resolved {
    sgj_commit_times info;
    sgj_pass_time passes[SGJ_MAX_PASSES_PER_COMMIT];
    char labels[SGJ_MAX_PASSES_PER_COMMIT][SGJ_LABEL_LEN];
};

struct sgj_state_t {
    bool setup = false;
    bool enabled = false;
    sgj_timer_state state = SGJ_TIMER_UNAVAILABLE;
    sgj_timer_reason reason = SGJ_REASON_BACKEND;
    sgj_timer_semantics semantics = SGJ_SEM_PIPELINE_POINT;
    bool frame_is_sum = false;
    sgj_timer_reason pass_reason = SGJ_REASON_NONE;   // passes have no time though frames do
    bool vk_host_query_reset = false;
    uint32_t view_tag = 0xFFFFFFFFu;

    sgj_slot* slots = nullptr;   // SGJ_RING_SLOTS, made at the first enable
    int next_slot = 0;
    int cur_slot = -1;           // the slot of the commit being recorded
    bool pass_open = false;      // the current pass issued its begin query
    int reserved_pass = -1;      // Metal: the index the render-pass hook gave the next pass

    sgj_resolved* resolved = nullptr;   // SGJ_RESOLVED_SLOTS ring
    int resolved_head = 0;
    int resolved_count = 0;
    sgj_resolved popped;

    sgj_gpu_timer_stats stats = {};

    #if defined(_SOKOL_ANY_GL)
    bool gl_disjoint = false;    // GLES3/WebGL2: GL_GPU_DISJOINT_EXT exists
    #if defined(__ANDROID__)
    void (GL_APIENTRY* gl_get_query_ui64)(GLuint id, GLenum pname, uint64_t* params) = nullptr;
    #endif
    #elif defined(SOKOL_D3D11)
    #elif defined(SOKOL_VULKAN)
    double vk_ns_per_tick = 1.0;
    uint64_t vk_valid_mask = ~0ull;
    #elif defined(SOKOL_METAL)
    id counter_set = nil;        // id<MTLCounterSet>: the timestamp set
    bool mtl_counters = false;
    // GPU ticks to ns: calibrated against the CPU clock (sampleTimestamps:gpuTimestamp:, whose
    // CPU time is in nanoseconds already, not in mach_absolute_time ticks: scaling it by the
    // mach timebase made every pass 41.67x too long on Apple silicon, GpuTimerGpu)
    uint64_t mtl_cpu0 = 0, mtl_gpu0 = 0;
    double mtl_ns_per_gpu_tick = 1.0;
    #endif
};

sgj_state_t sgj;

void sgj_install_hooks(bool on);

// ---- backend: capability

void sgj_backend_detect(void) {
    #if defined(_SOKOL_ANY_GL)
        sgj.semantics = SGJ_SEM_ELAPSED;
        sgj.frame_is_sum = true;
        #if defined(__EMSCRIPTEN__)
            // WebGL2: EXT_disjoint_timer_query_webgl2 (Chrome; Firefox and Safari do not have it)
            const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_get_current_context();
            if (!context || !emscripten_webgl_enable_extension(context, "EXT_disjoint_timer_query_webgl2")) {
                sgj.reason = SGJ_REASON_EXTENSION_MISSING;
                return;
            }
            sgj.gl_disjoint = true;
        #elif defined(SOKOL_GLES3)
            bool found = false;
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count && !found; i++) {
                const char* name = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                found = name && (std::strcmp(name, "GL_EXT_disjoint_timer_query") == 0);
            }
            if (!found) {
                sgj.reason = SGJ_REASON_EXTENSION_MISSING;
                return;
            }
            sgj.gl_disjoint = true;
            #if defined(__ANDROID__)
                // the 64-bit result entry point of the extension (the 32-bit one is core GLES3)
                sgj.gl_get_query_ui64 = (void (GL_APIENTRY*)(GLuint, GLenum, uint64_t*))eglGetProcAddress("glGetQueryObjectui64vEXT");
            #endif
        #endif
        #if !defined(__EMSCRIPTEN__)
            // a driver may expose the query with no bits behind it
            GLint bits = 0;
            glGetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &bits);
            while (glGetError() != GL_NO_ERROR) {}
            if (bits <= 0) {
                sgj.reason = SGJ_REASON_DEVICE_UNSUPPORTED;
                return;
            }
        #endif
        sgj.state = SGJ_TIMER_AVAILABLE;
        sgj.reason = SGJ_REASON_NONE;
    #elif defined(SOKOL_D3D11)
        sgj.semantics = SGJ_SEM_PIPELINE_POINT;
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        ID3D11Query* probe = nullptr;
        if (FAILED(_sg.d3d11.dev->CreateQuery(&desc, &probe)) || !probe) {
            sgj.reason = SGJ_REASON_DEVICE_UNSUPPORTED;
            return;
        }
        probe->Release();
        sgj.state = SGJ_TIMER_AVAILABLE;
        sgj.reason = SGJ_REASON_NONE;
    #elif defined(SOKOL_VULKAN)
        sgj.semantics = SGJ_SEM_PIPELINE_POINT;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(_sg.vk.phys_dev, &count, nullptr);
        uint32_t valid_bits = 0;
        if (_sg.vk.queue_family_index < count) {
            VkQueueFamilyProperties* families = (VkQueueFamilyProperties*)calloc(count, sizeof(VkQueueFamilyProperties));
            vkGetPhysicalDeviceQueueFamilyProperties(_sg.vk.phys_dev, &count, families);
            valid_bits = families[_sg.vk.queue_family_index].timestampValidBits;
            free(families);
        }
        const float period = _sg.vk.dev_props.properties.limits.timestampPeriod;
        if (valid_bits == 0 || period <= 0.0f || !sgj.vk_host_query_reset) {
            sgj.reason = SGJ_REASON_DEVICE_UNSUPPORTED;
            return;
        }
        sgj.vk_valid_mask = (valid_bits >= 64) ? ~0ull : ((1ull << valid_bits) - 1ull);
        sgj.vk_ns_per_tick = (double)period;
        sgj.state = SGJ_TIMER_AVAILABLE;
        sgj.reason = SGJ_REASON_NONE;
    #elif defined(SOKOL_METAL)
        sgj.semantics = SGJ_SEM_ENCODER_SPAN;
        // the command buffer's GPU time is always there: passes need counter sampling
        sgj.state = SGJ_TIMER_AVAILABLE;
        sgj.reason = SGJ_REASON_NONE;
        sgj.pass_reason = SGJ_REASON_DEVICE_UNSUPPORTED;
        if (@available(macOS 11.0, iOS 14.0, *)) {
            id<MTLDevice> device = _sg.mtl.device;
            if ([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
                for (id<MTLCounterSet> set in device.counterSets) {
                    if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
                        sgj.counter_set = set;
                        break;
                    }
                }
            }
            if (sgj.counter_set) {
                MTLTimestamp cpu = 0, gpu = 0;
                [device sampleTimestamps:&cpu gpuTimestamp:&gpu];
                sgj.mtl_cpu0 = cpu;
                sgj.mtl_gpu0 = gpu;
                sgj.mtl_counters = true;
                sgj.pass_reason = SGJ_REASON_NONE;
            }
        }
    #else
        // WebGPU and the dummy backend
        sgj.reason = SGJ_REASON_BACKEND;
    #endif
}

// ---- backend: per-slot objects (made at the first enable, kept until shutdown: a pool may still
// be read by the GPU when the timer is switched off)

bool sgj_backend_make_slot(sgj_slot& slot) {
    #if defined(_SOKOL_ANY_GL)
        glGenQueries(SGJ_MAX_PASSES_PER_COMMIT, slot.queries);
        return glGetError() == GL_NO_ERROR;
    #elif defined(SOKOL_D3D11)
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(_sg.d3d11.dev->CreateQuery(&desc, &slot.disjoint)))
            return false;
        desc.Query = D3D11_QUERY_TIMESTAMP;
        for (int i = 0; i < 2 * SGJ_MAX_PASSES_PER_COMMIT; i++) {
            slot.stamps[i] = nullptr;
            if (FAILED(_sg.d3d11.dev->CreateQuery(&desc, &slot.stamps[i])))
                return false;
        }
        return true;
    #elif defined(SOKOL_VULKAN)
        VkQueryPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2 * SGJ_MAX_PASSES_PER_COMMIT;
        if (vkCreateQueryPool(_sg.vk.dev, &info, nullptr, &slot.pool) != VK_SUCCESS)
            return false;
        vkResetQueryPool(_sg.vk.dev, slot.pool, 0, 2 * SGJ_MAX_PASSES_PER_COMMIT);
        return true;
    #elif defined(SOKOL_METAL)
        if (!sgj.mtl_counters)
            return true;
        if (@available(macOS 11.0, iOS 14.0, *)) {
            MTLCounterSampleBufferDescriptor* desc = [[MTLCounterSampleBufferDescriptor alloc] init];
            desc.counterSet = (id<MTLCounterSet>)sgj.counter_set;
            desc.storageMode = MTLStorageModeShared;
            desc.sampleCount = SGJ_MTL_SAMPLES_PER_PASS * SGJ_MAX_PASSES_PER_COMMIT;
            NSError* error = nil;
            slot.samples = [_sg.mtl.device newCounterSampleBufferWithDescriptor:desc error:&error];
            return slot.samples != nil;
        }
        return false;
    #else
        (void)slot;
        return false;
    #endif
}

void sgj_backend_destroy_slot(sgj_slot& slot) {
    #if defined(_SOKOL_ANY_GL)
        glDeleteQueries(SGJ_MAX_PASSES_PER_COMMIT, slot.queries);
    #elif defined(SOKOL_D3D11)
        if (slot.disjoint) { slot.disjoint->Release(); slot.disjoint = nullptr; }
        for (int i = 0; i < 2 * SGJ_MAX_PASSES_PER_COMMIT; i++) {
            if (slot.stamps[i]) { slot.stamps[i]->Release(); slot.stamps[i] = nullptr; }
        }
    #elif defined(SOKOL_VULKAN)
        if (slot.pool) {
            vkDestroyQueryPool(_sg.vk.dev, slot.pool, nullptr);
            slot.pool = VK_NULL_HANDLE;
        }
    #elif defined(SOKOL_METAL)
        slot.samples = nil;
        slot.completion.reset();
    #else
        (void)slot;
    #endif
}

// ---- backend: recording

void sgj_backend_open_slot(sgj_slot& slot) {
    #if defined(SOKOL_D3D11)
        _sg.d3d11.ctx->Begin(slot.disjoint);
    #elif defined(SOKOL_VULKAN)
        // the ring came back to this slot: its last commit completed on the GPU long ago
        vkResetQueryPool(_sg.vk.dev, slot.pool, 0, 2 * SGJ_MAX_PASSES_PER_COMMIT);
    #else
        (void)slot;
    #endif
}

// false: no query could be issued for this pass (it is reported invalid)
bool sgj_backend_begin_pass(sgj_slot& slot, uint32_t index) {
    #if defined(_SOKOL_ANY_GL)
        glBeginQuery(GL_TIME_ELAPSED, slot.queries[index]);
        sgj.stats.queries_issued++;
        return true;
    #elif defined(SOKOL_D3D11)
        _sg.d3d11.ctx->End(slot.stamps[2 * index]);
        sgj.stats.queries_issued++;
        return true;
    #elif defined(SOKOL_VULKAN)
        if (!_sg.vk.frame.cmd_buf)
            return false;
        vkCmdWriteTimestamp2(_sg.vk.frame.cmd_buf, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, slot.pool, 2 * index);
        sgj.stats.queries_issued++;
        return true;
    #elif defined(SOKOL_METAL)
        // the samples were attached to the render pass descriptor (sgj_mtl_render_pass_hook);
        // a compute encoder has none
        (void)slot;
        return sgj.mtl_counters && sgj.reserved_pass == (int)index && !_sg.cur_pass.is_compute;
    #else
        (void)slot; (void)index;
        return false;
    #endif
}

void sgj_backend_end_pass(sgj_slot& slot, uint32_t index) {
    #if defined(_SOKOL_ANY_GL)
        (void)slot; (void)index;
        glEndQuery(GL_TIME_ELAPSED);
    #elif defined(SOKOL_D3D11)
        _sg.d3d11.ctx->End(slot.stamps[2 * index + 1]);
        sgj.stats.queries_issued++;
    #elif defined(SOKOL_VULKAN)
        vkCmdWriteTimestamp2(_sg.vk.frame.cmd_buf, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.pool, 2 * index + 1);
        sgj.stats.queries_issued++;
    #else
        (void)slot; (void)index;
    #endif
}

void sgj_backend_close_slot(sgj_slot& slot) {
    #if defined(_SOKOL_ANY_GL)
        // the queries reach the GPU now, not at the next swap (an offscreen frame may have none)
        (void)slot;
        glFlush();
    #elif defined(SOKOL_D3D11)
        _sg.d3d11.ctx->End(slot.disjoint);
    #elif defined(SOKOL_METAL)
        // the frame time, and the moment the samples can be resolved
        slot.completion = std::make_shared<sgj_mtl_completion>();
        std::shared_ptr<sgj_mtl_completion> completion = slot.completion;
        if (_sg.mtl.cmd_buffer) {
            [_sg.mtl.cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> cmd_buf) {
                // another thread
                completion->gpu_start = cmd_buf.GPUStartTime;
                completion->gpu_end = cmd_buf.GPUEndTime;
                completion->done.store(true, std::memory_order_release);
            }];
        }
    #else
        (void)slot;
    #endif
}

// ---- backend: reading (never waits)

enum sgj_read_result { SGJ_READ_PENDING, SGJ_READ_DONE, SGJ_READ_DISJOINT };

sgj_read_result sgj_backend_read(sgj_slot& slot, sgj_resolved& out) {
    const uint32_t n = slot.num_passes;
    out.info.frame_valid = false;
    out.info.frame_gpu_ms = 0.0;
    #if defined(_SOKOL_ANY_GL)
        // queries complete in order, so the last issued one being available says the others are
        // (one availability query a commit, not one a pass: each is a driver round trip, GPU
        // campaign perf2)
        for (int i = (int)n - 1; i >= 0; i--) {
            if (!slot.pass_issued[i])
                continue;
            GLuint available = 0;
            glGetQueryObjectuiv(slot.queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
                return SGJ_READ_PENDING;
            break;
        }
        double sum = 0.0;
        bool any = false;
        for (uint32_t i = 0; i < n; i++) {
            out.passes[i].valid = false;
            out.passes[i].gpu_ms = 0.0;
            if (!slot.pass_issued[i])
                continue;
            uint64_t ns = 0;
            #if defined(SOKOL_GLCORE)
                GLuint64 value = 0;
                glGetQueryObjectui64v(slot.queries[i], GL_QUERY_RESULT, &value);
                ns = (uint64_t)value;
            #else
                #if defined(__ANDROID__)
                if (sgj.gl_get_query_ui64) {
                    sgj.gl_get_query_ui64(slot.queries[i], GL_QUERY_RESULT, &ns);
                } else
                #endif
                {
                    // 32 bits of nanoseconds: 4.29 s per pass
                    GLuint value = 0;
                    glGetQueryObjectuiv(slot.queries[i], GL_QUERY_RESULT, &value);
                    ns = (uint64_t)value;
                }
            #endif
            out.passes[i].gpu_ms = (double)ns * 1.0e-6;
            out.passes[i].valid = true;
            sum += out.passes[i].gpu_ms;
            any = true;
        }
        out.info.frame_gpu_ms = sum;
        out.info.frame_valid = any;
        return SGJ_READ_DONE;
    #elif defined(SOKOL_D3D11)
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        if (_sg.d3d11.ctx->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return SGJ_READ_PENDING;
        if (disjoint.Disjoint || disjoint.Frequency == 0)
            return SGJ_READ_DISJOINT;
        static uint64_t stamps[2 * SGJ_MAX_PASSES_PER_COMMIT];
        for (uint32_t i = 0; i < n; i++) {
            if (!slot.pass_issued[i])
                continue;
            for (int k = 0; k < 2; k++) {
                if (_sg.d3d11.ctx->GetData(slot.stamps[2 * i + k], &stamps[2 * i + k], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                    return SGJ_READ_PENDING;
            }
        }
        const double ms_per_tick = 1000.0 / (double)disjoint.Frequency;
        uint64_t first = 0, last = 0;
        bool any = false;
        for (uint32_t i = 0; i < n; i++) {
            out.passes[i].valid = slot.pass_issued[i] && stamps[2 * i + 1] >= stamps[2 * i];
            out.passes[i].gpu_ms = out.passes[i].valid ? (double)(stamps[2 * i + 1] - stamps[2 * i]) * ms_per_tick : 0.0;
            if (out.passes[i].valid) {
                if (!any || stamps[2 * i] < first) first = stamps[2 * i];
                if (!any || stamps[2 * i + 1] > last) last = stamps[2 * i + 1];
                any = true;
            }
        }
        out.info.frame_valid = any;
        out.info.frame_gpu_ms = any ? (double)(last - first) * ms_per_tick : 0.0;
        return SGJ_READ_DONE;
    #elif defined(SOKOL_VULKAN)
        static uint64_t stamps[2 * SGJ_MAX_PASSES_PER_COMMIT];
        const uint32_t count = 2 * n;
        if (count == 0)
            return SGJ_READ_DONE;
        // every query of the slot's passes; one that was never written stays unavailable, so
        // invalid passes are read one by one
        bool all_issued = true;
        for (uint32_t i = 0; i < n; i++)
            all_issued = all_issued && slot.pass_issued[i];
        if (all_issued) {
            const VkResult res = vkGetQueryPoolResults(_sg.vk.dev, slot.pool, 0, count, sizeof(uint64_t) * count, stamps,
                                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (res != VK_SUCCESS)
                return SGJ_READ_PENDING;
        } else {
            for (uint32_t i = 0; i < n; i++) {
                if (!slot.pass_issued[i])
                    continue;
                const VkResult res = vkGetQueryPoolResults(_sg.vk.dev, slot.pool, 2 * i, 2, sizeof(uint64_t) * 2, &stamps[2 * i],
                                                           sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
                if (res != VK_SUCCESS)
                    return SGJ_READ_PENDING;
            }
        }
        const double ms_per_tick = sgj.vk_ns_per_tick * 1.0e-6;
        uint64_t first = 0, last = 0;
        bool any = false;
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t begin = stamps[2 * i] & sgj.vk_valid_mask;
            const uint64_t end = stamps[2 * i + 1] & sgj.vk_valid_mask;
            out.passes[i].valid = slot.pass_issued[i] && end >= begin;
            out.passes[i].gpu_ms = out.passes[i].valid ? (double)(end - begin) * ms_per_tick : 0.0;
            if (out.passes[i].valid) {
                if (!any || begin < first) first = begin;
                if (!any || end > last) last = end;
                any = true;
            }
        }
        out.info.frame_valid = any;
        out.info.frame_gpu_ms = any ? (double)(last - first) * ms_per_tick : 0.0;
        return SGJ_READ_DONE;
    #elif defined(SOKOL_METAL)
        if (!slot.completion || !slot.completion->done.load(std::memory_order_acquire))
            return SGJ_READ_PENDING;
        const double gpu_ms = (slot.completion->gpu_end - slot.completion->gpu_start) * 1000.0;
        out.info.frame_valid = gpu_ms > 0.0;
        out.info.frame_gpu_ms = gpu_ms > 0.0 ? gpu_ms : 0.0;
        for (uint32_t i = 0; i < n; i++) {
            out.passes[i].valid = false;
            out.passes[i].gpu_ms = 0.0;
        }
        if (sgj.mtl_counters && slot.samples && n > 0) {
            if (@available(macOS 11.0, iOS 14.0, *)) {
                id<MTLCounterSampleBuffer> buffer = (id<MTLCounterSampleBuffer>)slot.samples;
                NSData* data = [buffer resolveCounterRange:NSMakeRange(0, SGJ_MTL_SAMPLES_PER_PASS * n)];
                // recalibrate GPU ticks against the CPU clock over the run so far
                MTLTimestamp cpu = 0, gpu = 0;
                [_sg.mtl.device sampleTimestamps:&cpu gpuTimestamp:&gpu];
                if (gpu > sgj.mtl_gpu0 && cpu > sgj.mtl_cpu0) {
                    sgj.mtl_ns_per_gpu_tick = (double)(cpu - sgj.mtl_cpu0) / (double)(gpu - sgj.mtl_gpu0);
                }
                if (data && data.length >= sizeof(MTLCounterResultTimestamp) * SGJ_MTL_SAMPLES_PER_PASS * n) {
                    const MTLCounterResultTimestamp* samples = (const MTLCounterResultTimestamp*)data.bytes;
                    for (uint32_t i = 0; i < n; i++) {
                        if (!slot.pass_issued[i])
                            continue;
                        // start of vertex, end of vertex, start of fragment, end of fragment; a
                        // stage that did not run (a pass that only clears) has no sample
                        const uint64_t start_vertex = samples[SGJ_MTL_SAMPLES_PER_PASS * i + 0].timestamp;
                        const uint64_t end_vertex = samples[SGJ_MTL_SAMPLES_PER_PASS * i + 1].timestamp;
                        const uint64_t start_fragment = samples[SGJ_MTL_SAMPLES_PER_PASS * i + 2].timestamp;
                        const uint64_t end_fragment = samples[SGJ_MTL_SAMPLES_PER_PASS * i + 3].timestamp;
                        const uint64_t invalid = MTLCounterErrorValue;
                        uint64_t begin = start_vertex != invalid && start_vertex != 0 ? start_vertex : start_fragment;
                        uint64_t end = end_fragment != invalid && end_fragment != 0 ? end_fragment : end_vertex;
                        if (begin == invalid || end == invalid || begin == 0 || end == 0 || end < begin)
                            continue;
                        out.passes[i].valid = true;
                        out.passes[i].gpu_ms = (double)(end - begin) * sgj.mtl_ns_per_gpu_tick * 1.0e-6;
                    }
                }
            }
        }
        return SGJ_READ_DONE;
    #else
        (void)slot; (void)n;
        return SGJ_READ_DONE;
    #endif
}

// GLES3/WebGL2: a disjoint event since the last check invalidates everything in flight
bool sgj_backend_disjoint_since_last_check(void) {
    #if defined(_SOKOL_ANY_GL)
        if (!sgj.gl_disjoint)
            return false;
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        return disjoint != 0;
    #else
        return false;
    #endif
}

// ---- the ring

void sgj_drop_slot(sgj_slot& slot, sgj_timer_reason reason) {
    if (!slot.in_flight)
        return;
    slot.in_flight = false;
    if (reason == SGJ_REASON_DISJOINT) sgj.stats.dropped_disjoint++;
    else if (reason == SGJ_REASON_DISABLED) sgj.stats.dropped_disabled++;
    else sgj.stats.dropped_overflow++;
}

sgj_resolved& sgj_push_resolved(void) {
    if (sgj.resolved_count == SGJ_RESOLVED_SLOTS) {
        // nobody popped: the oldest goes
        sgj.resolved_head = (sgj.resolved_head + 1) % SGJ_RESOLVED_SLOTS;
        sgj.resolved_count--;
        sgj.stats.dropped_overflow++;
    }
    const int index = (sgj.resolved_head + sgj.resolved_count) % SGJ_RESOLVED_SLOTS;
    sgj.resolved_count++;
    return sgj.resolved[index];
}

void sgj_poll(void) {
    const bool disjoint = sgj_backend_disjoint_since_last_check();
    // oldest first, so resolved commits queue in commit order
    for (int k = 0; k < SGJ_RING_SLOTS; k++) {
        sgj_slot* oldest = nullptr;
        for (int i = 0; i < SGJ_RING_SLOTS; i++) {
            sgj_slot& slot = sgj.slots[i];
            if (slot.in_flight && (!oldest || slot.commit_index < oldest->commit_index))
                oldest = &slot;
        }
        if (!oldest)
            break;
        sgj_slot& slot = *oldest;
        if (disjoint) {
            sgj_drop_slot(slot, SGJ_REASON_DISJOINT);
            continue;
        }
        static sgj_resolved scratch;
        const sgj_read_result result = sgj_backend_read(slot, scratch);
        if (result == SGJ_READ_PENDING) {
            if (slot.age > SGJ_MAX_AGE) {
                sgj_drop_slot(slot, SGJ_REASON_OVERFLOW);
                continue;
            }
            break;   // the later ones are newer still
        }
        if (result == SGJ_READ_DISJOINT) {
            sgj_drop_slot(slot, SGJ_REASON_DISJOINT);
            continue;
        }
        sgj_resolved& out = sgj_push_resolved();
        out.info = scratch.info;
        out.info.commit_index = slot.commit_index;
        out.info.view_tag = slot.view_tag;
        out.info.frame_is_sum = sgj.frame_is_sum;
        out.info.semantics = sgj.semantics;
        out.info.pass_reason = sgj.pass_reason;
        out.info.num_passes = slot.num_passes;
        out.info.dropped_passes = slot.dropped_passes;
        out.info.passes = out.passes;
        for (uint32_t i = 0; i < slot.num_passes; i++) {
            std::memcpy(out.labels[i], slot.labels[i], SGJ_LABEL_LEN);
            out.passes[i] = scratch.passes[i];
            out.passes[i].label = out.labels[i];
            out.passes[i].pass_index = i;
        }
        slot.in_flight = false;
        sgj.stats.resolved_commits++;
    }
}

// ---- the hooks

void sgj_open_slot_if_needed(void) {
    if (sgj.cur_slot >= 0)
        return;
    sgj_slot& slot = sgj.slots[sgj.next_slot];
    // a commit still unresolved after a whole ring: dropped (its pools are idle on the GPU,
    // sokol keeps fewer frames in flight than the ring has slots)
    sgj_drop_slot(slot, SGJ_REASON_OVERFLOW);
    sgj.cur_slot = sgj.next_slot;
    sgj.next_slot = (sgj.next_slot + 1) % SGJ_RING_SLOTS;
    slot.commit_index = _sg.frame_index;
    slot.view_tag = sgj.view_tag;
    slot.num_passes = 0;
    slot.dropped_passes = 0;
    slot.age = 0;
    std::memset(slot.pass_issued, 0, sizeof(slot.pass_issued));
    sgj_backend_open_slot(slot);
}

void sgj_hook_begin_pass(const char* label) {
    if (!_sg.cur_pass.valid) {
        sgj.reserved_pass = -1;
        return;
    }
    sgj_open_slot_if_needed();
    sgj_slot& slot = sgj.slots[sgj.cur_slot];
    uint32_t index = slot.num_passes;
    #if defined(SOKOL_METAL)
    if (sgj.reserved_pass >= 0)
        index = (uint32_t)sgj.reserved_pass;
    #endif
    if (index >= SGJ_MAX_PASSES_PER_COMMIT) {
        slot.dropped_passes++;
        sgj.reserved_pass = -1;
        return;
    }
    slot.num_passes = index + 1;
    const char* text = (label && label[0]) ? label : "pass";
    std::strncpy(slot.labels[index], text, SGJ_LABEL_LEN - 1);
    slot.labels[index][SGJ_LABEL_LEN - 1] = '\0';
    slot.pass_issued[index] = sgj_backend_begin_pass(slot, index);
    sgj.pass_open = slot.pass_issued[index];
    sgj.reserved_pass = -1;
}

void sgj_hook_end_pass(void) {
    if (!sgj.pass_open || sgj.cur_slot < 0)
        return;
    sgj_slot& slot = sgj.slots[sgj.cur_slot];
    sgj_backend_end_pass(slot, slot.num_passes - 1);
    sgj.pass_open = false;
}

void sgj_hook_commit(void) {
    if (sgj.cur_slot >= 0) {
        sgj_slot& slot = sgj.slots[sgj.cur_slot];
        sgj_backend_close_slot(slot);
        slot.in_flight = true;
        sgj.cur_slot = -1;
    }
    for (int i = 0; i < SGJ_RING_SLOTS; i++) {
        if (sgj.slots[i].in_flight)
            sgj.slots[i].age++;
    }
    sgj_poll();
    if (!sgj.enabled) {
        // switched off: what is in flight is not waited for
        for (int i = 0; i < SGJ_RING_SLOTS; i++)
            sgj_drop_slot(sgj.slots[i], SGJ_REASON_DISABLED);
        sgj_install_hooks(false);
    }
}

#if defined(SOKOL_METAL)
void sgj_hook_mtl_render_pass(void* pass_descriptor) {
    sgj.reserved_pass = -1;
    sgj_open_slot_if_needed();
    sgj_slot& slot = sgj.slots[sgj.cur_slot];
    if (!sgj.mtl_counters || !slot.samples || slot.num_passes >= SGJ_MAX_PASSES_PER_COMMIT)
        return;
    if (@available(macOS 11.0, iOS 14.0, *)) {
        const NSUInteger first = SGJ_MTL_SAMPLES_PER_PASS * slot.num_passes;
        MTLRenderPassDescriptor* desc = (__bridge MTLRenderPassDescriptor*)pass_descriptor;
        MTLRenderPassSampleBufferAttachmentDescriptor* attachment = desc.sampleBufferAttachments[0];
        attachment.sampleBuffer = (id<MTLCounterSampleBuffer>)slot.samples;
        attachment.startOfVertexSampleIndex = first + 0;
        attachment.endOfVertexSampleIndex = first + 1;
        attachment.startOfFragmentSampleIndex = first + 2;
        attachment.endOfFragmentSampleIndex = first + 3;
        sgj.reserved_pass = (int)slot.num_passes;
    }
}
#endif

void sgj_install_hooks(bool on) {
    _sg_jasper_hooks.begin_pass = on ? sgj_hook_begin_pass : nullptr;
    _sg_jasper_hooks.end_pass = on ? sgj_hook_end_pass : nullptr;
    _sg_jasper_hooks.commit = on ? sgj_hook_commit : nullptr;
    #if defined(SOKOL_METAL)
    _sg_jasper_hooks.mtl_begin_render_pass = on ? sgj_hook_mtl_render_pass : nullptr;
    #endif
}

bool sgj_make_ring(void) {
    if (sgj.slots)
        return true;
    sgj.slots = new sgj_slot[SGJ_RING_SLOTS]();
    sgj.resolved = new sgj_resolved[SGJ_RESOLVED_SLOTS]();
    for (int i = 0; i < SGJ_RING_SLOTS; i++) {
        if (!sgj_backend_make_slot(sgj.slots[i])) {
            for (int k = 0; k <= i; k++)
                sgj_backend_destroy_slot(sgj.slots[k]);
            delete[] sgj.slots;
            delete[] sgj.resolved;
            sgj.slots = nullptr;
            sgj.resolved = nullptr;
            sgj.state = SGJ_TIMER_UNAVAILABLE;
            sgj.reason = SGJ_REASON_DEVICE_UNSUPPORTED;
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" {

void sgj_gpu_timer_setup(const sgj_gpu_timer_desc* desc) {
    const bool host_query_reset = sgj.vk_host_query_reset;
    sgj = sgj_state_t();
    sgj.vk_host_query_reset = host_query_reset;
    sgj.setup = true;
    if (desc && desc->disabled) {
        sgj.state = SGJ_TIMER_UNAVAILABLE;
        sgj.reason = SGJ_REASON_DISABLED;
        return;
    }
    if (!_sg.valid) {
        sgj.reason = SGJ_REASON_BACKEND;
        return;
    }
    sgj_backend_detect();
}

void sgj_gpu_timer_shutdown(void) {
    sgj_install_hooks(false);
    if (sgj.slots) {
        #if defined(SOKOL_VULKAN)
        // the last commits may still write timestamps into the pools
        if (_sg.vk.dev)
            vkDeviceWaitIdle(_sg.vk.dev);
        #endif
        for (int i = 0; i < SGJ_RING_SLOTS; i++)
            sgj_backend_destroy_slot(sgj.slots[i]);
        delete[] sgj.slots;
        sgj.slots = nullptr;
    }
    delete[] sgj.resolved;
    sgj.resolved = nullptr;
    const bool host_query_reset = sgj.vk_host_query_reset;
    sgj = sgj_state_t();
    sgj.vk_host_query_reset = host_query_reset;
}

void sgj_gpu_timer_set_vulkan_host_query_reset(bool enabled) {
    sgj.vk_host_query_reset = enabled;
}

void sgj_gpu_timer_set_enabled(bool on) {
    if (!sgj.setup || sgj.state != SGJ_TIMER_AVAILABLE || on == sgj.enabled)
        return;
    if (on) {
        if (!sgj_make_ring())
            return;
        sgj.enabled = true;
        // a pass already open began without a query: its end hook finds no pass open
        sgj_install_hooks(true);
    } else {
        sgj.enabled = false;
        // the commit hook drops what is in flight and removes the hooks; outside a commit with
        // nothing recorded they go at once
        if (sgj.cur_slot < 0 && !_sg.cur_pass.in_pass) {
            for (int i = 0; i < SGJ_RING_SLOTS; i++)
                sgj_drop_slot(sgj.slots[i], SGJ_REASON_DISABLED);
            sgj_install_hooks(false);
        }
    }
}

bool sgj_gpu_timer_enabled(void) {
    return sgj.enabled;
}

void sgj_gpu_timer_set_view_tag(uint32_t tag) {
    sgj.view_tag = tag;
}

sgj_timer_state sgj_gpu_timer_state(const char** reason_out) {
    if (reason_out)
        *reason_out = sgj_gpu_timer_reason_name(sgj.reason);
    return sgj.state;
}

sgj_timer_reason sgj_gpu_timer_reason(void) {
    return sgj.reason;
}

sgj_timer_semantics sgj_gpu_timer_semantics(void) {
    return sgj.semantics;
}

sgj_timer_reason sgj_gpu_timer_pass_reason(void) {
    return sgj.state == SGJ_TIMER_AVAILABLE ? sgj.pass_reason : sgj.reason;
}

const char* sgj_gpu_timer_reason_name(sgj_timer_reason reason) {
    switch (reason) {
        case SGJ_REASON_NONE: return "";
        case SGJ_REASON_BACKEND: return "backend";
        case SGJ_REASON_EXTENSION_MISSING: return "extension_missing";
        case SGJ_REASON_DEVICE_UNSUPPORTED: return "device_unsupported";
        case SGJ_REASON_DISJOINT: return "disjoint";
        case SGJ_REASON_OVERFLOW: return "overflow";
        case SGJ_REASON_DISABLED: return "disabled";
    }
    return "";
}

const char* sgj_gpu_timer_semantics_name(sgj_timer_semantics semantics) {
    switch (semantics) {
        case SGJ_SEM_PIPELINE_POINT: return "pipeline_point";
        case SGJ_SEM_ELAPSED: return "elapsed";
        case SGJ_SEM_ENCODER_SPAN: return "encoder_span";
    }
    return "";
}

bool sgj_gpu_timer_pop(sgj_commit_times* out) {
    if (!sgj.resolved || sgj.resolved_count == 0)
        return false;
    sgj_resolved& front = sgj.resolved[sgj.resolved_head];
    sgj.popped.info = front.info;
    for (uint32_t i = 0; i < front.info.num_passes; i++) {
        std::memcpy(sgj.popped.labels[i], front.labels[i], SGJ_LABEL_LEN);
        sgj.popped.passes[i] = front.passes[i];
        sgj.popped.passes[i].label = sgj.popped.labels[i];
    }
    sgj.popped.info.passes = sgj.popped.passes;
    sgj.resolved_head = (sgj.resolved_head + 1) % SGJ_RESOLVED_SLOTS;
    sgj.resolved_count--;
    if (out)
        *out = sgj.popped.info;
    return true;
}

sgj_gpu_timer_stats sgj_gpu_timer_get_stats(void) {
    return sgj.stats;
}

uint64_t sgj_commit_index(void) {
    return _sg.frame_index;
}

bool sgj_pass_valid(void) {
    return _sg.cur_pass.in_pass && _sg.cur_pass.valid;
}

bool sgj_query_device_memory(sgj_device_memory* out) {
    if (!out || !_sg.valid)
        return false;
    *out = {};
    #if defined(_SOKOL_ANY_GL) && !defined(__EMSCRIPTEN__)
        // NVIDIA: total and currently available video memory (KiB); AMD: free texture memory
        const GLenum TOTAL_AVAILABLE_NVX = 0x9048, CURRENT_AVAILABLE_NVX = 0x9049, TEXTURE_FREE_ATI = 0x87FC;
        // the extensions, looked up once (the process keeps its GPU)
        static bool nvx = false, ati = false, looked_up = false;
        if (!looked_up) {
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; i++) {
                const char* name = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (!name) continue;
                nvx = nvx || std::strcmp(name, "GL_NVX_gpu_memory_info") == 0;
                ati = ati || std::strcmp(name, "GL_ATI_meminfo") == 0;
            }
            looked_up = true;
        }
        if (nvx) {
            GLint total = 0, available = 0;
            glGetIntegerv(TOTAL_AVAILABLE_NVX, &total);
            glGetIntegerv(CURRENT_AVAILABLE_NVX, &available);
            out->source = "GL_NVX_gpu_memory_info";
            out->budget_bytes = (uint64_t)total * 1024u;
            out->available_bytes = (uint64_t)available * 1024u;
            // a driver without a current figure (Mesa's software ones) reports only its total
            out->used_known = available > 0;
            out->used_bytes = out->used_known && out->budget_bytes > out->available_bytes ? out->budget_bytes - out->available_bytes : 0;
            return total > 0;
        }
        if (ati) {
            GLint free_memory[4] = {};
            glGetIntegerv(TEXTURE_FREE_ATI, free_memory);
            out->source = "GL_ATI_meminfo";
            out->available_bytes = (uint64_t)free_memory[0] * 1024u;
            return free_memory[0] > 0;
        }
        return false;
    #elif defined(SOKOL_VULKAN)
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(_sg.vk.phys_dev, nullptr, &count, nullptr);
        VkExtensionProperties* extensions = (VkExtensionProperties*)calloc(count ? count : 1, sizeof(VkExtensionProperties));
        vkEnumerateDeviceExtensionProperties(_sg.vk.phys_dev, nullptr, &count, extensions);
        bool budget = false;
        for (uint32_t i = 0; i < count && !budget; i++)
            budget = std::strcmp(extensions[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
        free(extensions);
        if (!budget)
            return false;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT heaps = {};
        heaps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 props = {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props.pNext = &heaps;
        vkGetPhysicalDeviceMemoryProperties2(_sg.vk.phys_dev, &props);
        // the device-local heaps (a software device has only host heaps: all of them)
        bool any_local = false;
        for (uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; i++)
            any_local = any_local || (props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        for (uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; i++) {
            if (any_local && !(props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
                continue;
            out->used_bytes += heaps.heapUsage[i];
            out->budget_bytes += heaps.heapBudget[i];
        }
        out->source = "VK_EXT_memory_budget";
        out->used_known = true;
        out->available_bytes = out->budget_bytes > out->used_bytes ? out->budget_bytes - out->used_bytes : 0;
        return true;
    #elif defined(SOKOL_D3D11)
        IDXGIDevice* dxgi_device = nullptr;
        if (FAILED(_sg.d3d11.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device)) || !dxgi_device)
            return false;
        IDXGIAdapter* adapter = nullptr;
        const HRESULT got_adapter = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(got_adapter) || !adapter)
            return false;
        IDXGIAdapter3* adapter3 = nullptr;
        const HRESULT got_adapter3 = adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&adapter3);
        adapter->Release();
        if (FAILED(got_adapter3) || !adapter3)
            return false;
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        const HRESULT queried = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        adapter3->Release();
        if (FAILED(queried))
            return false;
        out->source = "IDXGIAdapter3::QueryVideoMemoryInfo";
        out->used_bytes = info.CurrentUsage;
        out->used_known = true;
        out->budget_bytes = info.Budget;
        out->available_bytes = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
        return true;
    #elif defined(SOKOL_METAL)
        id<MTLDevice> device = _sg.mtl.device;
        out->source = "MTLDevice.currentAllocatedSize";
        out->used_bytes = (uint64_t)device.currentAllocatedSize;
        out->used_known = true;
        #if defined(_SG_TARGET_MACOS)
            out->budget_bytes = (uint64_t)device.recommendedMaxWorkingSetSize;
        #else
            if (@available(iOS 16.0, *)) {
                out->budget_bytes = (uint64_t)device.recommendedMaxWorkingSetSize;
            }
        #endif
        out->available_bytes = out->budget_bytes > out->used_bytes ? out->budget_bytes - out->used_bytes : 0;
        return true;
    #else
        return false;
    #endif
}

const char* sgj_device_name(void) {
    static char name[256];
    name[0] = '\0';
    if (!_sg.valid)
        return name;
    #if defined(_SOKOL_ANY_GL)
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        std::snprintf(name, sizeof(name), "%s", renderer ? renderer : "");
    #elif defined(SOKOL_VULKAN)
        std::snprintf(name, sizeof(name), "%s", _sg.vk.dev_props.properties.deviceName);
    #elif defined(SOKOL_D3D11)
        IDXGIDevice* dxgi_device = nullptr;
        if (SUCCEEDED(_sg.d3d11.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device)) && dxgi_device) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(adapter->GetDesc(&desc)))
                    std::snprintf(name, sizeof(name), "%ls", desc.Description);
                adapter->Release();
            }
            dxgi_device->Release();
        }
    #elif defined(SOKOL_METAL)
        std::snprintf(name, sizeof(name), "%s", [_sg.mtl.device.name UTF8String]);
    #endif
    return name;
}

bool sgj_device_is_cpu(void) {
    if (!_sg.valid)
        return false;
    #if defined(SOKOL_VULKAN)
        return _sg.vk.dev_props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    #else
        return false;
    #endif
}

// the API version the context reports: GL_VERSION ("OpenGL ES 3.2 Mesa ..." on a GLES context,
// "4.5 (Core Profile) Mesa ..." on a desktop one), Vulkan's apiVersion; "" elsewhere
const char* sgj_api_version(void) {
    static char version[256];
    version[0] = '\0';
    if (!_sg.valid)
        return version;
    #if defined(_SOKOL_ANY_GL)
        const char* text = (const char*)glGetString(GL_VERSION);
        std::snprintf(version, sizeof(version), "%s", text ? text : "");
    #elif defined(SOKOL_VULKAN)
        const uint32_t v = _sg.vk.dev_props.properties.apiVersion;
        std::snprintf(version, sizeof(version), "Vulkan %u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    #endif
    return version;
}

const char* sgj_pixel_format_name(int format) {
    switch ((sg_pixel_format)format) {
        case SG_PIXELFORMAT_NONE: return "NONE";
        case SG_PIXELFORMAT_R8: return "R8";
        case SG_PIXELFORMAT_R8SN: return "R8SN";
        case SG_PIXELFORMAT_R8UI: return "R8UI";
        case SG_PIXELFORMAT_R8SI: return "R8SI";
        case SG_PIXELFORMAT_R16: return "R16";
        case SG_PIXELFORMAT_R16SN: return "R16SN";
        case SG_PIXELFORMAT_R16UI: return "R16UI";
        case SG_PIXELFORMAT_R16SI: return "R16SI";
        case SG_PIXELFORMAT_R16F: return "R16F";
        case SG_PIXELFORMAT_RG8: return "RG8";
        case SG_PIXELFORMAT_RG8SN: return "RG8SN";
        case SG_PIXELFORMAT_RG8UI: return "RG8UI";
        case SG_PIXELFORMAT_RG8SI: return "RG8SI";
        case SG_PIXELFORMAT_R32UI: return "R32UI";
        case SG_PIXELFORMAT_R32SI: return "R32SI";
        case SG_PIXELFORMAT_R32F: return "R32F";
        case SG_PIXELFORMAT_RG16: return "RG16";
        case SG_PIXELFORMAT_RG16SN: return "RG16SN";
        case SG_PIXELFORMAT_RG16UI: return "RG16UI";
        case SG_PIXELFORMAT_RG16SI: return "RG16SI";
        case SG_PIXELFORMAT_RG16F: return "RG16F";
        case SG_PIXELFORMAT_RGBA8: return "RGBA8";
        case SG_PIXELFORMAT_SRGB8A8: return "SRGB8A8";
        case SG_PIXELFORMAT_RGBA8SN: return "RGBA8SN";
        case SG_PIXELFORMAT_RGBA8UI: return "RGBA8UI";
        case SG_PIXELFORMAT_RGBA8SI: return "RGBA8SI";
        case SG_PIXELFORMAT_BGRA8: return "BGRA8";
        case SG_PIXELFORMAT_RGB10A2: return "RGB10A2";
        case SG_PIXELFORMAT_RG11B10F: return "RG11B10F";
        case SG_PIXELFORMAT_RGB9E5: return "RGB9E5";
        case SG_PIXELFORMAT_RG32UI: return "RG32UI";
        case SG_PIXELFORMAT_RG32SI: return "RG32SI";
        case SG_PIXELFORMAT_RG32F: return "RG32F";
        case SG_PIXELFORMAT_RGBA16: return "RGBA16";
        case SG_PIXELFORMAT_RGBA16SN: return "RGBA16SN";
        case SG_PIXELFORMAT_RGBA16UI: return "RGBA16UI";
        case SG_PIXELFORMAT_RGBA16SI: return "RGBA16SI";
        case SG_PIXELFORMAT_RGBA16F: return "RGBA16F";
        case SG_PIXELFORMAT_RGBA32UI: return "RGBA32UI";
        case SG_PIXELFORMAT_RGBA32SI: return "RGBA32SI";
        case SG_PIXELFORMAT_RGBA32F: return "RGBA32F";
        case SG_PIXELFORMAT_DEPTH: return "DEPTH";
        case SG_PIXELFORMAT_DEPTH_STENCIL: return "DEPTH_STENCIL";
        case SG_PIXELFORMAT_BC1_RGBA: return "BC1_RGBA";
        case SG_PIXELFORMAT_BC2_RGBA: return "BC2_RGBA";
        case SG_PIXELFORMAT_BC3_RGBA: return "BC3_RGBA";
        case SG_PIXELFORMAT_BC3_SRGBA: return "BC3_SRGBA";
        case SG_PIXELFORMAT_BC4_R: return "BC4_R";
        case SG_PIXELFORMAT_BC4_RSN: return "BC4_RSN";
        case SG_PIXELFORMAT_BC5_RG: return "BC5_RG";
        case SG_PIXELFORMAT_BC5_RGSN: return "BC5_RGSN";
        case SG_PIXELFORMAT_BC6H_RGBF: return "BC6H_RGBF";
        case SG_PIXELFORMAT_BC6H_RGBUF: return "BC6H_RGBUF";
        case SG_PIXELFORMAT_BC7_RGBA: return "BC7_RGBA";
        case SG_PIXELFORMAT_BC7_SRGBA: return "BC7_SRGBA";
        case SG_PIXELFORMAT_ETC2_RGB8: return "ETC2_RGB8";
        case SG_PIXELFORMAT_ETC2_SRGB8: return "ETC2_SRGB8";
        case SG_PIXELFORMAT_ETC2_RGB8A1: return "ETC2_RGB8A1";
        case SG_PIXELFORMAT_ETC2_RGBA8: return "ETC2_RGBA8";
        case SG_PIXELFORMAT_ETC2_SRGB8A8: return "ETC2_SRGB8A8";
        case SG_PIXELFORMAT_EAC_R11: return "EAC_R11";
        case SG_PIXELFORMAT_EAC_R11SN: return "EAC_R11SN";
        case SG_PIXELFORMAT_EAC_RG11: return "EAC_RG11";
        case SG_PIXELFORMAT_EAC_RG11SN: return "EAC_RG11SN";
        case SG_PIXELFORMAT_ASTC_4x4_RGBA: return "ASTC_4x4_RGBA";
        case SG_PIXELFORMAT_ASTC_4x4_SRGBA: return "ASTC_4x4_SRGBA";
        default: return "?";
    }
}

} // extern "C"
