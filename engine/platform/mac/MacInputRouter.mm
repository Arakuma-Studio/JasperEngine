// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MacInputRouter.h"
#include "KeyCodesMac.h"

#include "Engine.h"
#include "Input.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    // A mouse or tablet event's pen pressure: a tablet's point (also a mouse event's subtype), 0
    // for a pen coming into proximity, -1 for a mouse, a puck or a pen leaving (the editor's
    // BackendMac.mm reads it the same way)
    float macPenPressure(NSEvent* event) {
        // subtype throws on the events that have none: only mouse events reach it
        const bool proximity = event.type == NSEventTypeTabletProximity ||
            (event.type != NSEventTypeTabletPoint && event.subtype == NSEventSubtypeTabletProximity);
        if (proximity) {
            const bool pen = event.pointingDeviceType == NSPointingDeviceTypePen ||
                             event.pointingDeviceType == NSPointingDeviceTypeEraser;
            return event.isEnteringProximity && pen ? 0.0f : -1.0f;
        }
        if (event.type == NSEventTypeTabletPoint || event.subtype == NSEventSubtypeTabletPoint)
            return std::clamp(static_cast<float>(event.pressure), 0.0f, 1.0f);
        return -1.0f;
    }

}

@implementation MacInputRouter {
    // The view owns the router, so a strong reference here would be a cycle
    __weak NSView* _view;
    NSTrackingArea* _trackingArea;
    CGPoint _mousePoint;
    BOOL _hasMousePoint;
    // Keys whose down went to the input method's composition instead of the engine,
    // so their up is held back too
    bool _imeKeys[D_KEY_LAST + 1];
}

- (instancetype)initWithView:(NSView*)view {
    self = [super init];
    if (self) {
        _view = view;
        _trackingArea = nil;
        _mousePoint = CGPointZero;
        _hasMousePoint = NO;
    }
    return self;
}

- (void)updateTrackingAreas {
    NSView* view = _view;
    if (!view) return;

    if (_trackingArea != nil)
        [view removeTrackingArea:_trackingArea];

    // ActiveAlways so a game keeps tracking the pointer while not frontmost
    const NSTrackingAreaOptions options =
        NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
        NSTrackingActiveAlways | NSTrackingInVisibleRect;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:[view bounds]
                                                options:options
                                                  owner:view
                                               userInfo:nil];
    [view addTrackingArea:_trackingArea];
}

// The engine works in backing pixels from the top left; Cocoa reports window
// points from the bottom left.
- (CGPoint)mousePointOf:(NSEvent*)event {
    NSView* view = _view;
    if (!view) return CGPointZero;

    const NSPoint inView = [view convertPoint:[event locationInWindow] fromView:nil];
    const NSPoint inBacking = [view convertPointToBacking:inView];
    const CGFloat height = [view convertSizeToBacking:view.bounds.size].height;
    return CGPointMake(inBacking.x, height - inBacking.y);
}

- (CGPoint)trackedMousePointOf:(NSEvent*)event {
    const bool captured = Engine::getMouseMode() == MouseMode::CAPTURED;
    if (!captured || !_hasMousePoint) {
        _mousePoint = [self mousePointOf:event];
        _hasMousePoint = YES;
        return _mousePoint;
    }

    NSView* view = _view;
    if (!view) return _mousePoint;

    // Captured: the pointer is pinned, so only the deltas are meaningful. A move
    // event's deltaY points down, like the engine coordinates.
    const NSSize delta = [view convertSizeToBacking:NSMakeSize([event deltaX], [event deltaY])];
    _mousePoint.x += delta.width;
    _mousePoint.y += delta.height;
    return _mousePoint;
}

// The view's NSTextInputClient holds marked text: an IME composition or a dead key
- (BOOL)isComposing {
    NSView* view = _view;
    return [view conformsToProtocol:@protocol(NSTextInputClient)] &&
           [(id<NSTextInputClient>)view hasMarkedText];
}

- (void)keyDown:(NSEvent*)event {
    const int key = macKeyFromCode([event keyCode]);
    if (key != D_KEY_UNKNOWN) {
        // While a document text field shows a composition, the keys typed into it
        // (Return confirming, Backspace editing, Escape cancelling) are the input
        // method's, as Windows reports them as VK_PROCESSKEY; the document only gets
        // the composition and the commit. Everything else is reported as always.
        if (Engine::wantsTextComposition() && [self isComposing]) _imeKeys[key] = true;
        else Engine::systemKeyDown(key, [event isARepeat], macKeyModifiers([event modifierFlags]));
    }

    // Feeds the text input manager, which calls back into the view's
    // NSTextInputClient methods for IME and dead keys
    [_view interpretKeyEvents:@[event]];
}

- (void)keyUp:(NSEvent*)event {
    const int key = macKeyFromCode([event keyCode]);
    if (key == D_KEY_UNKNOWN) return;

    if (_imeKeys[key]) {
        _imeKeys[key] = false;
        return;
    }
    Engine::systemKeyUp(key, [event isARepeat], macKeyModifiers([event modifierFlags]));
}

- (void)flagsChanged:(NSEvent*)event {
    const int key = macKeyFromCode([event keyCode]);
    if (key == D_KEY_UNKNOWN) return;

    const NSUInteger flags = [event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
    const int mods = macKeyModifiers([event modifierFlags]);

    // flagsChanged carries no up/down, so the modifier's own bit decides
    if (flags & macModFlagForKey(key)) Engine::systemKeyDown(key, false, mods);
    else Engine::systemKeyUp(key, false, mods);
}

- (void)mouseDown:(NSEvent*)event button:(int)button {
    Input::setPenPressure(macPenPressure(event));
    const CGPoint point = [self trackedMousePointOf:event];
    Engine::systemMouseDown(button, point.x, point.y, macKeyModifiers([event modifierFlags]));
}

- (void)mouseUp:(NSEvent*)event button:(int)button {
    Input::setPenPressure(macPenPressure(event));
    const CGPoint point = [self trackedMousePointOf:event];
    Engine::systemMouseUp(button, point.x, point.y, macKeyModifiers([event modifierFlags]));
}

- (void)mouseMoved:(NSEvent*)event {
    Input::setPenPressure(macPenPressure(event));
    const CGPoint point = [self trackedMousePointOf:event];
    Engine::systemMouseMove(point.x, point.y, macKeyModifiers([event modifierFlags]));
}

- (void)tabletEvent:(NSEvent*)event {
    Input::setPenPressure(macPenPressure(event));
}

- (void)scrollWheel:(NSEvent*)event {
    float dx = (float)[event scrollingDeltaX];
    float dy = (float)[event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas]) {
        dx *= 0.1f;
        dy *= 0.1f;
    }
    if (dx != 0.0f || dy != 0.0f)
        Engine::systemMouseScroll(dx, dy, macKeyModifiers([event modifierFlags]));
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    Engine::systemMouseEnter();
    // Cocoa resets to the arrow on re-entry, dropping the game's chosen cursor
    if (Engine::getMouseCursor() != CursorType::ARROW)
        Engine::setMouseCursor(Engine::getMouseCursor());
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    Engine::systemMouseLeave();
}

// Keys the input manager reports as editing commands instead of characters. The
// component UI edits with these; a document text field already had the key down,
// and the engine drops the character for it.
- (void)doCommandBySelector:(SEL)selector {
    if (selector == @selector(deleteBackward:)) Engine::systemCharInput('\b');
    else if (selector == @selector(insertNewline:)) Engine::systemCharInput('\r');
    else if (selector == @selector(insertTab:)) Engine::systemCharInput('\t');
    else if (selector == @selector(cancelOperation:)) Engine::systemCharInput(0x1b);
}

@end
