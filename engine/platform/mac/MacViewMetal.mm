// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "MacViewMetal.h"

#include "MacInputRouter.h"
#include "MacTextInputClient.h"

// SystemMac reaches the text input client through the content view
@interface MacViewMetal () <MacTextInputView>
@end

@implementation MacViewMetal {
    MacInputRouter* _router;
    MacTextInputClient* _textInput;
}

// MTKView's initWithFrame:device: does not route through initWithFrame:, and a
// storyboard uses initWithCoder:, so all three have to end up with a router.
// MTKView may chain them, hence the guard.
- (void)setupInput {
    if (_router) return;
    _router = [[MacInputRouter alloc] initWithView:self];
    _textInput = [[MacTextInputClient alloc] initWithView:self];
}

- (instancetype)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device {
    self = [super initWithFrame:frame device:device];
    if (self) [self setupInput];
    return self;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) [self setupInput];
    return self;
}

- (instancetype)initWithCoder:(NSCoder*)coder {
    self = [super initWithCoder:coder];
    if (self) [self setupInput];
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

- (void)viewDidMoveToWindow { [[self window] setAcceptsMouseMovedEvents:YES]; }
- (void)updateTrackingAreas { [_router updateTrackingAreas]; [super updateTrackingAreas]; }

- (void)keyDown:(NSEvent*)event { [_router keyDown:event]; }
- (void)keyUp:(NSEvent*)event { [_router keyUp:event]; }
- (void)flagsChanged:(NSEvent*)event { [_router flagsChanged:event]; }

- (void)mouseDown:(NSEvent*)event { [_router mouseDown:event button:D_MOUSE_BUTTON_LEFT]; }
- (void)rightMouseDown:(NSEvent*)event { [_router mouseDown:event button:D_MOUSE_BUTTON_RIGHT]; }
- (void)otherMouseDown:(NSEvent*)event { [_router mouseDown:event button:D_MOUSE_BUTTON_MIDDLE]; }
- (void)mouseUp:(NSEvent*)event { [_router mouseUp:event button:D_MOUSE_BUTTON_LEFT]; }
- (void)rightMouseUp:(NSEvent*)event { [_router mouseUp:event button:D_MOUSE_BUTTON_RIGHT]; }
- (void)otherMouseUp:(NSEvent*)event { [_router mouseUp:event button:D_MOUSE_BUTTON_MIDDLE]; }

- (void)mouseMoved:(NSEvent*)event { [_router mouseMoved:event]; }
- (void)mouseDragged:(NSEvent*)event { [_router mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [_router mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [_router mouseMoved:event]; }
- (void)tabletPoint:(NSEvent*)event { [_router tabletEvent:event]; }
- (void)tabletProximity:(NSEvent*)event { [_router tabletEvent:event]; }

- (void)scrollWheel:(NSEvent*)event { [_router scrollWheel:event]; }
- (void)mouseEntered:(NSEvent*)event { [_router mouseEntered:event]; }
- (void)mouseExited:(NSEvent*)event { [_router mouseExited:event]; }

// NSTextInputClient is a view protocol, so it cannot move into the router or the
// text input client; the view only forwards
- (MacTextInputClient*)engineTextInputClient { return _textInput; }
- (void)insertText:(id)string replacementRange:(NSRange)range { [_textInput insertText:string replacementRange:range]; }
- (void)doCommandBySelector:(SEL)selector { [_router doCommandBySelector:selector]; }
- (void)setMarkedText:(id)string selectedRange:(NSRange)selected replacementRange:(NSRange)replacement { [_textInput setMarkedText:string selectedRange:selected replacementRange:replacement]; }
- (void)unmarkText { [_textInput unmarkText]; }
- (NSRange)selectedRange { return [_textInput selectedRange]; }
- (NSRange)markedRange { return [_textInput markedRange]; }
- (BOOL)hasMarkedText { return [_textInput hasMarkedText]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual { return [_textInput attributedSubstringForProposedRange:range actualRange:actual]; }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return [_textInput validAttributesForMarkedText]; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual { return [_textInput firstRectForCharacterRange:range actualRange:actual]; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point { return [_textInput characterIndexForPoint:point]; }

@end
