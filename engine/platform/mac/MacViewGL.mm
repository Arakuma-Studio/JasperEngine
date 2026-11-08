// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "MacViewGL.h"

#include "MacInputRouter.h"
#include "MacTextInputClient.h"
#include "WindowMac.h"

namespace {

    class MacGLWorkerContext : public jasperengine::GpuWorkerContext {
    public:
        explicit MacGLWorkerContext(NSOpenGLContext* context) : context(context) {}
        bool makeCurrent() override {
            [context makeCurrentContext];
            return [NSOpenGLContext currentContext] == context;
        }
        void release() override { [NSOpenGLContext clearCurrentContext]; }
    private:
        NSOpenGLContext* context;
    };

}

std::unique_ptr<jasperengine::GpuWorkerContext> makeMacGLWorkerContext(MacViewGL* view) {
    NSOpenGLContext* shared = [view openGLContext];
    NSOpenGLPixelFormat* format = [view pixelFormat];
    if (!shared || !format)
        return nullptr;
    NSOpenGLContext* context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:shared];
    if (!context)
        return nullptr;
    return std::make_unique<MacGLWorkerContext>(context);
}

// SystemMac reaches the text input client through the content view
@interface MacViewGL () <MacTextInputView>
@end

@implementation MacViewGL {
    MacInputRouter* _router;
    MacTextInputClient* _textInput;
}

// NSOpenGLView's initWithFrame: asks the class for its format, so this is what
// gets a 4.1 core profile out of the plain initializer.
+ (NSOpenGLPixelFormat*)defaultPixelFormat {
    static NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAStencilSize, 8,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        0
    };
    return [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
}

- (instancetype)initWithFrame:(NSRect)frame pixelFormat:(NSOpenGLPixelFormat*)format {
    self = [super initWithFrame:frame pixelFormat:format];
    if (self) {
        _router = [[MacInputRouter alloc] initWithView:self];
        _textInput = [[MacTextInputClient alloc] initWithView:self];
        // Without this the surface stays at 1x on a Retina display
        [self setWantsBestResolutionOpenGLSurface:YES];
    }
    return self;
}

- (instancetype)initWithFrame:(NSRect)frame {
    return [self initWithFrame:frame pixelFormat:[[self class] defaultPixelFormat]];
}

- (void)setSwapInterval:(int)interval {
    GLint value = interval;
    [[self openGLContext] setValues:&value
                       forParameter:NSOpenGLContextParameterSwapInterval];
}

- (void)makeContextCurrent { [[self openGLContext] makeCurrentContext]; }

- (void)presentFrame { [[self openGLContext] flushBuffer]; }

- (void)reshape {
    [super reshape];
    [[self openGLContext] update];
    jasperengine::WindowMac::refreshDrawableSize();
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }
- (BOOL)isOpaque { return YES; }

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
