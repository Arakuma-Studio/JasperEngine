// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "ViewController.h"
#import "Renderer.h"
#import "EngineView.h"
#import "JasperNative.h"
#import "JasperEngineGameController.h"

@implementation ViewController
{
    EngineView *_view;

    Renderer *_renderer;

    // the window can't be seen: the view is paused and this timer updates the game without drawing
    NSTimer *_updateTimer;
    BOOL _hidden;
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

    // Also sent on minimize and restore, Space changes and the app hidden
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:nil];
}

- (void)occlusionChanged:(NSNotification *)notification
{
    if (notification.object == self.view.window) [self updateVisibility];
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
    // a window opened behind others gets no occlusion change
    [self updateVisibility];
}

- (void)destroyView
{
    // nothing may call into the engine once it is torn down
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_updateTimer invalidate];
    _updateTimer = nil;
    _view.delegate = nil;

    [_renderer destroyView];
}

// Nothing drawn can be seen: the view pauses and the game updates at a steady rate instead
- (void)updateVisibility
{
    NSWindow *window = self.view.window;
    if (!window || !_renderer) return;
    const BOOL hidden = (window.occlusionState & NSWindowOcclusionStateVisible) == 0;
    if (hidden == _hidden) return;
    _hidden = hidden;

    // a hidden game keeps updating, but the pads are no longer read: what they hold comes up
    // before the game hears of it
    [JasperEngineGameController setSuspended:hidden];
    [Renderer visibilityChanged:!hidden];
    _view.paused = hidden;

    [_updateTimer invalidate];
    _updateTimer = nil;
    if (hidden) {
        _updateTimer = [NSTimer timerWithTimeInterval:1.0 / [Renderer hiddenUpdateFps]
                                              repeats:YES
                                                block:^(NSTimer *timer) { (void)timer; [Renderer updateGame]; }];
        [[NSRunLoop currentRunLoop] addTimer:_updateTimer forMode:NSRunLoopCommonModes];
    }
}

// MTKView's default of 60 runs a 75 Hz display at 37.5: the screen's rate, or what
// Engine.targetFrameRate / vSyncCount ask for
- (void)updateFrameRate
{
    [Renderer applyFramePacing];
}

@end
