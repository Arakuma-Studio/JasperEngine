// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GlPass.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 0
#include <GL/glcorearb.h>

#include <chrono>
#include <cstring>

namespace {

#define GL_FUNCTIONS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLGETACTIVEUNIFORMPROC, glGetActiveUniform) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLFINISHPROC, glFinish) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)

#define DECLARE(type, name) type gl_##name = nullptr;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

GLuint compileStage(GLenum stage, const std::string& source, std::string& error){
    const GLuint shader = gl_glCreateShader(stage);
    const char* text = source.c_str();
    gl_glShaderSource(shader, 1, &text, nullptr);
    gl_glCompileShader(shader);
    GLint ok = 0;
    gl_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok){
        char log[4096] = {};
        gl_glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        error = log;
        gl_glDeleteShader(shader);
        return 0;
    }
    return shader;
}

double since(std::chrono::steady_clock::time_point start){
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}

bool GlPass::create(std::string& error){
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display = getPlatformDisplay
        ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)){
        error = "no EGL display";
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)){
        error = "no desktop GL in EGL";
        return false;
    }
    const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configs = 0;
    eglChooseConfig(display, configAttributes, &config, 1, &configs);
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                                        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, configs > 0 ? config : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)){
        error = "no GL 4.1 core context";
        return false;
    }
#define LOAD(type, name) gl_##name = (type)eglGetProcAddress(#name);
    GL_FUNCTIONS(LOAD)
#undef LOAD
    GLuint vao = 0;
    gl_glGenVertexArrays(1, &vao);
    gl_glBindVertexArray(vao);
    gl_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    return true;
}

std::string GlPass::renderer() const {
    return (const char*)gl_glGetString(GL_RENDERER);
}

bool GlPass::render(const std::string& vertex, const std::string& fragment, const std::vector<float>& uniforms, int textureSize,
                    const std::vector<float>& textureRgba, int size, std::vector<float>& out, Timings& timings, std::string& error){
    auto start = std::chrono::steady_clock::now();
    const GLuint vs = compileStage(GL_VERTEX_SHADER, vertex, error);
    if (!vs)
        return false;
    const GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragment, error);
    if (!fs){
        gl_glDeleteShader(vs);
        return false;
    }
    const GLuint program = gl_glCreateProgram();
    gl_glAttachShader(program, vs);
    gl_glAttachShader(program, fs);
    gl_glLinkProgram(program);
    gl_glDeleteShader(vs);
    gl_glDeleteShader(fs);
    GLint linked = 0;
    gl_glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked){
        char log[4096] = {};
        gl_glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        error = log;
        gl_glDeleteProgram(program);
        return false;
    }
    timings.compileLinkMs = since(start);

    gl_glUseProgram(program);
    GLint uniformCount = 0;
    gl_glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    int unit = 0;
    std::vector<GLuint> created;
    for (GLint i = 0; i < uniformCount; i++){
        char name[256] = {};
        GLint elements = 0;
        GLenum type = 0;
        gl_glGetActiveUniform(program, (GLuint)i, sizeof(name) - 1, nullptr, &elements, &type, name);
        const GLint location = gl_glGetUniformLocation(program, name);
        if (type == GL_FLOAT_VEC4){
            std::vector<float> values(4 * (size_t)elements, 0.0f);
            std::memcpy(values.data(), uniforms.data(), std::min(values.size(), uniforms.size()) * sizeof(float));
            gl_glUniform4fv(location, elements, values.data());
        } else if (type == GL_SAMPLER_2D){
            GLuint texture = 0;
            gl_glGenTextures(1, &texture);
            created.push_back(texture);
            gl_glActiveTexture(GL_TEXTURE0 + unit);
            gl_glBindTexture(GL_TEXTURE_2D, texture);
            gl_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, textureSize, textureSize, 0, GL_RGBA, GL_FLOAT, textureRgba.data());
            gl_glGenerateMipmap(GL_TEXTURE_2D);
            gl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            gl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            gl_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            gl_glUniform1i(location, unit);
            unit++;
        }
    }
    GLuint color = 0, framebuffer = 0;
    gl_glGenTextures(1, &color);
    gl_glActiveTexture(GL_TEXTURE0 + 31);
    gl_glBindTexture(GL_TEXTURE_2D, color);
    gl_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, size, size, 0, GL_RGBA, GL_FLOAT, nullptr);
    gl_glGenFramebuffers(1, &framebuffer);
    gl_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (gl_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
        error = "incomplete framebuffer";
        return false;
    }
    gl_glViewport(0, 0, size, size);
    gl_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    for (int pass = 0; pass < 2; pass++){
        gl_glClear(GL_COLOR_BUFFER_BIT);
        start = std::chrono::steady_clock::now();
        gl_glDrawArrays(GL_TRIANGLES, 0, 3);
        gl_glFinish();
        (pass == 0 ? timings.firstDrawMs : timings.drawMs) = since(start);
    }
    out.resize((size_t)size * size * 4);
    gl_glReadPixels(0, 0, size, size, GL_RGBA, GL_FLOAT, out.data());

    gl_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_glDeleteFramebuffers(1, &framebuffer);
    gl_glDeleteTextures(1, &color);
    for (GLuint texture : created)
        gl_glDeleteTextures(1, &texture);
    gl_glUseProgram(0);
    gl_glDeleteProgram(program);
    return true;
}
