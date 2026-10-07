// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MacViewMetal_h
#define MacViewMetal_h

#import <MetalKit/MetalKit.h>

// The Metal drawable for macOS games. It holds no input handling of its own:
// every event goes to MacInputRouter and text to MacTextInputClient, shared with
// the OpenGL and Vulkan views.
@interface MacViewMetal : MTKView <NSTextInputClient>

@end

#endif /* MacViewMetal_h */
