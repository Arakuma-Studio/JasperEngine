// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "ViewController.h"
#import "Renderer.h"
#import "EngineView.h"
#import "JasperNative.h"

#include <math.h>

@implementation ViewController
{
    EngineView *_view;

    Renderer *_renderer;
}

- (void)viewDidDisappear
{
    [_renderer destroyView];
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    
    NSArray *args = [[NSProcessInfo processInfo] arguments];

    _view = (EngineView *)self.view;

    _view.device = MTLCreateSystemDefaultDevice();
    _view.backgroundColor = UIColor.blackColor;

    if(!_view.device)
    {
        NSLog(@"Metal is not supported on this device");
        self.view = [[UIView alloc] initWithFrame:self.view.frame];
        return;
    }

    // native plugins' +jasperInit (SDK setup) before the engine and its Lua start
    [JasperNative dispatchInit];

    _renderer = [[Renderer alloc] initWithMetalKitView:_view withArgs:args];

    [_renderer mtkView:_view drawableSizeWillChange:_view.drawableSize];

    _view.delegate = _renderer;

    // Screen.safeArea from the start (zero until the view is in a window; the change follows)
    [self updateSafeArea];

    // Input.deviceOrientation reads UIDevice.orientation
    [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];

    // Focus: a Control Center pull, a call banner or the app switcher covers the scene
    [center addObserver:self selector:@selector(focusLost) name:UISceneWillDeactivateNotification object:nil];
    [center addObserver:self selector:@selector(focusGained) name:UISceneDidActivateNotification object:nil];

    // Pause game when its scene is backgrounded, resume when it comes back (before it is
    // active again: onResume, then onFocus(true))
    [center addObserver:self selector:@selector(pauseGame) name:UISceneDidEnterBackgroundNotification object:nil];
    [center addObserver:self selector:@selector(resumeGame) name:UISceneWillEnterForegroundNotification object:nil];

    [center addObserver:self selector:@selector(lowMemory) name:UIApplicationDidReceiveMemoryWarningNotification object:nil];

    // Device.thermalState (spec S56): read now, then on every change
    [center addObserver:self selector:@selector(thermalStateChanged) name:NSProcessInfoThermalStateDidChangeNotification object:nil];
    [Renderer thermalStateChanged];

    // Screen.orientation / Screen.autorotateTo* changed
    [center addObserver:self selector:@selector(updateOrientationPolicy) name:JasperEngineOrientationPolicyNotification object:nil];
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    // what init() asked for, now that the view has a window scene
    [self updateOrientationPolicy];
}

- (void)pauseGame {
    [Renderer pauseGame];
}

- (void)resumeGame {
    [Renderer resumeGame];
}

// focusGame: lets the fingers and pressed buttons go too (a call, the app switcher)
- (void)focusLost {
    [Renderer focusGame:NO];
}

- (void)focusGained {
    [Renderer focusGame:YES];
}

- (void)lowMemory {
    [Renderer lowMemory];
}

- (void)thermalStateChanged {
    [Renderer thermalStateChanged];
}

// Screen.orientation: a fixed orientation alone, or the autorotateTo* set (Unity's
// LandscapeLeft is UIKit's LandscapeRight). The app delegate allows every orientation, so the
// Info.plist list only picks the launch orientation.
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return (UIInterfaceOrientationMask)[Renderer supportedInterfaceOrientations];
}

- (void)updateOrientationPolicy
{
    UIInterfaceOrientationMask mask = (UIInterfaceOrientationMask)[Renderer supportedInterfaceOrientations];
    if (@available(iOS 16.0, *)) {
        [self setNeedsUpdateOfSupportedInterfaceOrientations];
        UIWindowScene *scene = self.view.window.windowScene;
        if (!scene) return;
        UIWindowSceneGeometryPreferencesIOS *preferences =
            [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
        [scene requestGeometryUpdateWithPreferences:preferences errorHandler:^(NSError *error) {
            NSLog(@"Screen.orientation: iOS refused the orientation change: %@", error.localizedDescription);
        }];
    } else {
        // the long-standing way: turn the device's reported orientation to a fixed request
        // the interface is not in, then have UIKit check the supported ones again
        UIInterfaceOrientation fixed = (UIInterfaceOrientation)[Renderer requestedInterfaceOrientation];
        UIInterfaceOrientation current = self.view.window.windowScene.interfaceOrientation;
        if (fixed != UIInterfaceOrientationUnknown && fixed != current) {
            [[UIDevice currentDevice] setValue:@(fixed) forKey:@"orientation"];
        }
        [UIViewController attemptRotationToDeviceOrientation];
    }
}

// Screen.safeArea: UIKit's safe area of the view (the notch, the Dynamic Island, rounded
// corners, the home indicator), in drawable pixels
- (void)viewSafeAreaInsetsDidChange
{
    [super viewSafeAreaInsetsDidChange];
    [self updateSafeArea];
}

// During a rotation the view's bounds (and its insets) change before the drawable does: those
// insets are pushed by mtkView:drawableSizeWillChange: with the new size instead, so the game sees
// one change with the new insets and the new size together
- (void)updateSafeArea
{
    if (!_renderer) return;
    CGFloat scale = self.view.contentScaleFactor;
    CGSize bounds = self.view.bounds.size;
    CGSize drawable = _view.drawableSize;
    if (fabs(bounds.width * scale - drawable.width) > 1.0 || fabs(bounds.height * scale - drawable.height) > 1.0) return;
    [Renderer safeAreaOfView:self.view];
}

// onCanvasChanged comes with the new drawable size; onOrientationChanged once the rotation ended
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
        [Renderer orientationChanged];
        // the drawable has its new size: the insets in its pixels again
        [self updateSafeArea];
    }];
}

- (BOOL)shouldAutorotate
{
    return YES;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [[UIDevice currentDevice] endGeneratingDeviceOrientationNotifications];
}

@end
