// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SOKOL_GPU_TIMER_H
#define SOKOL_GPU_TIMER_H

// JasperEngine's GPU timer extension of sokol_gfx (spec S15 R1-R5, R21-R25): the GPU time of
// every render pass begun through sg_begin_pass, per commit, on GL 3.3 core, GLES3
// (EXT_disjoint_timer_query), WebGL2 (EXT_disjoint_timer_query_webgl2), D3D11, Vulkan and Metal.
//
// The backend code lives in sokol_gpu_timer_impl.h, compiled into sokol's own translation unit
// (sokol.cpp), where sokol's state is visible. sokol_gfx.h calls it through four hooks marked
// "JasperEngine: gpu timer hook" (after the backend began a pass, before it ends one, before the
// commit submits, and on Metal before the render encoder is made); with the timer off the
// hook table is empty and each site costs one null-pointer test.
//
// Results are never waited for: a commit's passes resolve when the GPU has finished them (two
// to four commits later), sgj_gpu_timer_pop hands them out oldest first, and a commit that cannot
// be resolved is dropped with a counted reason (sgj_gpu_timer_stats).
//
// Semantics of the numbers:
//   SGJ_SEM_PIPELINE_POINT (Vulkan, D3D11): timestamps at the pass's start and end; the frame
//       time is the last pass end minus the first pass begin.
//   SGJ_SEM_ELAPSED (the GL family): one GL_TIME_ELAPSED query per pass; the frame time is the
//       sum of the passes (frame_is_sum).
//   SGJ_SEM_ENCODER_SPAN (Metal): stage-boundary samples of each render encoder. Encoders may
//       overlap on the GPU, so pass times are upper bounds and must not be summed; the frame
//       time is the command buffer's GPUEndTime - GPUStartTime.

#include <stdbool.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum sgj_timer_state {
    SGJ_TIMER_AVAILABLE,
    SGJ_TIMER_UNAVAILABLE,
} sgj_timer_state;

typedef enum sgj_timer_semantics {
    SGJ_SEM_PIPELINE_POINT,
    SGJ_SEM_ELAPSED,
    SGJ_SEM_ENCODER_SPAN,
} sgj_timer_semantics;

// why the timer, a commit or a pass has no time
typedef enum sgj_timer_reason {
    SGJ_REASON_NONE,
    SGJ_REASON_BACKEND,             // a backend without timer support (WebGPU, the dummy backend)
    SGJ_REASON_EXTENSION_MISSING,   // GLES3/WebGL2 without the disjoint timer query extension
    SGJ_REASON_DEVICE_UNSUPPORTED,  // no timestamp bits, no host query reset, no Metal counters
    SGJ_REASON_DISJOINT,            // the GPU reported a disjoint event: the commit is invalid
    SGJ_REASON_OVERFLOW,            // more passes than a commit holds, or not resolved in time
    SGJ_REASON_DISABLED,            // switched off (JASPERENGINE_GPU_TIMER=off, or while disabling)
} sgj_timer_reason;

typedef struct sgj_pass_time {
    const char* label;              // the sg_pass label ("pass" when it had none)
    uint32_t pass_index;            // in the commit, from 0
    double gpu_ms;
    bool valid;
} sgj_pass_time;

typedef struct sgj_commit_times {
    uint64_t commit_index;          // sg_commit count when the commit was made (sgj_commit_index)
    uint32_t view_tag;              // sgj_gpu_timer_set_view_tag before its first pass
    double frame_gpu_ms;
    bool frame_valid;
    bool frame_is_sum;              // the GL family: the sum of the pass times
    sgj_timer_semantics semantics;
    sgj_timer_reason pass_reason;   // why passes are not valid (Metal without counters: device_unsupported)
    uint32_t num_passes;
    const sgj_pass_time* passes;    // valid until the next sgj_gpu_timer_pop
    uint32_t dropped_passes;        // passes past the per-commit capacity (not in `passes`)
} sgj_commit_times;

typedef struct sgj_gpu_timer_desc {
    bool disabled;                  // report SGJ_REASON_DISABLED and never issue a query
} sgj_gpu_timer_desc;

typedef struct sgj_gpu_timer_stats {
    uint64_t resolved_commits;
    uint64_t dropped_disjoint;      // commits dropped because of a disjoint event
    uint64_t dropped_overflow;      // commits never resolved, or resolved but never popped
    uint64_t dropped_disabled;      // commits in flight when the timer was switched off
    uint64_t queries_issued;
} sgj_gpu_timer_stats;

// The most passes one commit records (more are counted in dropped_passes).
#define SGJ_MAX_PASSES_PER_COMMIT 256

// After sg_setup (SokolSystem::setup), before sg_shutdown.
void sgj_gpu_timer_setup(const sgj_gpu_timer_desc* desc);
void sgj_gpu_timer_shutdown(void);

// Vulkan only, before sgj_gpu_timer_setup: whoever made the VkDevice says whether it enabled
// VkPhysicalDeviceVulkan12Features::hostQueryReset (without it: device_unsupported).
void sgj_gpu_timer_set_vulkan_host_query_reset(bool enabled);

// Starts or stops issuing queries. Outside a pass any time; switching off drops what is in flight.
void sgj_gpu_timer_set_enabled(bool on);
bool sgj_gpu_timer_enabled(void);
// The tag of the commits that start from now (the engine's view id, or the game frame).
void sgj_gpu_timer_set_view_tag(uint32_t tag);

sgj_timer_state sgj_gpu_timer_state(const char** reason_out);
sgj_timer_reason sgj_gpu_timer_reason(void);
sgj_timer_semantics sgj_gpu_timer_semantics(void);
// Why passes have no time though frames do (Metal without counter sampling), else NONE.
sgj_timer_reason sgj_gpu_timer_pass_reason(void);
// The short name of a reason ("extension_missing") and of a semantics value ("pipeline_point").
const char* sgj_gpu_timer_reason_name(sgj_timer_reason reason);
const char* sgj_gpu_timer_semantics_name(sgj_timer_semantics semantics);

// Pops the oldest resolved commit; false when none is ready (never blocks).
bool sgj_gpu_timer_pop(sgj_commit_times* out);
sgj_gpu_timer_stats sgj_gpu_timer_get_stats(void);

// sokol's commit counter: the commit_index the next sg_commit will carry.
uint64_t sgj_commit_index(void);
// Inside a pass (a trace hook): whether sokol began it (false: it failed validation, or its
// attachments or the swapchain were not usable, and nothing of it reaches the GPU).
bool sgj_pass_valid(void);
// The name of a pixel format without its prefix ("RGBA8", "DEPTH_STENCIL").
const char* sgj_pixel_format_name(int sg_pixel_format);

// What the driver says about device memory, where it says it (spec S15 R9): Vulkan
// VK_EXT_memory_budget (device-local heaps), Metal currentAllocatedSize and
// recommendedMaxWorkingSetSize, D3D11 IDXGIAdapter3::QueryVideoMemoryInfo, GL
// GL_NVX_gpu_memory_info or GL_ATI_meminfo (free memory only). False elsewhere.
typedef struct sgj_device_memory {
    const char* source;
    uint64_t used_bytes;
    bool used_known;
    uint64_t budget_bytes;
    uint64_t available_bytes;
} sgj_device_memory;
bool sgj_query_device_memory(sgj_device_memory* out);
// The GPU's name as the backend reports it ("" when it does not).
const char* sgj_device_name(void);
// the context's API version (GL_VERSION, the Vulkan apiVersion; "" elsewhere)
const char* sgj_api_version(void);
// Whether the API says the device is the CPU (Vulkan's VK_PHYSICAL_DEVICE_TYPE_CPU, lavapipe's).
// False where the API has no such property: GL names its renderer only.
bool sgj_device_is_cpu(void);

#if defined(__cplusplus)
} // extern "C"
#endif

// ---- the hook table sokol_gfx.h calls (sokol's own translation unit only)
#if defined(SOKOL_IMPL) || defined(SOKOL_GFX_IMPL)
#if defined(__cplusplus)
extern "C" {
#endif
typedef struct _sg_jasper_hooks_t {
    void (*begin_pass)(const char* label);      // after the backend began the pass
    void (*end_pass)(void);                     // before the backend ends it
    void (*commit)(void);                       // before the commit submits
    void (*mtl_begin_render_pass)(void* pass_descriptor);  // Metal: MTLRenderPassDescriptor*, before its encoder
} _sg_jasper_hooks_t;
extern _sg_jasper_hooks_t _sg_jasper_hooks;
#if defined(__cplusplus)
} // extern "C"
#endif
#endif

#endif // SOKOL_GPU_TIMER_H
