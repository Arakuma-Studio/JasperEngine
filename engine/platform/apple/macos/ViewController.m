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

// MTKView's default of 60 runs a 75 Hz display at 37.5
- (void)updateFrameRate
{
    NSScreen *screen = self.view.window.screen ?: NSScreen.mainScreen;
    _view.preferredFramesPerSecond = screen.maximumFramesPerSecond;
}

@end
