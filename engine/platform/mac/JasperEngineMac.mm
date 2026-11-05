// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineMac.h"

#import <Cocoa/Cocoa.h>

#if defined(SOKOL_METAL)
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import "MacViewMetal.h"
#elif defined(SOKOL_VULKAN)
#import "MacViewVulkan.h"
#include "VulkanContext.h"
#else
#import "MacViewGL.h"
#endif

#include "SystemMac.h"
#include "WindowMac.h"

#include "Engine.h"
#include "device/Device.h"

#include <algorithm>
#include "PlatformConfig.h"
#include "player/RunControl.h"
#include "util/FrameLimiter.h"
#if defined(SOKOL_METAL)
#include "../apple/MetalReadback.h"
#endif

#import "JasperEngineGameController.h"

#include <cstdlib>

using namespace jasperengine;

namespace {
    // Engine.targetFrameRate without vsync: frames the timer below skips
    FrameLimiter frameLimiter;

#if defined(SOKOL_METAL)
    MacViewMetal* gameView = nil;
#elif defined(SOKOL_VULKAN)
    MacViewVulkan* gameView = nil;

    VkResult createWindowSurface(VkInstance instance, VkSurfaceKHR* surface) {
        VkMetalSurfaceCreateInfoEXT surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        surfaceInfo.pLayer = gameView.metalLayer;
        return vkCreateMetalSurfaceEXT(instance, &surfaceInfo, nullptr, surface);
    }

    bool createContext() {
        VulkanContextConfig config;
        config.surfaceExtension = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
        config.createSurface = createWindowSurface;
        config.vsync = Engine::getFramePacing().swapInterval > 0;
        return VulkanContext::create(config);
    }
#else
    MacViewGL* gameView = nil;
#endif
}

#if defined(SOKOL_METAL)
@interface JasperEngineMacDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, MTKViewDelegate>
#else
@interface JasperEngineMacDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
#endif
@property (nonatomic, strong) NSTimer* frameTimer;
@end

@implementation JasperEngineMacDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;

    const PlatformConfig& platform = platformConfig();
    WindowMacConfig config;
    config.title = platform.title;
    config.width = platform.width;
    config.height = platform.height;
    config.resizable = platform.resizable;

    WindowMac::create(config);
    WindowMac::setWindowDelegate((__bridge void*)self);

    // Device.thermalState (spec S56): NSProcessInfo's, read now (which makes the notifications
    // come) and on every change; the thermal response is off on desktops unless the project
    // turns it on
    auto reportThermal = []() {
        const NSProcessInfoThermalState state = [[NSProcessInfo processInfo] thermalState];
        Device::reportThermalState((ThermalState)std::clamp((int)state, 0, THERMAL_STATE_COUNT - 1));
    };
    reportThermal();
    [[NSNotificationCenter defaultCenter] addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil
                                                       queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification* note) {
        (void)note;
        reportThermal();
    }];
    WindowMac::setDrawableResizeCallback([](int, int) {
        Engine::systemViewChanged();
    });

    const NSRect viewFrame = NSMakeRect(0, 0, config.width, config.height);

#if defined(SOKOL_METAL)
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        NSLog(@"Error: Metal is not supported on this Mac");
        [NSApp terminate:nil];
        return;
    }

    gameView = [[MacViewMetal alloc] initWithFrame:viewFrame device:device];
    gameView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    gameView.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    // Paused: the frame timer below drives the view, like the OpenGL loop
    gameView.paused = YES;
    gameView.enableSetNeedsDisplay = NO;
    gameView.delegate = self;

    WindowMac::setContentView((__bridge void*)gameView);
    JasperEngineMac::applySwapInterval(Engine::getFramePacing().swapInterval);
    // --screenshot reads the composite on sokol's queue
    installMetalReadback();
#elif defined(SOKOL_VULKAN)
    gameView = [[MacViewVulkan alloc] initWithFrame:viewFrame];
    WindowMac::setContentView((__bridge void*)gameView);

    // The surface comes from the view's layer, so both go up before
    // systemViewLoaded, where sokol reads the environment
    if (!createContext()) {
        VulkanContext::destroy();
        NSLog(@"Error: Vulkan is not available on this Mac");
        [NSApp terminate:nil];
        return;
    }
#else
    gameView = [[MacViewGL alloc] initWithFrame:viewFrame];
    if (!gameView || ![gameView pixelFormat]) {
        NSLog(@"Error: no OpenGL 4.1 core pixel format available");
        [NSApp terminate:nil];
        return;
    }

    WindowMac::setContentView((__bridge void*)gameView);
    [gameView makeContextCurrent];
    JasperEngineMac::applySwapInterval(Engine::getFramePacing().swapInterval);
    // the shader creation thread's context (spec S32)
    MacViewGL* sharingView = gameView;
    setGpuWorkerContextFactory([sharingView]() { return makeMacGLWorkerContext(sharingView); });
#endif

    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    if (platform.headless) {
        // no input, nothing seen; the frames render all the same
        WindowMac::showHeadless();
    } else {
        [JasperEngineGameController start];
        WindowMac::show(true);
        WindowMac::applyInitialWindowMode(platform.windowMode == 1, platform.windowMode == 2);
    }

    // A timer-driven loop keeps the run loop live, so Cocoa still delivers
    // input and window events between frames. VSync paces the actual rate.
    self.frameTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 240.0
                                                      repeats:YES
                                                        block:^(NSTimer*){ [self renderFrame]; }];
    [[NSRunLoop currentRunLoop] addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
}

- (void)renderFrame {
    if (!gameView) return;
    // the timer runs at 240 Hz; a frame rate cap draws only the frames due
    if (!frameLimiter.isDue(Engine::getFramePacing().limiterFps)) return;
#if defined(SOKOL_METAL)
    // Runs drawInMTKView below, the only place currentDrawable is valid
    [gameView draw];
#elif defined(SOKOL_VULKAN)
    VulkanContext::beginFrame();
    Engine::systemDraw();
    VulkanContext::endFrame();
#else
    [gameView makeContextCurrent];
    Engine::systemDraw();
    [gameView presentFrame];
#endif
}

#if defined(SOKOL_METAL)
- (void)drawInMTKView:(MTKView*)view {
    (void)view;
    Engine::systemDraw();
}

// The Metal counterpart of MacViewGL's reshape
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
    WindowMac::refreshDrawableSize();
}
#endif

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    WindowMac::refreshDrawableSize();
}

// Unity's OnApplicationFocus: the game window is the key window; held keys and buttons come
// up while another window has the keyboard
- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    Engine::systemFocusChanged(true);
    Engine::systemFocus(true);
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    Engine::systemFocusChanged(false);
    Engine::systemFocus(false);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

// The pointer is disassociated globally, so a capture left on while another app
// is in front would freeze the cursor system wide.
- (void)applicationDidResignActive:(NSNotification*)notification {
    (void)notification;
    WindowMac::setCursorCaptured(false, false);
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
    (void)notification;
    if (Engine::getMouseMode() == MouseMode::CAPTURED)
        WindowMac::setMouseMode(MouseMode::CAPTURED);
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    Engine::systemViewDestroyed();
    Engine::systemShutdown();
#if defined(SOKOL_VULKAN)
    // After sokol has released its own objects, which live on this device
    VulkanContext::destroy();
#endif
    gameView = nil;
    WindowMac::destroy();
    // [NSApp terminate:] exits with 0 after this: a run's own code (R19) goes first
    std::exit(player::RunControl::finish());
}

@end

void JasperEngineMac::applySwapInterval(int interval) {
    if (!gameView) return;
#if defined(SOKOL_METAL)
    static_cast<CAMetalLayer*>(gameView.layer).displaySyncEnabled = interval > 0 ? YES : NO;
#elif defined(SOKOL_VULKAN)
    VulkanContext::setVSync(interval > 0);
#else
    [gameView makeContextCurrent];
    [gameView setSwapInterval:interval > 0 ? 1 : 0];
#endif
}

int JasperEngineMac::init(int argc, char **argv) {
    @autoreleasepool {
        Engine::systemInit(argc, argv, new SystemMac());

        if (platformConfig().headless) {
            WindowMac::setupHeadlessApplication();
        } else {
            WindowMac::setupApplication();
            WindowMac::installMinimalMenuBar(platformConfig().title);
        }

        JasperEngineMacDelegate* delegate = [[JasperEngineMacDelegate alloc] init];
        [NSApp setDelegate:delegate];
        [NSApp run];
    }
    return 0;
}
