// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MacInputRouter_h
#define MacInputRouter_h

#import <Cocoa/Cocoa.h>

#include "Input.h"

// macOS event translation, shared by the Metal and OpenGL drawables. They cannot
// share a superclass (MTKView vs NSOpenGLView), so each forwards its events here
// instead of translating them itself.
@interface MacInputRouter : NSObject

- (instancetype)initWithView:(NSView*)view;

- (void)updateTrackingAreas;

- (void)keyDown:(NSEvent*)event;
- (void)keyUp:(NSEvent*)event;
- (void)flagsChanged:(NSEvent*)event;

- (void)mouseDown:(NSEvent*)event button:(int)button;
- (void)mouseUp:(NSEvent*)event button:(int)button;
- (void)mouseMoved:(NSEvent*)event;
// tabletPoint: and tabletProximity: (a pen's pressure without a mouse event)
- (void)tabletEvent:(NSEvent*)event;
- (void)scrollWheel:(NSEvent*)event;
- (void)mouseEntered:(NSEvent*)event;
- (void)mouseExited:(NSEvent*)event;

// NSTextInputClient stays on the view, and its text on MacTextInputClient; this
// turns the editing commands the input manager reports into characters
- (void)doCommandBySelector:(SEL)selector;

@end

#endif /* MacInputRouter_h */
