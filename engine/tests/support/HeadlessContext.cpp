// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HeadlessContext.h"

#include "WindowLinux.h"

#if defined(SOKOL_VULKAN)
// after WindowLinux.h: the Xlib surface entry points come from <X11/Xlib.h>
#include "VulkanContext.h"
#else
#include <GL/glx.h>
#include "GlxWorkerContext.h"
#endif
#include <X11/Xutil.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

bool created = false;
std::string rendererName;

#if !defined(SOKOL_VULKAN)

using CreateContextAttribsProc = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

GLXFBConfig framebufferConfig = nullptr;
GLXContext glContext = nullptr;
bool glxContextError = false;

int recordGlxContextError(Display*, XErrorEvent*){
    glxContextError = true;
    return 0;
}

// as JasperEngineLinux.cpp selectVisual
bool selectVisual(XVisualInfo*& info){
    static const int attributes[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_STENCIL_SIZE, 8,
        None
    };
    Display* display = WindowLinux::display();
    int count = 0;
    GLXFBConfig* configs = glXChooseFBConfig(display, WindowLinux::screen(), attributes, &count);
    if (!configs || count == 0){
        if (configs) XFree(configs);
        std::fprintf(stderr, "HeadlessContext: no GLX framebuffer configuration\n");
        return false;
    }
    framebufferConfig = configs[0];
    XFree(configs);

    info = glXGetVisualFromFBConfig(display, framebufferConfig);
    if (!info){
        std::fprintf(stderr, "HeadlessContext: no X visual for the GLX configuration\n");
        return false;
    }
    return true;
}

// as JasperEngineLinux.cpp createContext, without the swap interval (never presented)
bool createGraphicsContext(){
    Display* display = WindowLinux::display();

    auto createContextAttribs = reinterpret_cast<CreateContextAttribsProc>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
    if (!createContextAttribs){
        std::fprintf(stderr, "HeadlessContext: GLX_ARB_create_context is required\n");
        return false;
    }

#if defined(SOKOL_GLES3)
    // the mobile and web backend in an OpenGL ES 3.2 context, as JasperEngineLinux.cpp makes it
    // (Mesa's llvmpipe; the linux-backends image)
    static const int attributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 2,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_ES2_PROFILE_BIT_EXT,
        None
    };
    const char* contextName = "OpenGL ES 3.2";
#else
    static const int attributes[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        GLX_CONTEXT_MINOR_VERSION_ARB, 1,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };
    const char* contextName = "OpenGL 4.1 core";
#endif

    XSync(display, False);
    glxContextError = false;
    XErrorHandler previousHandler = XSetErrorHandler(recordGlxContextError);
    glContext = createContextAttribs(display, framebufferConfig, nullptr, True, attributes);
    XSync(display, False);
    XSetErrorHandler(previousHandler);

    if (glxContextError || !glContext){
        if (glContext) glXDestroyContext(display, glContext);
        glContext = nullptr;
        std::fprintf(stderr, "HeadlessContext: could not create an %s context\n", contextName);
        return false;
    }
    if (!glXMakeCurrent(display, WindowLinux::handle(), glContext)){
        std::fprintf(stderr, "HeadlessContext: could not activate the OpenGL context\n");
        glXDestroyContext(display, glContext);
        glContext = nullptr;
        return false;
    }
    const GLubyte* renderer = glGetString(GL_RENDERER);
    rendererName = renderer ? reinterpret_cast<const char*>(renderer) : "";
    // as the player: the shader creation thread's context (spec S32)
    setGpuWorkerContextFactory([](){
        return GlxWorkerContext::create(WindowLinux::display(), framebufferConfig, glContext, attributes);
    });
    return true;
}

void destroyGraphicsContext(){
    setGpuWorkerContextFactory(nullptr);
    Display* display = WindowLinux::display();
    if (glContext && display){
        glXMakeCurrent(display, None, nullptr);
        glXDestroyContext(display, glContext);
    }
    glContext = nullptr;
    framebufferConfig = nullptr;
}

bool createWindow(WindowLinuxConfig& config){
    XVisualInfo* info = nullptr;
    if (!selectVisual(info))
        return false;
    config.visual = info->visual;
    config.depth = info->depth;
    const bool result = WindowLinux::create(config);
    XFree(info);
    return result;
}

#else

VkResult createWindowSurface(VkInstance instance, VkSurfaceKHR* surface){
    VkXlibSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.dpy = WindowLinux::display();
    surfaceInfo.window = WindowLinux::handle();
    return vkCreateXlibSurfaceKHR(instance, &surfaceInfo, nullptr, surface);
}

bool createGraphicsContext(){
    VulkanContextConfig config;
    config.surfaceExtension = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
    config.createSurface = createWindowSurface;
    config.vsync = false;
    if (!VulkanContext::create(config))
        return false;
    rendererName = VulkanContext::deviceName();
    return true;
}

void destroyGraphicsContext(){
    VulkanContext::destroy();
}

bool createWindow(WindowLinuxConfig& config){
    // a Vulkan surface works with any drawable: the screen's own visual
    config.visual = DefaultVisual(WindowLinux::display(), WindowLinux::screen());
    config.depth = DefaultDepth(WindowLinux::display(), WindowLinux::screen());
    return WindowLinux::create(config);
}

#endif

}

bool HeadlessContext::create(int width, int height){
    if (created)
        return true;

    if (!WindowLinux::openDisplay()){
        std::fprintf(stderr, "HeadlessContext: no X display (run the tests under xvfb-run)\n");
        return false;
    }

    WindowLinuxConfig config;
    config.title = "jasperengine-htmlui-tests";
    config.appId = "jasperengine-htmlui-tests";
    config.width = width;
    config.height = height;
    config.resizable = false;

    // the window is never mapped: rendering goes to offscreen framebuffers only
    if (!createWindow(config)){
        WindowLinux::destroy();
        return false;
    }

    if (!createGraphicsContext()){
        destroyGraphicsContext();
        WindowLinux::destroy();
        return false;
    }

    created = true;
    return true;
}

void HeadlessContext::destroy(){
    if (!created)
        return;

#if defined(JASPERENGINE_SANITIZE_ADDRESS)
    // LeakSanitizer reports at exit; once the context and the display are gone Mesa has unloaded
    // its driver, and the driver's allocations show as "<unknown module>", which no suppression can
    // name. An ASan run keeps both until the process ends.
    return;
#endif
    destroyGraphicsContext();
    WindowLinux::destroy();
    created = false;
}

bool HeadlessContext::isCreated(){
    return created;
}

const char* HeadlessContext::getBackendName(){
#if defined(SOKOL_VULKAN)
    return "vulkan";
#elif defined(SOKOL_GLES3)
    return "gles3";
#else
    return "glcore";
#endif
}

const char* HeadlessContext::getRendererName(){
    return rendererName.c_str();
}
