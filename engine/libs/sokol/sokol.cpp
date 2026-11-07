#include "Log.h"
#include <mutex>  // the pools' lock in sokol_gfx.h (PATCHES.md)
#define SOKOL_LOG(s) { SOKOL_ASSERT(s); jasperengine::Log::error(s); }

#define SOKOL_IMPL

#if defined(JASPERENGINE_ANDROID)
#define SOKOL_EXTERNAL_GL_LOADER
#include <GLES3/gl31.h>
#endif

#if defined(_WIN32)
#undef SOKOL_LOG
#define SOKOL_LOG(s) OutputDebugStringA(s)
#endif
/* this is only needed for the debug-inspection headers */
#define SOKOL_TRACE_HOOKS
/* sokol 3D-API defines are provided by build options */

// sokol_app (and the sokol_glue/sokol_fetch helpers that go with it) is no
// longer compiled: every platform now has its own native application backend
// under engine/platform, which owns the window, the event loop and the
// graphics device.

// the GPU timer's query entry points for sokol's own Win32 GL loader (sokol_gpu_timer_impl.h), and
// glFinish for the asynchronous creation's worker context (sokol_async_impl.h)
#define SG_GL_FUNCS_EXT \
    _SG_XMACRO(glGenQueries,          void, (GLsizei n, GLuint* ids)) \
    _SG_XMACRO(glDeleteQueries,       void, (GLsizei n, const GLuint* ids)) \
    _SG_XMACRO(glBeginQuery,          void, (GLenum target, GLuint id)) \
    _SG_XMACRO(glEndQuery,            void, (GLenum target)) \
    _SG_XMACRO(glGetQueryiv,          void, (GLenum target, GLenum pname, GLint* params)) \
    _SG_XMACRO(glGetQueryObjectuiv,   void, (GLuint id, GLenum pname, GLuint* params)) \
    _SG_XMACRO(glGetQueryObjectui64v, void, (GLuint id, GLenum pname, GLuint64* params)) \
    _SG_XMACRO(glFinish,              void, (void))

// the GPU timer's hook table, which sokol_gfx.h calls (marked "JasperEngine: gpu timer hook")
#include "sokol_gpu_timer.h"
#include "sokol_gfx.h"
#include "sokol_time.h"
// its backends, where sokol's state is visible
#include "sokol_gpu_timer_impl.h"
#include "sokol_async.h"
#include "sokol_async_impl.h"
//#include "sokol_audio.h"
//#include "sokol_log.h"

// The highest MSAA sample count up to requested (a power of two) that a multisampled RGBA8
// colour and depth-stencil target can have on this device: the post-processing MSAA of the
// engine (SokolFramebuffer::getMaxMsaaSamples). sokol_gfx only tells whether a format has
// MSAA at all, so the counts come from the backend here, where sokol's state is visible.
extern "C" int jasperengine_sg_max_msaa_samples(int requested){
    if (requested <= 1)
        return 1;
    int supported = requested;
#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    #ifndef GL_MAX_SAMPLES
        #define GL_MAX_SAMPLES 0x8D57
    #endif
    // GL picks a supported count at or above the requested one, up to GL_MAX_SAMPLES
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    while (supported > 1 && supported > maxSamples)
        supported /= 2;
#elif defined(SOKOL_VULKAN)
    const VkPhysicalDeviceLimits& limits = _sg.vk.dev_props.properties.limits;
    const VkSampleCountFlags counts = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts &
                                      limits.framebufferStencilSampleCounts;
    while (supported > 1 && !(counts & (VkSampleCountFlags)supported))
        supported /= 2;
#elif defined(SOKOL_D3D11)
    while (supported > 1){
        UINT colorLevels = 0;
        UINT depthLevels = 0;
        _sg.d3d11.dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)supported, &colorLevels);
        _sg.d3d11.dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, (UINT)supported, &depthLevels);
        if (colorLevels > 0 && depthLevels > 0)
            break;
        supported /= 2;
    }
#endif
    // Metal: every Apple GPU renders 2 and 4 samples
    return supported;
}

// Slots in use and free of a resource pool (by SystemRender's GpuResourceKind: buffer, image,
// sampler, view, pipeline, shader), for SokolSystem's pool limits and their tests. A pool of
// size N has N + 1 slots, slot 0 never used.
extern "C" void jasperengine_sg_pool_usage(int kind, int* used, int* free){
    const _sg_pool_t* pools[6] = {&_sg.pools.buffer_pool, &_sg.pools.image_pool, &_sg.pools.sampler_pool,
                                  &_sg.pools.view_pool, &_sg.pools.pipeline_pool, &_sg.pools.shader_pool};
    *used = 0;
    *free = 0;
    if (kind < 0 || kind >= 6 || !_sg.valid)
        return;
    _SGJ_POOL_LOCK();
    const _sg_pool_t* pool = pools[kind];
    *free = pool->queue_top;
    *used = (pool->size - 1) - pool->queue_top;
}

// Vulkan's per-frame shared buffers, for SokolSystem's budgets (Engine::GpuResourceLimits):
// 0 the stream staging buffer, 1 the uniform buffer, 2 the descriptor buffer. The bytes the
// frame being recorded has taken (0 before its first pass or upload), the buffer's size, and
// whether sokol found it full this frame. False on the other backends (no such buffers).
extern "C" bool jasperengine_sg_vk_frame_buffer_usage(int which, uint32_t* used, uint32_t* size, bool* overflown){
    *used = 0;
    *size = 0;
    *overflown = false;
#if defined(SOKOL_VULKAN)
    if (!_sg.valid || which < 0 || which > 2)
        return false;
    const _sg_vk_shared_buffer_t* buffers[3] = {&_sg.vk.stage.stream, &_sg.vk.uniforms.dbuf, &_sg.vk.bind};
    const _sg_vk_shared_buffer_t* buffer = buffers[which];
    *size = buffer->size;
    if (buffer->cur_buf){
        *used = buffer->offset;
        *overflown = buffer->overflown;
    }
    return true;
#else
    (void)which;
    return false;
#endif
}

// Whether the draw being set up is still valid: a failed sg_apply_bindings (a destroyed buffer
// or image) turns it off until the next sg_apply_pipeline, so a renderer that skips applying the
// pipeline already applied (SokolDrawBatch, spec S55) applies it again after such a draw.
extern "C" bool jasperengine_sg_next_draw_valid(void){
    return _sg.valid && _sg.next_draw_valid;
}

// Vulkan: an upload into a STREAM buffer larger than the per-frame stream staging buffer, made
// alone through the copy staging path (in chunks of the copy staging buffer, the queue waited for
// first) instead of the stream staging buffer it could never fit (spec S55 R6). It counts as the
// buffer's update of the frame. False on the other backends (sg_update_buffer writes the buffer).
extern "C" bool jasperengine_sg_vk_update_buffer_alone(sg_buffer buf_id, const void* data, size_t size){
#if defined(SOKOL_VULKAN)
    if (!_sg.valid || !data || size == 0)
        return false;
    _sg_buffer_t* buf = _sg_lookup_buffer(buf_id.id);
    if (!buf || buf->slot.state != SG_RESOURCESTATE_VALID || size > (size_t)buf->cmn.size)
        return false;
    const sg_range range = { data, size };
    _sg_vk_staging_copy_buffer_data(buf, &range, 0, true);
    buf->cmn.update_frame_index = _sg.frame_index;
    buf->cmn.append_pos = 0;
    buf->cmn.append_overflow = false;
    return true;
#else
    (void)buf_id; (void)data; (void)size;
    return false;
#endif
}

// The same for a STREAM image: its mip levels through the copy staging path.
extern "C" bool jasperengine_sg_vk_update_image_alone(sg_image img_id, const sg_image_data* data){
#if defined(SOKOL_VULKAN)
    if (!_sg.valid || !data)
        return false;
    _sg_image_t* img = _sg_lookup_image(img_id.id);
    if (!img || img->slot.state != SG_RESOURCESTATE_VALID)
        return false;
    _sg_vk_staging_copy_image_data(img, data, true);
    img->cmn.upd_frame_index = _sg.frame_index;
    return true;
#else
    (void)img_id; (void)data;
    return false;
#endif
}

// Vulkan: the descriptor buffer bytes the next sg_apply_bindings and sg_draw take (their
// textures', samplers' and storage buffers' descriptor set when `bindings` (the draw applies
// them: SokolSystem skips bindings equal to the applied ones), and the uniform blocks' one when
// uniforms were applied since the last draw, each rounded up to the buffer's alignment as sokol
// places them), the bytes the frame has taken and the buffer's size; false when they do not fit
// in it, or in `limit` bytes when that is less (sokol would drop the draw). True with nothing
// needed on the other backends and outside a valid pass or pipeline (sokol skips such a draw
// before it gets there).
extern "C" bool jasperengine_sg_vk_next_draw_descriptors(uint32_t limit, bool bindings, uint32_t* need, uint32_t* used, uint32_t* size){
    *need = 0;
    *used = 0;
    *size = 0;
#if defined(SOKOL_VULKAN)
    if (!_sg.valid)
        return true;
    const _sg_vk_shared_buffer_t* bind = &_sg.vk.bind;
    *size = bind->size;
    *used = bind->cur_buf ? bind->offset : 0;
    if (!_sg.cur_pass.valid || !_sg.next_draw_valid || _sg.cur_pass.is_compute)
        return true;
    const _sg_pipeline_t* pip = _sg_pipeline_ref_ptr_or_null(&_sg.cur_pip);
    const _sg_shader_t* shd = pip ? _sg_shader_ref_ptr_or_null(&pip->cmn.shader) : nullptr;
    if (!shd)
        return true;
    const uint64_t room = limit < bind->size ? limit : bind->size;
    uint64_t offset = *used;
    bool fits = !bind->overflown;
    const uint64_t sets[2] = {bindings ? shd->vk.view_smp_dset_size : 0, _sg.vk.uniforms.dirty ? shd->vk.ub_dset_size : 0};
    for (uint64_t set : sets){
        if (set == 0)
            continue;
        if (offset + set > room)
            fits = false;
        offset = (offset + set + bind->align - 1) / bind->align * bind->align;
    }
    *need = (uint32_t)(offset - *used);
    return fits;
#else
    (void)limit;
    (void)bindings;
    return true;
#endif
}

// Returns once the GPU has finished every command buffer committed so far (Metal; nothing on the
// other backends). Engine::systemUpdate calls it between frames: Metal frees what a frame released
// SG_NUM_INFLIGHT_FRAMES + 1 commits later and only a frame with a pass waits for the GPU, so
// frames without passes (a hidden window's) would free what the GPU still reads. Vulkan frees
// behind its frame fences, D3D11 and GL through the driver.
extern "C" void jasperengine_sg_wait_gpu_idle(void){
#if defined(SOKOL_METAL)
    if (!_sg.valid || _sg.mtl.cmd_queue == nil)
        return;
    id<MTLCommandBuffer> commandBuffer = [_sg.mtl.cmd_queue commandBuffer];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
#endif
}
