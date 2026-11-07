// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The backends of the asynchronous creation extension (sokol_async.h, spec S32). Included by
// sokol.cpp right after sokol_gfx.h with SOKOL_IMPL, so sokol's state and private functions are
// visible (as in sokol_gpu_timer_impl.h). The prepare and publish halves run on the render thread;
// sgj_shader_create / sgj_pipeline_create run where sgj_async_caps allows:
//
//   GL family  _sg_gl_create_shader on a worker context in the render context's share group, or,
//              without one, sgj_gl_issue / sgj_gl_is_complete / sgj_gl_adopt on the render
//              thread's context (KHR_parallel_shader_compile, or a deferred status read). Warm
//              draws compile the program's state variant on the worker context.
//   D3D11      _sg_d3d11_create_shader on a worker thread (ID3D11Device is free-threaded; the
//              compiler DLL is loaded here at setup, not lazily on the worker).
//   Vulkan     _sg_vk_create_pipeline on a worker thread, through the device's pipeline cache.
//   Metal      newLibraryWithSource / newRenderPipelineStateWithDescriptor with completion
//              handlers; results enter sokol's id pool on the render thread only.

#include <atomic>
#include <cstring>
#include <vector>

#if defined(_SOKOL_ANY_GL)
    #if defined(__EMSCRIPTEN__)
        #include <emscripten/html5.h>
    #elif !defined(_WIN32)
        #include <dlfcn.h>
    #endif
    #ifndef GL_COMPLETION_STATUS_KHR
        #define GL_COMPLETION_STATUS_KHR 0x91B1
    #endif
    #ifndef GL_DEPTH_STENCIL_ATTACHMENT
        #define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
    #endif
    #ifndef GL_SAMPLE_ALPHA_TO_COVERAGE
        #define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
    #endif
#endif

#if defined(SOKOL_METAL)
    #if !__has_feature(objc_arc)
        #error "sokol_async_impl.h needs ARC (sokol's CMake target compiles sokol.cpp with -fobjc-arc)"
    #endif
#endif

namespace {
    sgj_async_caps sgjCaps = {};

#if defined(_SOKOL_ANY_GL) && !defined(__EMSCRIPTEN__)
    // glMaxShaderCompilerThreadsKHR/ARB(0xFFFFFFFF): the driver's own compile threads. The
    // extension says the initial count is implementation-defined, and NVIDIA's is none: until an
    // application asks, glCompileShader/glLinkProgram return only once done and
    // GL_COMPLETION_STATUS_KHR is always true (an RTX 3080 Ti, driver 595.99: a 2.1 s Lit lobe
    // variant link stayed on the calling thread in `parallel` mode; docs/perf/gpu-campaign.md).
    // Called in parallel mode only (see sgj_gl_enable_compile_threads).
    using MaxShaderCompilerThreadsProc = void (*)(GLuint);
    void* sgjGlProc(const char* name){
    #if defined(_WIN32)
        PROC proc = wglGetProcAddress(name);
        return (proc == 0 || proc == (PROC)1 || proc == (PROC)2 || proc == (PROC)3 || proc == (PROC)-1) ? nullptr : (void*)proc;
    #elif defined(__APPLE__)
        (void)name;
        return nullptr;  // macOS GL 4.1 has no parallel compile extension
    #else
        // GLX (desktop Linux) or EGL (Android, Linux GLES): whichever the process loaded
        using GetProc = void* (*)(const char*);
        static GetProc getProc = [](){
            void* f = dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
            if (!f) f = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
            return reinterpret_cast<GetProc>(f);
        }();
        void* proc = getProc ? getProc(name) : nullptr;
        return proc ? proc : dlsym(RTLD_DEFAULT, name);
    #endif
    }
#endif
}

extern "C" bool sgj_gl_enable_compile_threads(void){
#if defined(_SOKOL_ANY_GL) && !defined(__EMSCRIPTEN__)
    if (!sgjCaps.gl_parallel_compile)
        return false;
    auto set = reinterpret_cast<MaxShaderCompilerThreadsProc>(sgjGlProc("glMaxShaderCompilerThreadsKHR"));
    if (!set)
        set = reinterpret_cast<MaxShaderCompilerThreadsProc>(sgjGlProc("glMaxShaderCompilerThreadsARB"));
    if (!set)
        return false;
    set(0xFFFFFFFFu);
    while (glGetError() != GL_NO_ERROR) {}
    return true;
#else
    // WebGL: the browser's compile is parallel once the extension is enabled
    return sgjCaps.gl_parallel_compile;
#endif
}

extern "C" void sgj_async_setup(void){
    sgjCaps = {};
#if defined(_SOKOL_ANY_GL)
    sgjCaps.gl = true;
    {
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        sgjCaps.gl_software = renderer && (std::strstr(renderer, "llvmpipe") || std::strstr(renderer, "softpipe") ||
                                           std::strstr(renderer, "SwiftShader") || std::strstr(renderer, "Software Rasterizer"));
    }
    #if defined(__EMSCRIPTEN__)
        // WebGL extensions work once enabled; enableExtensionsByDefault does not cover this one everywhere
        sgjCaps.gl_parallel_compile = emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "KHR_parallel_shader_compile") == EM_TRUE;
    #else
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; i++){
            const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (ext && (std::strcmp(ext, "GL_KHR_parallel_shader_compile") == 0 || std::strcmp(ext, "GL_ARB_parallel_shader_compile") == 0))
                sgjCaps.gl_parallel_compile = true;
        }
    #endif
    // what a worker context may do is the engine's to say: it knows whether the platform gave one
    sgjCaps.shader_on_thread = false;
#elif defined(SOKOL_D3D11)
    sgjCaps.shader_on_thread = _sg_d3d11_load_d3dcompiler_dll();
#elif defined(SOKOL_VULKAN)
    sgjCaps.pipeline_on_thread = true;
    sgjCaps.pipeline_compiles = true;
#elif defined(SOKOL_METAL)
    sgjCaps.metal = true;
    sgjCaps.pipeline_compiles = true;
#endif
}

extern "C" sgj_async_caps sgj_async_query_caps(void){
    return sgjCaps;
}

extern "C" void sgj_async_shutdown(void){
    sgjCaps = {};
}

// ------------------------------------------------------------------ prepare / create / publish

extern "C" void* sgj_shader_prepare(sg_shader shd_id, sg_shader_desc* desc){
    SOKOL_ASSERT(_sg.valid && desc);
    *desc = _sg_shader_desc_defaults(desc);
    _sg_shader_t* shd = _sg_lookup_shader(shd_id.id);
    if (!shd)
        return nullptr;
    if (shd->slot.state != SG_RESOURCESTATE_ALLOC){
        _SG_ERROR(INIT_SHADER_INVALID_STATE);
        return nullptr;
    }
    if (!_sg_validate_shader_desc(desc) || !_sg_validate_shader_binding_limits(desc)){
        shd->slot.state = SG_RESOURCESTATE_FAILED;
        _sg_resource_stats_inc(shaders.inited);
        _SG_TRACE_ARGS(init_shader, shd_id, desc);
        return nullptr;
    }
    _sg_shader_common_init(&shd->cmn, desc);
    return shd;
}

extern "C" sg_resource_state sgj_shader_create(void* slot, const sg_shader_desc* desc){
    return _sg_create_shader(static_cast<_sg_shader_t*>(slot), desc);
}

extern "C" void sgj_shader_publish(void* slot, sg_shader shd_id, const sg_shader_desc* desc, sg_resource_state state){
    _sg_shader_t* shd = static_cast<_sg_shader_t*>(slot);
    SOKOL_ASSERT(shd && shd->slot.id == shd_id.id && shd->slot.state == SG_RESOURCESTATE_ALLOC);
    SOKOL_ASSERT((state == SG_RESOURCESTATE_VALID) || (state == SG_RESOURCESTATE_FAILED));
    shd->slot.state = state;
    _sg_resource_stats_inc(shaders.inited);
    _SG_TRACE_ARGS(init_shader, shd_id, desc);
}

extern "C" void* sgj_pipeline_prepare(sg_pipeline pip_id, sg_pipeline_desc* desc){
    SOKOL_ASSERT(_sg.valid && desc);
    *desc = _sg_pipeline_desc_defaults(desc);
    _sg_pipeline_t* pip = _sg_lookup_pipeline(pip_id.id);
    if (!pip)
        return nullptr;
    if (pip->slot.state != SG_RESOURCESTATE_ALLOC){
        _SG_ERROR(INIT_PIPELINE_INVALID_STATE);
        return nullptr;
    }
    if (_sg_validate_pipeline_desc(desc)){
        _sg_shader_t* shd = _sg_lookup_shader(desc->shader.id);
        if (shd && (shd->slot.state == SG_RESOURCESTATE_VALID)){
            _sg_pipeline_common_init(&pip->cmn, desc, shd);
            return pip;
        }
    }
    pip->slot.state = SG_RESOURCESTATE_FAILED;
    _sg_resource_stats_inc(pipelines.inited);
    _SG_TRACE_ARGS(init_pipeline, pip_id, desc);
    return nullptr;
}

extern "C" sg_resource_state sgj_pipeline_create(void* slot, const sg_pipeline_desc* desc){
    return _sg_create_pipeline(static_cast<_sg_pipeline_t*>(slot), desc);
}

extern "C" void sgj_pipeline_publish(void* slot, sg_pipeline pip_id, const sg_pipeline_desc* desc, sg_resource_state state){
    _sg_pipeline_t* pip = static_cast<_sg_pipeline_t*>(slot);
    SOKOL_ASSERT(pip && pip->slot.id == pip_id.id && pip->slot.state == SG_RESOURCESTATE_ALLOC);
    SOKOL_ASSERT((state == SG_RESOURCESTATE_VALID) || (state == SG_RESOURCESTATE_FAILED));
    pip->slot.state = state;
    _sg_resource_stats_inc(pipelines.inited);
    _SG_TRACE_ARGS(init_pipeline, pip_id, desc);
}

// ------------------------------------------------------------------ GL family

#if defined(_SOKOL_ANY_GL)

namespace {
    uint64_t sgjFnv(uint64_t h, const void* data, size_t size){
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++){
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
        return h;
    }
    template<typename T>
    uint64_t sgjMix(uint64_t h, T value){
        return sgjFnv(h, &value, sizeof(value));
    }

    // a shader object's log, as _sg_gl_compile_shader reports it
    void sgjLogShader(GLuint shader){
        GLint status = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status)
            return;
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        _SG_ERROR(GL_SHADER_COMPILATION_FAILED);
        if (length > 0){
            GLchar* text = (GLchar*)_sg_malloc((size_t)length);
            glGetShaderInfoLog(shader, length, &length, text);
            _SG_LOGMSG(GL_SHADER_COMPILATION_FAILED, text);
            _sg_free(text);
        }
    }

    GLuint sgjCompile(GLenum stage, const char* source){
        GLuint shader = glCreateShader(stage);
        glShaderSource(shader, 1, &source, 0);
        glCompileShader(shader);
        return shader;
    }
}

extern "C" bool sgj_gl_issue(const sg_shader_desc* desc, sgj_gl_program* out){
    std::memset(out, 0, sizeof(*out));
    if (!desc->vertex_func.source || !desc->fragment_func.source)
        return false;
    out->vertex = sgjCompile(GL_VERTEX_SHADER, desc->vertex_func.source);
    out->fragment = sgjCompile(GL_FRAGMENT_SHADER, desc->fragment_func.source);
    out->program = glCreateProgram();
    glAttachShader(out->program, out->vertex);
    glAttachShader(out->program, out->fragment);
    glLinkProgram(out->program);
    _SG_GL_CHECK_ERROR();
    return true;
}

extern "C" bool sgj_gl_is_complete(const sgj_gl_program* prog){
    if (!sgjCaps.gl_parallel_compile)
        return true;
    GLint done = 0;
    glGetProgramiv(prog->program, GL_COMPLETION_STATUS_KHR, &done);
    return done != 0;
}

extern "C" void sgj_gl_discard(sgj_gl_program* prog){
    if (prog->vertex) glDeleteShader(prog->vertex);
    if (prog->fragment) glDeleteShader(prog->fragment);
    if (prog->program) glDeleteProgram(prog->program);
    std::memset(prog, 0, sizeof(*prog));
}

extern "C" sg_resource_state sgj_gl_adopt(void* slot, const sg_shader_desc* desc, sgj_gl_program* prog){
    _sg_shader_t* shd = static_cast<_sg_shader_t*>(slot);
    GLint linked = 0;
    glGetProgramiv(prog->program, GL_LINK_STATUS, &linked);
    if (!linked || !_sg_gl_ensure_glsl_bindslot_ranges(desc)){
        if (!linked){
            sgjLogShader(prog->vertex);
            sgjLogShader(prog->fragment);
            GLint length = 0;
            glGetProgramiv(prog->program, GL_INFO_LOG_LENGTH, &length);
            _SG_ERROR(GL_SHADER_LINKING_FAILED);
            if (length > 0){
                GLchar* text = (GLchar*)_sg_malloc((size_t)length);
                glGetProgramInfoLog(prog->program, length, &length, text);
                _SG_LOGMSG(GL_SHADER_LINKING_FAILED, text);
                _sg_free(text);
            }
        }
        sgj_gl_discard(prog);
        return SG_RESOURCESTATE_FAILED;
    }
    // flagged for deletion: they go with the program
    glDeleteShader(prog->vertex);
    glDeleteShader(prog->fragment);
    shd->gl.prog = prog->program;   // adopted by _sg_gl_create_shader (PATCHES.md)
    std::memset(prog, 0, sizeof(*prog));
    return _sg_gl_create_shader(shd, desc);
}

extern "C" void sgj_gl_finish(void){
    glFinish();
}

// ---- warm draws

namespace {
    struct SgjWarmTarget {
        int colorCount = 0;
        sg_pixel_format colors[SG_MAX_COLOR_ATTACHMENTS] = {};
        sg_pixel_format depth = SG_PIXELFORMAT_NONE;
        int samples = 1;
        GLuint fbo = 0;
        GLuint buffers[SG_MAX_COLOR_ATTACHMENTS + 1] = {};
        bool complete = false;
    };

    struct SgjWarmTexture {
        GLenum target = 0;
        sg_pixel_format format = SG_PIXELFORMAT_NONE;
        int samples = 1;
        GLuint texture = 0;
    };
    struct SgjWarmSampler {
        sgj_gl_warm_sampler state = {};
        GLuint sampler = 0;
    };

    // the worker context's own objects (VAOs and FBOs are never shared between contexts; its
    // textures and samplers stand for the render thread's, which it never touches)
    struct SgjWarmObjects {
        GLuint vao = 0;
        GLuint vertices = 0;
        SgjWarmTarget targets[32];
        int targetCount = 0;
        std::vector<SgjWarmTexture> textures;
        std::vector<SgjWarmSampler> samplers;
    };
    SgjWarmObjects sgjWarm;

    // zeros for a 1x1 texel (a 4x4 block when compressed), every face or layer
    const void* sgjZeros(){
        static const std::vector<uint8_t> zeros(4096, 0);
        return zeros.data();
    }

    // a complete 1x1 texture of that target, format and sample count; 0 when this GL cannot make
    // one (the unit is left unbound: that variant is compiled at the render thread's first draw)
    GLuint sgjWarmTexture(GLenum target, sg_pixel_format format, int samples){
        for (const SgjWarmTexture& t : sgjWarm.textures){
            if (t.target == target && t.format == format && t.samples == samples)
                return t.texture;
        }
        SgjWarmTexture made;
        made.target = target;
        made.format = format;
        made.samples = samples;
        const bool compressed = _sg_is_compressed_pixel_format(format);
        const GLsizei size = compressed ? 4 : 1;
        const GLenum internal = _sg_gl_teximage_internal_format(format);
        const GLsizei bytes = (GLsizei)_sg_surface_pitch(format, size, size, 1);
        glGenTextures(1, &made.texture);
        glBindTexture(target, made.texture);
        if (samples > 1){
            #if defined(SOKOL_GLCORE) && !defined(__APPLE__)
                if (target == GL_TEXTURE_2D_MULTISAMPLE)
                    glTexImage2DMultisample(target, samples, internal, 1, 1, GL_TRUE);
            #endif
        }else if (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP){
            const int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
            for (int face = 0; face < faces; face++){
                const GLenum faceTarget = target == GL_TEXTURE_CUBE_MAP ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : target;
                if (compressed){
                    glCompressedTexImage2D(faceTarget, 0, internal, size, size, 0, bytes, sgjZeros());
                }else{
                    glTexImage2D(faceTarget, 0, (GLint)internal, size, size, 0, _sg_gl_teximage_format(format), _sg_gl_teximage_type(format), sgjZeros());
                }
            }
        }else if (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY){
            if (compressed){
                glCompressedTexImage3D(target, 0, internal, size, size, 1, 0, bytes, sgjZeros());
            }else{
                glTexImage3D(target, 0, (GLint)internal, size, size, 1, 0, _sg_gl_teximage_format(format), _sg_gl_teximage_type(format), sgjZeros());
            }
        }
        if (samples <= 1){
            glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
        }
        glBindTexture(target, 0);
        if (glGetError() != GL_NO_ERROR){
            glDeleteTextures(1, &made.texture);
            made.texture = 0;
            while (glGetError() != GL_NO_ERROR){
            }
        }
        sgjWarm.textures.push_back(made);
        return made.texture;
    }

    GLuint sgjWarmSampler(const sgj_gl_warm_sampler& state){
        for (const SgjWarmSampler& s : sgjWarm.samplers){
            if (std::memcmp(&s.state, &state, sizeof(state)) == 0)
                return s.sampler;
        }
        SgjWarmSampler made;
        made.state = state;
        glGenSamplers(1, &made.sampler);
        glSamplerParameteri(made.sampler, GL_TEXTURE_MIN_FILTER, (GLint)state.min_filter);
        glSamplerParameteri(made.sampler, GL_TEXTURE_MAG_FILTER, (GLint)state.mag_filter);
        glSamplerParameteri(made.sampler, GL_TEXTURE_WRAP_S, (GLint)state.wrap_s);
        glSamplerParameteri(made.sampler, GL_TEXTURE_WRAP_T, (GLint)state.wrap_t);
        glSamplerParameteri(made.sampler, GL_TEXTURE_WRAP_R, (GLint)state.wrap_r);
        if (state.compare != 0){
            glSamplerParameteri(made.sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glSamplerParameteri(made.sampler, GL_TEXTURE_COMPARE_FUNC, (GLint)state.compare);
        }else{
            glSamplerParameteri(made.sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
        if (_sg.gl.ext_anisotropic && state.max_anisotropy > 1){
            glSamplerParameteri(made.sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, (GLint)state.max_anisotropy);
        }
        sgjWarm.samplers.push_back(made);
        return made.sampler;
    }

    // vertex attributes read at most stride * 2 + offset + 16 bytes for three vertices
    constexpr GLsizeiptr SGJ_WARM_VERTEX_BYTES = 8192;

    GLuint sgjRenderbuffer(sg_pixel_format format, int samples){
        GLuint buffer = 0;
        glGenRenderbuffers(1, &buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, buffer);
        const GLenum internal = _sg_gl_teximage_internal_format(format);
        if (samples > 1){
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, 1, 1);
        }else{
            glRenderbufferStorage(GL_RENDERBUFFER, internal, 1, 1);
        }
        return buffer;
    }

    SgjWarmTarget* sgjWarmTarget(const sgj_gl_warm_draw* d){
        for (int i = 0; i < sgjWarm.targetCount; i++){
            SgjWarmTarget& t = sgjWarm.targets[i];
            if (t.colorCount == d->color_count && t.depth == d->depth_format && t.samples == d->sample_count &&
                    std::memcmp(t.colors, d->color_formats, sizeof(t.colors)) == 0)
                return &t;
        }
        if (sgjWarm.targetCount == (int)(sizeof(sgjWarm.targets) / sizeof(sgjWarm.targets[0]))){
            // the oldest goes: a format set that rare is made again when it comes back
            SgjWarmTarget& old = sgjWarm.targets[0];
            glDeleteFramebuffers(1, &old.fbo);
            glDeleteRenderbuffers(SG_MAX_COLOR_ATTACHMENTS + 1, old.buffers);
            std::memmove(&sgjWarm.targets[0], &sgjWarm.targets[1], sizeof(SgjWarmTarget) * (size_t)(sgjWarm.targetCount - 1));
            sgjWarm.targetCount--;
        }
        SgjWarmTarget& t = sgjWarm.targets[sgjWarm.targetCount++];
        t = SgjWarmTarget();
        t.colorCount = d->color_count;
        std::memcpy(t.colors, d->color_formats, sizeof(t.colors));
        t.depth = d->depth_format;
        t.samples = d->sample_count > 1 ? d->sample_count : 1;
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        GLenum drawBuffers[SG_MAX_COLOR_ATTACHMENTS] = {};
        for (int i = 0; i < t.colorCount && i < SG_MAX_COLOR_ATTACHMENTS; i++){
            t.buffers[i] = sgjRenderbuffer(t.colors[i], t.samples);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, (GLenum)(GL_COLOR_ATTACHMENT0 + i), GL_RENDERBUFFER, t.buffers[i]);
            drawBuffers[i] = (GLenum)(GL_COLOR_ATTACHMENT0 + i);
        }
        if (t.depth != SG_PIXELFORMAT_NONE){
            t.buffers[SG_MAX_COLOR_ATTACHMENTS] = sgjRenderbuffer(t.depth, t.samples);
            const GLenum attachment = (t.depth == SG_PIXELFORMAT_DEPTH_STENCIL) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, t.buffers[SG_MAX_COLOR_ATTACHMENTS]);
        }
        if (t.colorCount > 0){
            glDrawBuffers(t.colorCount, drawBuffers);
        }else{
            const GLenum none = GL_NONE;
            glDrawBuffers(1, &none);
        }
        t.complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        return &t;
    }

    void sgjWarmState(const sgj_gl_warm_draw* d){
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(_sg_gl_compare_func(d->depth.compare));
        glDepthMask(d->depth.write_enabled ? GL_TRUE : GL_FALSE);
        const bool offset = d->depth.bias != 0.0f || d->depth.bias_slope_scale != 0.0f;
        if (offset){
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(d->depth.bias_slope_scale, d->depth.bias);
        }else{
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        if (d->stencil.enabled){
            glEnable(GL_STENCIL_TEST);
            glStencilMask(d->stencil.write_mask);
            for (int i = 0; i < 2; i++){
                const sg_stencil_face_state& face = i == 0 ? d->stencil.front : d->stencil.back;
                const GLenum glFace = i == 0 ? GL_FRONT : GL_BACK;
                glStencilFuncSeparate(glFace, _sg_gl_compare_func(face.compare), d->stencil.ref, d->stencil.read_mask);
                glStencilOpSeparate(glFace, _sg_gl_stencil_op(face.fail_op), _sg_gl_stencil_op(face.depth_fail_op), _sg_gl_stencil_op(face.pass_op));
            }
        }else{
            glDisable(GL_STENCIL_TEST);
        }
        if (d->blend.enabled){
            glEnable(GL_BLEND);
            glBlendFuncSeparate(_sg_gl_blend_factor(d->blend.src_factor_rgb), _sg_gl_blend_factor(d->blend.dst_factor_rgb),
                                _sg_gl_blend_factor(d->blend.src_factor_alpha), _sg_gl_blend_factor(d->blend.dst_factor_alpha));
            glBlendEquationSeparate(_sg_gl_blend_op(d->blend.op_rgb), _sg_gl_blend_op(d->blend.op_alpha));
        }else{
            glDisable(GL_BLEND);
        }
        const sg_color_mask mask = d->color_mask;
        glColorMask((mask & SG_COLORMASK_R) != 0, (mask & SG_COLORMASK_G) != 0, (mask & SG_COLORMASK_B) != 0, (mask & SG_COLORMASK_A) != 0);
        if (d->cull_mode == SG_CULLMODE_NONE){
            glDisable(GL_CULL_FACE);
        }else{
            glEnable(GL_CULL_FACE);
            glCullFace(d->cull_mode == SG_CULLMODE_FRONT ? GL_FRONT : GL_BACK);
        }
        glFrontFace(d->face_winding == SG_FACEWINDING_CW ? GL_CW : GL_CCW);
        if (d->alpha_to_coverage){
            glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }else{
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }
        #if defined(SOKOL_GLCORE)
            if (d->sample_count > 1){
                glEnable(GL_MULTISAMPLE);
            }else{
                glDisable(GL_MULTISAMPLE);
            }
            glEnable(GL_PROGRAM_POINT_SIZE);
            bool srgb = false;
            for (int i = 0; i < d->color_count && i < SG_MAX_COLOR_ATTACHMENTS; i++){
                srgb = srgb || d->color_formats[i] == SG_PIXELFORMAT_SRGB8A8;
            }
            if (srgb){
                glEnable(GL_FRAMEBUFFER_SRGB);
            }else{
                glDisable(GL_FRAMEBUFFER_SRGB);
            }
        #endif
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 1, 1);
        glViewport(0, 0, 1, 1);
    }
}

extern "C" bool sgj_gl_warm_describe(sg_pipeline pip_id, const sg_bindings* bindings, sgj_gl_warm_draw* out, uint64_t* key){
    std::memset(out, 0, sizeof(*out));
    const _sg_pipeline_t* pip = _sg_lookup_pipeline(pip_id.id);
    if (!pip || pip->slot.state != SG_RESOURCESTATE_VALID || pip->cmn.is_compute || !_sg_shader_ref_alive(&pip->cmn.shader))
        return false;
    const _sg_shader_t* shd = _sg_shader_ref_ptr(&pip->cmn.shader);
    if (!shd->gl.prog)
        return false;

    out->program = shd->gl.prog;
    out->primitive = _sg_gl_primitive_type(pip->gl.primitive_type);
    for (int i = 0; i < SG_MAX_VERTEX_ATTRIBUTES; i++){
        const _sg_gl_attr_t& attr = pip->gl.attrs[i];
        sgj_gl_warm_attr& w = out->attrs[i];
        if (attr.vb_index < 0)
            continue;
        w.enabled = 1;
        w.divisor = attr.divisor > 0 ? attr.divisor : 0;
        w.size = attr.size;
        w.normalized = attr.normalized;
        w.integer = (attr.base_type == SG_SHADERATTRBASETYPE_SINT || attr.base_type == SG_SHADERATTRBASETYPE_UINT) ? 1 : 0;
        w.stride = attr.stride;
        w.offset = attr.offset;
        w.type = attr.type;
    }
    out->depth = pip->gl.depth;
    out->stencil = pip->gl.stencil;
    out->blend = pip->gl.blend;
    out->color_mask = pip->gl.color_write_mask[0];
    out->cull_mode = pip->gl.cull_mode;
    out->face_winding = pip->gl.face_winding;
    out->alpha_to_coverage = pip->gl.alpha_to_coverage_enabled;
    out->color_count = pip->cmn.color_count;
    for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++){
        out->color_formats[i] = i < pip->cmn.color_count ? pip->cmn.colors[i].pixel_format : SG_PIXELFORMAT_NONE;
    }
    out->depth_format = pip->cmn.depth.pixel_format;
    out->sample_count = pip->cmn.sample_count > 1 ? pip->cmn.sample_count : 1;

    // the variant: the program and its state, without the texture names (they change, the
    // formats and sampler states the driver compiles for do not)
    uint64_t h = 14695981039346656037ull;
    h = sgjMix(h, out->program);
    h = sgjMix(h, out->primitive);
    for (const sgj_gl_warm_attr& w : out->attrs){
        h = sgjMix(h, w.enabled);
        if (w.enabled){
            h = sgjMix(h, w.divisor); h = sgjMix(h, w.size); h = sgjMix(h, w.normalized); h = sgjMix(h, w.integer);
            h = sgjMix(h, w.stride); h = sgjMix(h, w.offset); h = sgjMix(h, w.type);
        }
    }
    h = sgjMix(h, (int)out->depth.compare); h = sgjMix(h, out->depth.write_enabled);
    h = sgjMix(h, out->depth.bias != 0.0f || out->depth.bias_slope_scale != 0.0f);
    h = sgjMix(h, out->stencil.enabled);
    if (out->stencil.enabled){
        for (const sg_stencil_face_state* face : {&out->stencil.front, &out->stencil.back}){
            h = sgjMix(h, (int)face->compare); h = sgjMix(h, (int)face->fail_op);
            h = sgjMix(h, (int)face->depth_fail_op); h = sgjMix(h, (int)face->pass_op);
        }
        h = sgjMix(h, out->stencil.read_mask); h = sgjMix(h, out->stencil.write_mask);
    }
    h = sgjMix(h, out->blend.enabled);
    if (out->blend.enabled){
        h = sgjMix(h, (int)out->blend.src_factor_rgb); h = sgjMix(h, (int)out->blend.dst_factor_rgb); h = sgjMix(h, (int)out->blend.op_rgb);
        h = sgjMix(h, (int)out->blend.src_factor_alpha); h = sgjMix(h, (int)out->blend.dst_factor_alpha); h = sgjMix(h, (int)out->blend.op_alpha);
    }
    h = sgjMix(h, (int)out->color_mask); h = sgjMix(h, (int)out->cull_mode); h = sgjMix(h, (int)out->face_winding);
    h = sgjMix(h, out->alpha_to_coverage); h = sgjMix(h, out->color_count);
    for (sg_pixel_format format : out->color_formats) h = sgjMix(h, (int)format);
    h = sgjMix(h, (int)out->depth_format); h = sgjMix(h, out->sample_count);

    for (size_t i = 0; i < SG_MAX_TEXTURE_SAMPLER_PAIRS; i++){
        const _sg_shader_texture_sampler_t& pair = shd->cmn.texture_samplers[i];
        if (pair.stage == SG_SHADERSTAGE_NONE)
            continue;
        const int unit = shd->gl.tex_slot[i];
        if (unit < 0 || unit >= SGJ_GL_WARM_MAX_UNITS)
            continue;
        const _sg_view_t* view = _sg_lookup_view(bindings->views[pair.view_slot].id);
        const _sg_sampler_t* smp = _sg_lookup_sampler(bindings->samplers[pair.sampler_slot].id);
        if (!view || view->slot.state != SG_RESOURCESTATE_VALID || !_sg_image_ref_alive(&view->cmn.img.ref) ||
                !smp || smp->slot.state != SG_RESOURCESTATE_VALID)
            return false;
        const _sg_image_t* img = _sg_image_ref_ptr(&view->cmn.img.ref);
        out->unit_target[unit] = img->gl.target;
        out->unit_format[unit] = img->cmn.pixel_format;
        out->unit_samples[unit] = img->cmn.sample_count > 1 ? img->cmn.sample_count : 1;
        sgj_gl_warm_sampler& warmSampler = out->unit_sampler[unit];
        warmSampler.min_filter = _sg_gl_min_filter(smp->cmn.min_filter, smp->cmn.mipmap_filter);
        warmSampler.mag_filter = _sg_gl_mag_filter(smp->cmn.mag_filter);
        warmSampler.wrap_s = _sg_gl_wrap(smp->cmn.wrap_u);
        warmSampler.wrap_t = _sg_gl_wrap(smp->cmn.wrap_v);
        warmSampler.wrap_r = _sg_gl_wrap(smp->cmn.wrap_w);
        warmSampler.compare = smp->cmn.compare != SG_COMPAREFUNC_NEVER ? _sg_gl_compare_func(smp->cmn.compare) : 0;
        warmSampler.max_anisotropy = smp->cmn.max_anisotropy;
        if (unit + 1 > out->unit_count)
            out->unit_count = unit + 1;
        h = sgjMix(h, unit); h = sgjMix(h, img->gl.target); h = sgjMix(h, (int)img->cmn.pixel_format);
        h = sgjMix(h, (int)img->cmn.type); h = sgjMix(h, img->cmn.sample_count > 1);
        const _sg_sampler_common_t& s = smp->cmn;
        h = sgjMix(h, (int)s.min_filter); h = sgjMix(h, (int)s.mag_filter); h = sgjMix(h, (int)s.mipmap_filter);
        h = sgjMix(h, (int)s.wrap_u); h = sgjMix(h, (int)s.wrap_v); h = sgjMix(h, (int)s.wrap_w);
        h = sgjMix(h, (int)s.compare); h = sgjMix(h, s.max_anisotropy);
    }
    *key = h ? h : 1;
    return true;
}

extern "C" void sgj_gl_warm_run(const sgj_gl_warm_draw* d){
    if (!sgjWarm.vao){
        glGenVertexArrays(1, &sgjWarm.vao);
        glGenBuffers(1, &sgjWarm.vertices);
        glBindBuffer(GL_ARRAY_BUFFER, sgjWarm.vertices);
        void* zeros = _sg_malloc_clear((size_t)SGJ_WARM_VERTEX_BYTES);
        glBufferData(GL_ARRAY_BUFFER, SGJ_WARM_VERTEX_BYTES, zeros, GL_STATIC_DRAW);
        _sg_free(zeros);
    }
    SgjWarmTarget* target = sgjWarmTarget(d);
    if (target->complete){
        glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
        glBindVertexArray(sgjWarm.vao);
        glBindBuffer(GL_ARRAY_BUFFER, sgjWarm.vertices);
        for (GLuint i = 0; i < SG_MAX_VERTEX_ATTRIBUTES; i++){
            const sgj_gl_warm_attr& a = d->attrs[i];
            if (!a.enabled){
                glDisableVertexAttribArray(i);
                continue;
            }
            const GLvoid* offset = (const GLvoid*)(GLintptr)(a.offset < 4096 ? a.offset : 0);
            if (a.integer){
                glVertexAttribIPointer(i, a.size, a.type, a.stride, offset);
            }else{
                glVertexAttribPointer(i, a.size, a.type, a.normalized ? GL_TRUE : GL_FALSE, a.stride, offset);
            }
            glVertexAttribDivisor(i, (GLuint)a.divisor);
            glEnableVertexAttribArray(i);
        }
        for (int u = 0; u < d->unit_count; u++){
            if (!d->unit_target[u])
                continue;
            const GLuint texture = sgjWarmTexture(d->unit_target[u], d->unit_format[u], d->unit_samples[u]);
            if (!texture)
                continue;
            glActiveTexture((GLenum)(GL_TEXTURE0 + u));
            glBindTexture(d->unit_target[u], texture);
            glBindSampler((GLuint)u, d->unit_samples[u] > 1 ? 0 : sgjWarmSampler(d->unit_sampler[u]));
        }
        sgjWarmState(d);
        glUseProgram(d->program);
        glDrawArraysInstanced(d->primitive, 0, 3, 1);
        glFinish();
        for (int u = 0; u < d->unit_count; u++){
            if (!d->unit_target[u])
                continue;
            glActiveTexture((GLenum)(GL_TEXTURE0 + u));
            glBindTexture(d->unit_target[u], 0);
            glBindSampler((GLuint)u, 0);
        }
        glUseProgram(0);
    }
    // a program the render thread deleted meanwhile only makes this draw useless
    while (glGetError() != GL_NO_ERROR){
    }
}

extern "C" uint32_t sgj_gl_shader_program(sg_shader shd_id){
    const _sg_shader_t* shd = _sg_lookup_shader(shd_id.id);
    return (shd && shd->slot.state == SG_RESOURCESTATE_VALID) ? shd->gl.prog : 0;
}

extern "C" void sgj_gl_warm_release(void){
    for (int i = 0; i < sgjWarm.targetCount; i++){
        glDeleteFramebuffers(1, &sgjWarm.targets[i].fbo);
        glDeleteRenderbuffers(SG_MAX_COLOR_ATTACHMENTS + 1, sgjWarm.targets[i].buffers);
    }
    for (const SgjWarmTexture& t : sgjWarm.textures){
        if (t.texture) glDeleteTextures(1, &t.texture);
    }
    for (const SgjWarmSampler& smp : sgjWarm.samplers){
        glDeleteSamplers(1, &smp.sampler);
    }
    if (sgjWarm.vao) glDeleteVertexArrays(1, &sgjWarm.vao);
    if (sgjWarm.vertices) glDeleteBuffers(1, &sgjWarm.vertices);
    sgjWarm = SgjWarmObjects();
    glFinish();
}

#else

extern "C" bool sgj_gl_issue(const sg_shader_desc*, sgj_gl_program* out){ std::memset(out, 0, sizeof(*out)); return false; }
extern "C" bool sgj_gl_is_complete(const sgj_gl_program*){ return true; }
extern "C" sg_resource_state sgj_gl_adopt(void*, const sg_shader_desc*, sgj_gl_program*){ return SG_RESOURCESTATE_FAILED; }
extern "C" void sgj_gl_discard(sgj_gl_program* prog){ std::memset(prog, 0, sizeof(*prog)); }
extern "C" void sgj_gl_finish(void){}
extern "C" bool sgj_gl_warm_describe(sg_pipeline, const sg_bindings*, sgj_gl_warm_draw* out, uint64_t* key){ std::memset(out, 0, sizeof(*out)); *key = 0; return false; }
extern "C" void sgj_gl_warm_run(const sgj_gl_warm_draw*){}
extern "C" void sgj_gl_warm_release(void){}
extern "C" uint32_t sgj_gl_shader_program(sg_shader){ return 0; }

#endif

// ------------------------------------------------------------------ Vulkan pipeline cache

#if defined(SOKOL_VULKAN)

extern "C" bool sgj_vk_device_identity_query(sgj_vk_device_identity* out){
    std::memset(out, 0, sizeof(*out));
    if (!_sg.valid || !_sg.vk.dev)
        return false;
    const VkPhysicalDeviceProperties& props = _sg.vk.dev_props.properties;
    out->vendor_id = props.vendorID;
    out->device_id = props.deviceID;
    out->driver_version = props.driverVersion;
    std::memcpy(out->uuid, props.pipelineCacheUUID, sizeof(out->uuid));
    return true;
}

extern "C" bool sgj_vk_pipeline_cache_create(const void* data, size_t size){
    if (!_sg.valid || !_sg.vk.dev)
        return false;
    if (_sgj_vk_pipeline_cache != VK_NULL_HANDLE)
        return true;
    VkPipelineCacheCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data ? size : 0;
    info.pInitialData = data;
    if (vkCreatePipelineCache(_sg.vk.dev, &info, nullptr, &_sgj_vk_pipeline_cache) == VK_SUCCESS)
        return true;
    // data the driver refuses (another version than the header said): an empty cache
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    _sgj_vk_pipeline_cache = VK_NULL_HANDLE;
    return vkCreatePipelineCache(_sg.vk.dev, &info, nullptr, &_sgj_vk_pipeline_cache) == VK_SUCCESS;
}

extern "C" size_t sgj_vk_pipeline_cache_data(void* dst, size_t capacity){
    if (_sgj_vk_pipeline_cache == VK_NULL_HANDLE)
        return 0;
    size_t size = dst ? capacity : 0;
    const VkResult result = vkGetPipelineCacheData(_sg.vk.dev, _sgj_vk_pipeline_cache, &size, dst);
    if (result != VK_SUCCESS && !(dst == nullptr && result == VK_INCOMPLETE))
        return 0;
    return size;
}

extern "C" void sgj_vk_pipeline_cache_destroy(void){
    if (_sgj_vk_pipeline_cache != VK_NULL_HANDLE && _sg.vk.dev)
        vkDestroyPipelineCache(_sg.vk.dev, _sgj_vk_pipeline_cache, nullptr);
    _sgj_vk_pipeline_cache = VK_NULL_HANDLE;
}

#else

extern "C" bool sgj_vk_device_identity_query(sgj_vk_device_identity* out){ std::memset(out, 0, sizeof(*out)); return false; }
extern "C" bool sgj_vk_pipeline_cache_create(const void*, size_t){ return false; }
extern "C" size_t sgj_vk_pipeline_cache_data(void*, size_t){ return 0; }
extern "C" void sgj_vk_pipeline_cache_destroy(void){}

#endif

// ------------------------------------------------------------------ Metal

#if defined(SOKOL_METAL)

namespace {
    enum class SgjMtlKind { Shader, Pipeline };

    struct SgjMtlJob {
        SgjMtlKind kind = SgjMtlKind::Shader;
        // shader: the libraries of the functions given as source (bytecode is not compiled)
        const sg_shader_function* functions[3] = {};
        id<MTLLibrary> libraries[3] = {};
        // pipeline
        id<MTLRenderPipelineState> state = nil;
        // the handlers still to run, and the first error one of them reported
        std::atomic<int> waiting{0};
        NSString* error = nil;
        std::atomic<bool> failed{false};
    };

    // the job the hooks in _sg_mtl_create_shader_func / _sg_mtl_create_pipeline serve
    // (render thread only): capturing a pipeline's descriptor, or giving what was created
    SgjMtlJob* sgjMtlCapture = nullptr;
    SgjMtlJob* sgjMtlFinish = nullptr;

    void sgjMtlDone(SgjMtlJob* job, NSError* err, bool failed){
        if (failed){
            // the first error wins; the render thread reads it after waiting reached 0
            if (!job->failed.exchange(true))
                job->error = err ? err.localizedDescription : @"creation failed";
        }
        job->waiting.fetch_sub(1, std::memory_order_acq_rel);
    }
}

static id<MTLLibrary> _sgj_mtl_prebuilt_library(const sg_shader_function* func){
    if (!sgjMtlFinish || sgjMtlFinish->kind != SgjMtlKind::Shader)
        return nil;
    for (int i = 0; i < 3; i++){
        if (sgjMtlFinish->functions[i] == func)
            return sgjMtlFinish->libraries[i];
    }
    return nil;
}

static bool _sgj_mtl_capture_pipeline(MTLRenderPipelineDescriptor* rp_desc){
    if (!sgjMtlCapture)
        return false;
    SgjMtlJob* job = sgjMtlCapture;
    job->waiting.store(1);
    [_sg.mtl.device newRenderPipelineStateWithDescriptor:rp_desc completionHandler:^(id<MTLRenderPipelineState> state, NSError* err){
        job->state = state;
        sgjMtlDone(job, err, state == nil);
    }];
    return true;
}

static id<MTLRenderPipelineState> _sgj_mtl_prebuilt_pipeline_state(void){
    if (!sgjMtlFinish || sgjMtlFinish->kind != SgjMtlKind::Pipeline)
        return nil;
    return sgjMtlFinish->state;
}

extern "C" void* sgj_mtl_shader_begin(void* slot, const sg_shader_desc* desc){
    (void)slot;
    SgjMtlJob* job = new SgjMtlJob();
    job->kind = SgjMtlKind::Shader;
    const sg_shader_function* functions[3] = {&desc->vertex_func, &desc->fragment_func, &desc->compute_func};
    int count = 0;
    for (int i = 0; i < 3; i++){
        if (functions[i]->source && !functions[i]->bytecode.ptr)
            count++;
    }
    job->waiting.store(count);
    for (int i = 0; i < 3; i++){
        const sg_shader_function* func = functions[i];
        job->functions[i] = func;
        if (!func->source || func->bytecode.ptr)
            continue;
        [_sg.mtl.device newLibraryWithSource:[NSString stringWithUTF8String:func->source] options:_sg_mtl_compile_options()
            completionHandler:^(id<MTLLibrary> library, NSError* err){
                job->libraries[i] = library;
                sgjMtlDone(job, err, library == nil);
            }];
    }
    return job;
}

extern "C" void* sgj_mtl_pipeline_begin(void* slot, const sg_pipeline_desc* desc){
    _sg_pipeline_t* pip = static_cast<_sg_pipeline_t*>(slot);
    if (pip->cmn.is_compute)
        return nullptr;
    SgjMtlJob* job = new SgjMtlJob();
    job->kind = SgjMtlKind::Pipeline;
    sgjMtlCapture = job;
    const sg_resource_state state = _sg_mtl_create_pipeline(pip, desc);
    sgjMtlCapture = nullptr;
    if (state != SG_RESOURCESTATE_ALLOC){
        // the function failed before its descriptor was complete (it said why)
        delete job;
        return nullptr;
    }
    return job;
}

extern "C" bool sgj_mtl_job_ready(void* job){
    return static_cast<SgjMtlJob*>(job)->waiting.load(std::memory_order_acquire) <= 0;
}

extern "C" sg_resource_state sgj_mtl_job_finish(void* opaque, void* slot, const void* desc){
    SgjMtlJob* job = static_cast<SgjMtlJob*>(opaque);
    if (job->failed.load()){
        if (job->kind == SgjMtlKind::Shader){
            _SG_ERROR(METAL_SHADER_COMPILATION_FAILED);
            _SG_LOGMSG(METAL_SHADER_COMPILATION_OUTPUT, [job->error UTF8String]);
        }else{
            _SG_ERROR(METAL_CREATE_RPS_FAILED);
            _SG_LOGMSG(METAL_CREATE_RPS_OUTPUT, [job->error UTF8String]);
        }
        return SG_RESOURCESTATE_FAILED;
    }
    sgjMtlFinish = job;
    sg_resource_state state;
    if (job->kind == SgjMtlKind::Shader){
        state = _sg_mtl_create_shader(static_cast<_sg_shader_t*>(slot), static_cast<const sg_shader_desc*>(desc));
    }else{
        state = _sg_mtl_create_pipeline(static_cast<_sg_pipeline_t*>(slot), static_cast<const sg_pipeline_desc*>(desc));
    }
    sgjMtlFinish = nullptr;
    return state;
}

extern "C" void sgj_mtl_job_free(void* job){
    delete static_cast<SgjMtlJob*>(job);
}

#else

extern "C" void* sgj_mtl_shader_begin(void*, const sg_shader_desc*){ return nullptr; }
extern "C" void* sgj_mtl_pipeline_begin(void*, const sg_pipeline_desc*){ return nullptr; }
extern "C" bool sgj_mtl_job_ready(void*){ return true; }
extern "C" sg_resource_state sgj_mtl_job_finish(void*, void*, const void*){ return SG_RESOURCESTATE_FAILED; }
extern "C" void sgj_mtl_job_free(void*){}

#endif
