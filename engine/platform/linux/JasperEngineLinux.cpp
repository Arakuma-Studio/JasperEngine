// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineLinux.h"

#include "GamepadLinux.h"
#include "LinuxInputRouter.h"
#include "SystemLinux.h"
#include "WindowLinux.h"
#include "X11Visibility.h"
#if !defined(SOKOL_VULKAN)
#include "GlxWorkerContext.h"
#endif

#include "Engine.h"
#include "PlatformConfig.h"
#include "player/RunControl.h"
#include "util/FrameLimiter.h"

#if defined(SOKOL_VULKAN)
// After WindowLinux.h: the Xlib surface entry points come from <X11/Xlib.h>
#include "VulkanContext.h"
#else
#include <GL/glx.h>
#endif
#include <X11/Xutil.h>

#include <poll.h>
#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace jasperengine;

namespace {

#if !defined(SOKOL_VULKAN)
using CreateContextAttribsProc =
    GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
using SwapIntervalProc = void (*)(Display*, GLXDrawable, int);

GLXFBConfig framebufferConfig = nullptr;
GLXContext glContext = nullptr;
bool glxContextError = false;
#endif

GamepadLinux gamepads;
LinuxInputRouter inputRouter;
// Engine.targetFrameRate without vsync (Engine::getFramePacing().limiterFps)
FrameLimiter frameLimiter;
// a hidden window's update-only frames (Engine::getHiddenUpdateFps)
FrameLimiter hiddenClock;
// mapped and not _NET_WM_STATE_HIDDEN; a headless run is never shown and never hides
X11Visibility visibility;

bool shouldClose = false;
bool contextReady = false;

double monotonicSeconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return double(now.tv_sec) + double(now.tv_nsec) / 1.0e9;
}

// A hidden window has no present to pace the loop: it sleeps until an X event or the next update
void waitForEvents(double timeout) {
    Display* display = WindowLinux::display();
    if (timeout <= 0.0 || XPending(display)) return;

    pollfd connection{ConnectionNumber(display), POLLIN, 0};
    poll(&connection, 1, static_cast<int>(std::ceil(timeout * 1000.0)));
}

#if !defined(SOKOL_VULKAN)
template <typename T>
T glxProc(const char* name) {
    return reinterpret_cast<T>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
}

bool hasGlxExtension(Display* display, int screen, const char* name) {
    const char* extensions = glXQueryExtensionsString(display, screen);
    if (!extensions || !name || !*name || std::strchr(name, ' ')) return false;

    const size_t length = std::strlen(name);
    for (const char* match = extensions; (match = std::strstr(match, name));
         match += length) {
        const bool startsToken = match == extensions || match[-1] == ' ';
        const bool endsToken = match[length] == '\0' || match[length] == ' ';
        if (startsToken && endsToken) return true;
    }
    return false;
}

int recordGlxContextError(Display*, XErrorEvent*) {
    glxContextError = true;
    return 0;
}

// One configuration for the window, matching the editor backend's choice
bool selectVisual(XVisualInfo*& info) {
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
    GLXFBConfig* configs = glXChooseFBConfig(
        display, WindowLinux::screen(), attributes, &count);
    if (!configs || count == 0) {
        if (configs) XFree(configs);
        std::fprintf(stderr, "Error: No GLX framebuffer configuration available.\n");
        return false;
    }
    framebufferConfig = configs[0];
    XFree(configs);

    info = glXGetVisualFromFBConfig(display, framebufferConfig);
    if (!info) {
        std::fprintf(stderr, "Error: No X visual for the GLX configuration.\n");
        return false;
    }
    return true;
}

bool createContext() {
    Display* display = WindowLinux::display();
    const int screen = WindowLinux::screen();

    if (!hasGlxExtension(display, screen, "GLX_ARB_create_context")) {
        std::fprintf(stderr, "Error: GLX_ARB_create_context is required.\n");
        return false;
    }
    auto createContextAttribs =
        glxProc<CreateContextAttribsProc>("glXCreateContextAttribsARB");
    if (!createContextAttribs) {
        std::fprintf(stderr, "Error: GLX_ARB_create_context is required.\n");
        return false;
    }

#if defined(SOKOL_GLES3)
    // An OpenGL ES 3.2 context (sokol's GLES3 backend on Linux assumes 3.1+), to run
    // the mobile/web shaders and code paths on a desktop (Mesa provides it)
    if (!hasGlxExtension(display, screen, "GLX_EXT_create_context_es2_profile")) {
        std::fprintf(stderr, "Error: GLX_EXT_create_context_es2_profile is required.\n");
        return false;
    }
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

    // Some drivers report an unsupported profile with an X error instead of
    // returning null
    XSync(display, False);
    glxContextError = false;
    XErrorHandler previousHandler = XSetErrorHandler(recordGlxContextError);
    glContext = createContextAttribs(
        display, framebufferConfig, nullptr, True, attributes);
    XSync(display, False);
    XSetErrorHandler(previousHandler);

    if (glxContextError || !glContext) {
        if (glContext) glXDestroyContext(display, glContext);
        glContext = nullptr;
        std::fprintf(stderr, "Error: Could not create an %s context.\n", contextName);
        return false;
    }
    if (!glXMakeCurrent(display, WindowLinux::handle(), glContext)) {
        std::fprintf(stderr, "Error: Could not activate the OpenGL context.\n");
        glXDestroyContext(display, glContext);
        glContext = nullptr;
        return false;
    }

    // the shader creation thread's context (spec S32), with the same attributes
    static const int* workerAttributes = attributes;
    setGpuWorkerContextFactory([]() {
        return GlxWorkerContext::create(WindowLinux::display(), framebufferConfig, glContext, workerAttributes);
    });
    // the swap interval of the frame pacing (vSyncCount, from the project's VSync)
    contextReady = true;
    JasperEngineLinux::applySwapInterval(Engine::getFramePacing().swapInterval);
    return true;
}

void setSwapInterval(int interval) {
    Display* display = WindowLinux::display();
    if (!glContext || !display) return;
    if (!hasGlxExtension(display, WindowLinux::screen(), "GLX_EXT_swap_control")) return;
    const auto swapIntervalEXT = glxProc<SwapIntervalProc>("glXSwapIntervalEXT");
    if (swapIntervalEXT)
        swapIntervalEXT(display, WindowLinux::handle(), interval);
}

void destroyContext() {
    setGpuWorkerContextFactory(nullptr);
    contextReady = false;
    Display* display = WindowLinux::display();
    if (glContext && display) {
        glXMakeCurrent(display, None, nullptr);
        glXDestroyContext(display, glContext);
    }
    glContext = nullptr;
    framebufferConfig = nullptr;
}

void beginFrame() {
}

void endFrame() {
    glXSwapBuffers(WindowLinux::display(), WindowLinux::handle());
}

#else

VkResult createWindowSurface(VkInstance instance, VkSurfaceKHR* surface) {
    VkXlibSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.dpy = WindowLinux::display();
    surfaceInfo.window = WindowLinux::handle();
    return vkCreateXlibSurfaceKHR(instance, &surfaceInfo, nullptr, surface);
}

bool createContext() {
    VulkanContextConfig config;
    config.surfaceExtension = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
    config.createSurface = createWindowSurface;
    config.vsync = Engine::getFramePacing().swapInterval > 0;
    contextReady = VulkanContext::create(config);
    return contextReady;
}

void setSwapInterval(int interval) {
    // FIFO presents every vblank; longer intervals come from the limiter (getMaxSwapInterval)
    VulkanContext::setVSync(interval > 0);
}

void destroyContext() {
    contextReady = false;
    VulkanContext::destroy();
}

void beginFrame() {
    VulkanContext::beginFrame();
}

void endFrame() {
    VulkanContext::endFrame();
}

#endif

void processEvents() {
    Display* display = WindowLinux::display();
    while (XPending(display)) {
        XEvent event{};
        XNextEvent(display, &event);

        // the input method sees key events first (composition)
        if (inputRouter.filterEvent(event))
            continue;

        if (inputRouter.handleEvent(event))
            continue;

        if (WindowLinux::handleSelectionEvent(event))
            continue;

        if (WindowLinux::isCloseEvent(event)) {
            shouldClose = true;
            continue;
        }

        visibility.handleEvent(event);

        switch (event.type) {
            case ConfigureNotify:
                WindowLinux::updateSize(event.xconfigure.width,
                                        event.xconfigure.height);
                break;
            case FocusOut:
                // Match the other backends: a captured pointer is released when
                // the window loses focus so it can never be trapped.
                WindowLinux::releasePointerGrab();
                inputRouter.setFocus(false);
                // held keys and buttons come up, so none stays pressed (a grab's too: its
                // key ups go to the window manager)
                Engine::systemFocusChanged(false);
                // the grabs of a window manager's own shortcuts come and go: no real change
                if (event.xfocus.mode != NotifyGrab && event.xfocus.mode != NotifyUngrab)
                    Engine::systemFocus(false);
                break;
            case FocusIn:
                if (WindowLinux::isRelativeMouse())
                    WindowLinux::setMouseMode(MouseMode::CAPTURED);
                inputRouter.setFocus(true);
                Engine::systemFocusChanged(true);
                if (event.xfocus.mode != NotifyGrab && event.xfocus.mode != NotifyUngrab)
                    Engine::systemFocus(true);
                break;
            default:
                break;
        }
    }
}

}

void JasperEngineLinux::applySwapInterval(int interval) {
    if (contextReady) setSwapInterval(interval);
}

int JasperEngineLinux::init(int argc, char **argv) {
    const PlatformConfig& platform = platformConfig();
    Engine::systemInit(argc, argv, new SystemLinux(&inputRouter));

    if (!WindowLinux::openDisplay()) {
        // a CI machine or a container without a display server (R19)
        std::fprintf(stderr, "No X display (DISPLAY is unset or cannot be reached): run the game under "
                     "xvfb-run (xvfb-run ./game --headless ...) or set DISPLAY.\n");
        Engine::systemShutdown();
        return player::EXIT_NO_DISPLAY;
    }

    WindowLinuxConfig config;
    config.title = platform.title;
    config.appId = platform.appId;
    config.width = platform.width;
    config.height = platform.height;
    config.resizable = platform.resizable;

#if defined(SOKOL_VULKAN)
    // A Vulkan surface presents to any drawable, so the window keeps the
    // screen's own visual instead of one matched to a GLX configuration.
    config.visual = DefaultVisual(WindowLinux::display(), WindowLinux::screen());
    config.depth = DefaultDepth(WindowLinux::display(), WindowLinux::screen());

    const bool created = WindowLinux::create(config);
#else
    XVisualInfo* info = nullptr;
    if (!selectVisual(info)) {
        WindowLinux::destroy();
        return -1;
    }
    config.visual = info->visual;
    config.depth = info->depth;

    const bool created = WindowLinux::create(config);
    XFree(info);
#endif
    if (!created) {
        WindowLinux::destroy();
        return -1;
    }
    // --headless: never mapped; frames render offscreen all the same
    if (!platform.headless) {
        WindowLinux::show();
        // the window asked to be shown counts as shown until the server says otherwise
        visibility.attach(WindowLinux::display(), WindowLinux::handle(), true);
        inputRouter.openInputMethod(WindowLinux::display(), WindowLinux::handle());
    }

    if (!createContext()) {
        destroyContext();
        WindowLinux::destroy();
        return -1;
    }

    XWindowAttributes attributes{};
    XGetWindowAttributes(WindowLinux::display(), WindowLinux::handle(), &attributes);
    WindowLinux::updateSize(attributes.width, attributes.height);

    // Installed after the initial size, so the engine is not told the view
    // changed before it has been told the view exists.
    WindowLinux::setResizeCallback([](int, int) {
        Engine::systemViewChanged();
    });

    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    if (platform.windowMode == 1)
        WindowLinux::maximize();
    else if (platform.windowMode == 2)
        WindowLinux::requestFullscreen();

    bool hidden = false;
    while (!shouldClose) {
        // a present to a hidden window may block (or draw unseen): the game updates without
        // drawing, at a steady rate between X events
        if (hidden)
            waitForEvents(hiddenClock.secondsUntilDue());

        processEvents();
        if (shouldClose)
            break;

        const bool nowHidden = !platform.headless && visibility.isHidden();
        if (nowHidden != hidden) {
            hidden = nowHidden;
            // what is held comes up before the game hears of it, and the pads are not read
            if (hidden) {
                gamepads.release();
                hiddenClock.reset();
            }
            Engine::systemVisibilityChanged(!hidden);
        }

        if (hidden) {
            if (hiddenClock.isDue(Engine::getHiddenUpdateFps(), 0.0))
                Engine::systemUpdate();
            continue;
        }

        // a headless run takes no input
        if (!platform.headless)
            gamepads.poll(monotonicSeconds());

        beginFrame();
        Engine::systemDraw();
        endFrame();

        frameLimiter.wait(Engine::getFramePacing().limiterFps);
    }

    Engine::systemViewDestroyed();
    Engine::systemShutdown();

    gamepads.shutdown();
    destroyContext();
    if (!platform.headless)
        inputRouter.closeInputMethod();
    WindowLinux::destroy();
    return player::RunControl::finish();
}
