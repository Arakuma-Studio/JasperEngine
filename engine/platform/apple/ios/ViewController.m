// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "ViewController.h"
#import "Renderer.h"
#import "EngineView.h"

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

    _renderer = [[Renderer alloc] initWithMetalKitView:_view withArgs:args];

    [_renderer mtkView:_view drawableSizeWillChange:_view.drawableSize];

    _view.delegate = _renderer;

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

- (void)focusLost {
    [Renderer focusGame:NO];
}

- (void)focusGained {
    [Renderer focusGame:YES];
}

- (void)lowMemory {
    [Renderer lowMemory];
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

// onCanvasChanged comes with the new drawable size; onOrientationChanged once the rotation ended
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator
{
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
        [Renderer orientationChanged];
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
