// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "Renderer.h"

#include "Engine.h"
#include "JasperEngineApple.h"
#import "JasperEngineGameController.h"

#include <string>
#include <vector>

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
    jasperengine::Engine::systemInit((int)arguments.size(), argv.data(), new JasperEngineApple());

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
    jasperengine::Engine::systemViewChanged();
}

+ (void)pauseGame {
    jasperengine::Engine::systemPause();
}

+ (void)resumeGame {
    jasperengine::Engine::systemResume();
}

@end
