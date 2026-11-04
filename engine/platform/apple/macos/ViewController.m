// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "ViewController.h"
#import "Renderer.h"
#import "EngineView.h"
#import "JasperNative.h"

@implementation ViewController
{
    EngineView *_view;

    Renderer *_renderer;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    NSArray *args = [[NSProcessInfo processInfo] arguments];

    _view = (EngineView *)self.view;

    _view.device = MTLCreateSystemDefaultDevice();

    if(!_view.device)
    {
        NSLog(@"Metal is not supported on this device");
        self.view = [[NSView alloc] initWithFrame:self.view.frame];
        return;
    }

    // native plugins' +jasperInit (SDK setup) before the engine and its Lua start
    [JasperNative dispatchInit];

    _renderer = [[Renderer alloc] initWithMetalKitView:_view withArgs:args];

    [_renderer mtkView:_view drawableSizeWillChange:_view.drawableSize];

    _view.delegate = _renderer;

    // Not on viewDidDisappear, which minimizing or hiding the app also sends
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(destroyView)
                                                 name:NSApplicationWillTerminateNotification
                                               object:nil];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(updateFrameRate)
                                                 name:NSWindowDidChangeScreenNotification
                                               object:nil];

    // Device.thermalState (spec S56): read now, then on every change (the thermal response is off
    // on desktops unless the project turns it on)
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(thermalStateChanged)
                                                 name:NSProcessInfoThermalStateDidChangeNotification
                                               object:nil];
    [Renderer thermalStateChanged];

    // Unity's OnApplicationFocus: the game window is the key window
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(windowBecameKey:)
                                                 name:NSWindowDidBecomeKeyNotification
                                               object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(windowResignedKey:)
                                                 name:NSWindowDidResignKeyNotification
                                               object:nil];
}

- (void)thermalStateChanged
{
    [Renderer thermalStateChanged];
}

- (void)windowBecameKey:(NSNotification *)notification
{
    if (notification.object == self.view.window) [Renderer focusGame:YES];
}

- (void)windowResignedKey:(NSNotification *)notification
{
    if (notification.object == self.view.window) [Renderer focusGame:NO];
}

- (void)viewDidAppear
{
    [super viewDidAppear];
    [self updateFrameRate];
}

- (void)destroyView
{
    [_renderer destroyView];
}

// MTKView's default of 60 runs a 75 Hz display at 37.5: the screen's rate, or what
// Engine.targetFrameRate / vSyncCount ask for
- (void)updateFrameRate
{
    [Renderer applyFramePacing];
}

@end
