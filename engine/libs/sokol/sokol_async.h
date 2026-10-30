// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SOKOL_ASYNC_H
#define SOKOL_ASYNC_H

// JasperEngine's asynchronous creation extension of sokol_gfx (spec S32): shaders and pipelines
// whose driver work runs off the render thread. sokol creates a resource in three steps, which
// this splits where its own code already does:
//
//   prepare   (render thread)  defaults, validation and the common init of an ALLOC'd slot
//   create    (any thread the backend allows, see sgj_async_caps)  the backend object
//   publish   (render thread)  the slot's state, sokol's stats and the init trace hook
//
// The slot stays ALLOC (sg_query_*_state) until it is published, and sokol never reads the
// backend fields of an ALLOC slot, so a create on another thread writes memory nothing else reads.
// The caller (engine/renders/sokol/SokolAsyncCreate) owns the description's memory until publish
// and never destroys a slot whose create has not returned.
//
// The backend code lives in sokol_async_impl.h, compiled into sokol's translation unit
// (sokol.cpp). sokol_gfx.h calls it through the hooks marked "JasperEngine: async creation":
// a GL program issued here is adopted by _sg_gl_create_shader, Vulkan pipelines use the pipeline
// cache made here, and Metal libraries and render pipeline states come from the completion
// handlers started here.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// sokol_gfx.h guards only its declarations: sokol.cpp, which includes it with SOKOL_IMPL first,
// must not enter its implementation twice
#ifndef SOKOL_GFX_INCLUDED
#include "sokol_gfx.h"
#endif

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct sgj_async_caps {
    bool gl;                    // a GL-family backend (GL 3.3 core, GLES3, WebGL2)
    bool gl_parallel_compile;   // KHR/ARB_parallel_shader_compile (enabled on the web)
    bool shader_on_thread;      // sgj_shader_create may run on another thread (D3D11; GL with a current shared context)
    bool pipeline_on_thread;    // sgj_pipeline_create may run on another thread (Vulkan)
    bool metal;                 // Metal: libraries and render pipeline states through completion handlers
    bool pipeline_compiles;     // pipeline creation compiles shader code (Vulkan, Metal)
    bool gl_software;           // a software GL (llvmpipe, softpipe, SwiftShader): no warm draws
} sgj_async_caps;

// After sg_setup, on the render thread: preloads what a creation thread must not load (D3D11's
// compiler), enables KHR_parallel_shader_compile on the web, and reports what the backend allows
void sgj_async_setup(void);
sgj_async_caps sgj_async_query_caps(void);
// Before sg_shutdown, on the render thread
void sgj_async_shutdown(void);

// ---- shaders. `desc` is replaced by the description with defaults (the pointers stay the
// caller's). Null: the slot is not ALLOC, or the description is invalid, and it is published
// FAILED already
void* sgj_shader_prepare(sg_shader shd, sg_shader_desc* desc);
sg_resource_state sgj_shader_create(void* slot, const sg_shader_desc* desc);
void sgj_shader_publish(void* slot, sg_shader shd, const sg_shader_desc* desc, sg_resource_state state);

// ---- pipelines (as shaders). The pipeline's shader must be VALID at prepare
void* sgj_pipeline_prepare(sg_pipeline pip, sg_pipeline_desc* desc);
sg_resource_state sgj_pipeline_create(void* slot, const sg_pipeline_desc* desc);
void sgj_pipeline_publish(void* slot, sg_pipeline pip, const sg_pipeline_desc* desc, sg_resource_state state);

// ---- GL family: the render thread's own context (no worker context)
typedef struct sgj_gl_program {
    uint32_t program;
    uint32_t vertex;
    uint32_t fragment;
} sgj_gl_program;
// glCompileShader x2 and glLinkProgram, without reading a status (vertex and fragment only:
// false for a compute shader, which takes the synchronous path)
bool sgj_gl_issue(const sg_shader_desc* desc, sgj_gl_program* out);
// GL_COMPLETION_STATUS_KHR of the program (never waits); true without the extension
bool sgj_gl_is_complete(const sgj_gl_program* prog);
// Reads the link status (a wait if the driver is not done) and, linked, hands the program to
// sokol's creation; not linked, logs the shader and program logs and deletes them
sg_resource_state sgj_gl_adopt(void* slot, const sg_shader_desc* desc, sgj_gl_program* prog);
void sgj_gl_discard(sgj_gl_program* prog);
// glFinish on the calling thread's context: a program made on a worker context is complete
// before the render thread's context uses it
void sgj_gl_finish(void);

// ---- GL family: a warm draw on the worker context. The render thread describes it from a
// pipeline and the bindings an object draws with (plain GL names and values, so the worker never
// reads sokol's pools); the worker draws three vertices into a 1x1 target of the pipeline's
// formats, which makes the driver compile the program's variant for that state there
#define SGJ_GL_WARM_MAX_UNITS 16
typedef struct sgj_gl_warm_attr {
    int8_t enabled;
    int8_t divisor;
    uint8_t size;
    uint8_t normalized;
    uint8_t integer;
    uint8_t stride;
    int32_t offset;
    uint32_t type;
} sgj_gl_warm_attr;
typedef struct sgj_gl_warm_sampler {
    uint32_t min_filter;                    // GLenums
    uint32_t mag_filter;
    uint32_t wrap_s;
    uint32_t wrap_t;
    uint32_t wrap_r;
    uint32_t compare;                       // 0: no comparison
    uint32_t max_anisotropy;
} sgj_gl_warm_sampler;
typedef struct sgj_gl_warm_draw {
    uint32_t program;
    uint32_t primitive;                     // GLenum
    sgj_gl_warm_attr attrs[SG_MAX_VERTEX_ATTRIBUTES];
    sg_depth_state depth;
    sg_stencil_state stencil;
    sg_blend_state blend;
    sg_color_mask color_mask;
    sg_cull_mode cull_mode;
    sg_face_winding face_winding;
    bool alpha_to_coverage;
    int color_count;
    sg_pixel_format color_formats[SG_MAX_COLOR_ATTACHMENTS];
    sg_pixel_format depth_format;
    int sample_count;
    // per texture unit, what the driver's variant depends on: the worker binds its own 1x1
    // texture of that target and format and its own sampler of that state, never the render
    // thread's objects (which that thread may respecify or delete meanwhile)
    int unit_count;
    uint32_t unit_target[SGJ_GL_WARM_MAX_UNITS];        // GLenum, 0: unit unused
    sg_pixel_format unit_format[SGJ_GL_WARM_MAX_UNITS];
    int unit_samples[SGJ_GL_WARM_MAX_UNITS];
    sgj_gl_warm_sampler unit_sampler[SGJ_GL_WARM_MAX_UNITS];
} sgj_gl_warm_draw;
// Render thread: false when the pipeline or a binding is not valid (nothing to warm). key: what
// decides the driver's variant (program, pipeline description, texture formats and sampler states)
bool sgj_gl_warm_describe(sg_pipeline pip, const sg_bindings* bindings, sgj_gl_warm_draw* out, uint64_t* key);
// Worker context
void sgj_gl_warm_run(const sgj_gl_warm_draw* draw);
// Worker context, before it is released: the VAO, buffer and targets the warm draws made
void sgj_gl_warm_release(void);
// The GL program of a valid shader (0 otherwise): warm draws keyed by it are forgotten with it
uint32_t sgj_gl_shader_program(sg_shader shd);

// ---- Vulkan: the pipeline cache every pipeline is created through
typedef struct sgj_vk_device_identity {
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint8_t uuid[16];           // VkPhysicalDeviceProperties::pipelineCacheUUID
} sgj_vk_device_identity;
bool sgj_vk_device_identity_query(sgj_vk_device_identity* out);
// Creates the cache, seeded with `data` (null or empty: an empty cache); false without Vulkan
bool sgj_vk_pipeline_cache_create(const void* data, size_t size);
// The cache's data: the size when dst is null, else the bytes written (0 on failure)
size_t sgj_vk_pipeline_cache_data(void* dst, size_t capacity);
void sgj_vk_pipeline_cache_destroy(void);

// ---- Metal: completion handlers. begin starts the work (render thread, returns at once), ready
// says whether every handler has run (any thread may ask), finish publishes on the render thread
void* sgj_mtl_shader_begin(void* slot, const sg_shader_desc* desc);
void* sgj_mtl_pipeline_begin(void* slot, const sg_pipeline_desc* desc);
bool sgj_mtl_job_ready(void* job);
sg_resource_state sgj_mtl_job_finish(void* job, void* slot, const void* desc);
void sgj_mtl_job_free(void* job);

#if defined(__cplusplus)
}
#endif

#endif // SOKOL_ASYNC_H
