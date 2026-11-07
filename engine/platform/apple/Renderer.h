// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import <MetalKit/MetalKit.h>

@interface Renderer : NSObject <MTKViewDelegate>

- (nonnull instancetype)initWithMetalKitView:(nonnull MTKView *)view withArgs:(nullable NSArray*)args;

- (void)destroyView;

+ (void)pauseGame;
+ (void)resumeGame;
// the app is no longer (or again) the one the user touches: held input comes up
+ (void)focusChanged:(BOOL)focused;
// Unity's OnApplicationFocus (Engine::systemFocus, after focusChanged:), a memory warning, a
// finished rotation
+ (void)focusGame:(BOOL)focused;
+ (void)lowMemory;
// A frame without drawing, for a window that can't be seen (Engine::systemUpdate, which waits for
// the GPU first)
+ (void)updateGame;
// the window can (not) be seen (Engine::systemVisibilityChanged)
+ (void)visibilityChanged:(BOOL)visible;
// the update rate of a hidden window (Engine::getHiddenUpdateFps)
+ (double)hiddenUpdateFps;
// NSProcessInfo.thermalState to Device (spec S56): at start (reading it once is what makes the
// notifications come) and on NSProcessInfoThermalStateDidChangeNotification
+ (void)thermalStateChanged;
+ (void)orientationChanged;
#if TARGET_OS_IPHONE
// Screen.safeArea: the view's safeAreaInsets (points) times its contentScaleFactor, the drawable's
// pixels per point
+ (void)safeAreaOfView:(nonnull UIView*)view;
#endif
// Engine.targetFrameRate / vSyncCount as the view's preferredFramesPerSecond
+ (void)applyFramePacing;
#if TARGET_OS_IPHONE
// Screen.orientation / autorotateTo* as a UIInterfaceOrientationMask
+ (NSUInteger)supportedInterfaceOrientations;
// the UIInterfaceOrientation of a fixed request, 0 (Unknown) in auto-rotation
+ (NSInteger)requestedInterfaceOrientation;
#endif

@property (class, nonatomic, assign, readonly, nullable) MTKView* view;
@property (class, nonatomic, assign, readonly) CGSize screenSize;

@end

// Posted (main thread) when Screen.orientation or autorotateTo* change: the view controller
// asks UIKit for the new supported orientations
extern NSString* const JasperEngineOrientationPolicyNotification;

