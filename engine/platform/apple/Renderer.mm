// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "Renderer.h"

#include "Engine.h"
#include "Screen.h"
#include "device/Device.h"
#include "JasperEngineApple.h"
#include "../common/PlatformDefaults.h"
#include "player/Launch.h"
#include "MetalReadback.h"
#include "util/PlatformRules.h"
#import "JasperEngineGameController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

NSString* const JasperEngineOrientationPolicyNotification = @"JasperEngineOrientationPolicyNotification";

@implementation Renderer

static MTKView* _view;
static CGSize _screenSize;

+ (MTKView*)view {
    return _view;
}

+ (CGSize)screenSize {
    return _screenSize;
}

- (nonnull instancetype)initWithMetalKitView:(nonnull MTKView *)view withArgs:(NSArray*)args;
{
    std::vector<std::string> arguments;
    arguments.reserve([args count]);
    for (NSString* arg in args)
        arguments.emplace_back([arg UTF8String]);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& arg : arguments)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    // the build's settings, then the launch options (and a player's game.json) over them; a
    // player whose content does not match cannot run (docs/specs/S30-shipping.md R21)
    jasperengine::applyPlatformDefaults();
    const int launched = jasperengine::player::launch((int)arguments.size(), argv.data());
    if (launched >= 0) {
        std::fprintf(stderr, "JasperEngine: exiting with code %d\n", launched);
        std::exit(launched);
    }
    jasperengine::Engine::systemInit((int)arguments.size(), argv.data(), new JasperEngineApple());
    // --screenshot reads the composite on sokol's queue
    jasperengine::installMetalReadback();

    [JasperEngineGameController start];

    self = [super init];
    if(self)
    {
        _view = view;
        [view setColorPixelFormat:MTLPixelFormatBGRA8Unorm];
        [view setDepthStencilPixelFormat:MTLPixelFormatDepth32Float_Stencil8];
        
        _screenSize.width = (float) view.drawableSize.width;
        _screenSize.height = (float) view.drawableSize.height;
        jasperengine::Engine::systemViewLoaded();
        // what init() set before the view existed
        [Renderer applyFramePacing];
    }

    return self;
}

- (void)destroyView
{
    jasperengine::Engine::systemViewDestroyed();
    jasperengine::Engine::systemShutdown();
}

- (void)drawInMTKView:(nonnull MTKView *)view
{
    jasperengine::Engine::systemDraw();
}

- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size
{
    _screenSize.width = (float) size.width;
    _screenSize.height = (float) size.height;
#if TARGET_OS_IPHONE
    // the insets that go with the new size (a rotation), then onCanvasChanged and the rest
    [Renderer safeAreaOfView:view];
#endif
    jasperengine::Engine::systemViewChanged();
}

+ (void)pauseGame {
    jasperengine::Engine::systemPause();
}

+ (void)resumeGame {
    jasperengine::Engine::systemResume();
}

+ (void)focusChanged:(BOOL)focused {
    jasperengine::Engine::systemFocusChanged(focused);
}

+ (void)focusGame:(BOOL)focused {
    jasperengine::Engine::systemFocusChanged(focused == YES);
    jasperengine::Engine::systemFocus(focused == YES);
}

+ (void)lowMemory {
    jasperengine::Engine::systemLowMemory();
}

+ (void)updateGame {
    jasperengine::Engine::systemUpdate();
}

+ (void)visibilityChanged:(BOOL)visible {
    jasperengine::Engine::systemVisibilityChanged(visible == YES);
}

+ (double)hiddenUpdateFps {
    return jasperengine::Engine::getHiddenUpdateFps();
}

+ (void)thermalStateChanged {
    // NSProcessInfoThermalStateNominal, Fair, Serious, Critical: Device's order
    const NSProcessInfoThermalState state = [[NSProcessInfo processInfo] thermalState];
    jasperengine::Device::reportThermalState((jasperengine::ThermalState)std::clamp((int)state, 0, jasperengine::THERMAL_STATE_COUNT - 1));
}

+ (void)orientationChanged {
    jasperengine::Engine::systemOrientationChanged();
}

#if TARGET_OS_IPHONE
+ (void)safeAreaOfView:(nonnull UIView*)view {
    // the engine's screen is the drawable (pixels); the home indicator's inset stays even with
    // prefersHomeIndicatorAutoHidden (iOS keeps it); iOS gives no cutout rectangles
    const UIEdgeInsets insets = view.safeAreaInsets;
    const CGFloat scale = view.contentScaleFactor;
    jasperengine::Screen::setPlatformSafeArea(jasperengine::EdgeInsets((float)std::round(insets.left * scale), (float)std::round(insets.top * scale),
                                                                       (float)std::round(insets.right * scale), (float)std::round(insets.bottom * scale)));
}
#endif

+ (void)applyFramePacing {
    if (!_view) return;
    const jasperengine::FramePacing& pacing = jasperengine::Engine::getFramePacing();
#if TARGET_OS_IPHONE
    // 120 needs CADisableMinimumFrameDurationOnPhone (iOS Supports High Refresh Rate)
    _view.preferredFramesPerSecond = pacing.mobileFps;
#else
    // MTKView presents synchronized: a vSyncCount past 1 or a frame rate cap lowers its rate
    NSScreen* screen = _view.window.screen ?: NSScreen.mainScreen;
    const NSInteger maximum = screen ? screen.maximumFramesPerSecond : 60;
    NSInteger fps = maximum;
    if (pacing.limiterFps > 0.0) fps = std::min<NSInteger>(maximum, (NSInteger)std::lround(pacing.limiterFps));
    else if (pacing.swapInterval > 1) fps = std::max<NSInteger>(1, maximum / pacing.swapInterval);
    _view.preferredFramesPerSecond = std::max<NSInteger>(1, fps);
#endif
}

#if TARGET_OS_IPHONE
+ (NSUInteger)supportedInterfaceOrientations {
    const NSUInteger mask = jasperengine::PlatformRules::iosSupportedMask(jasperengine::Screen::getRequestedOrientation(),
                                                                          jasperengine::Screen::getAllowedMask());
    return mask ? mask : UIInterfaceOrientationMaskAll;
}

+ (NSInteger)requestedInterfaceOrientation {
    const jasperengine::ScreenOrientation requested = jasperengine::Screen::getRequestedOrientation();
    if (!jasperengine::PlatformRules::isFixedOrientation(requested)) return 0;
    return jasperengine::PlatformRules::iosInterfaceOrientation(requested);
}
#endif

@end
