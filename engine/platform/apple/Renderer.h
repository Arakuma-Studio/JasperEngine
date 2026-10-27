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

@property (class, nonatomic, assign, readonly, nullable) MTKView* view;
@property (class, nonatomic, assign, readonly) CGSize screenSize;

@end

