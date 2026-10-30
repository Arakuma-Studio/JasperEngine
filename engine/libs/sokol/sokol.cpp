#include "Log.h"
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
