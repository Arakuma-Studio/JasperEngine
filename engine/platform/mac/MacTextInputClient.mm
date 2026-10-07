// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MacTextInputClient.h"

#include "Engine.h"

#include <string>

using namespace jasperengine;

namespace {

    // The input manager hands over either kind
    NSString* plainString(id string) {
        if ([string isKindOfClass:[NSAttributedString class]])
            return [(NSAttributedString*)string string];
        if ([string isKindOfClass:[NSString class]])
            return (NSString*)string;
        return @"";
    }

    std::string utf8Of(NSString* string) {
        const char* utf8 = [string UTF8String];
        return utf8 ? std::string(utf8) : std::string();
    }

    // NSRange counts UTF-16 units; the engine counts code points
    int codepointsBefore(NSString* string, NSUInteger units) {
        const NSUInteger length = [string length];
        if (units > length) units = length;

        int count = 0;
        for (NSUInteger i = 0; i < units; i++, count++) {
            if (CFStringIsSurrogateHighCharacter([string characterAtIndex:i]) && i + 1 < units &&
                CFStringIsSurrogateLowCharacter([string characterAtIndex:i + 1]))
                i++;
        }
        return count;
    }

    // Function keys can arrive as characters of the private range 0xF700-0xF7FF;
    // they are keys, already reported by keyDown:, not text
    NSString* withoutFunctionKeys(NSString* string) {
        static NSCharacterSet* functionKeys = [NSCharacterSet characterSetWithRange:NSMakeRange(0xF700, 0x100)];
        if ([string rangeOfCharacterFromSet:functionKeys].location == NSNotFound) return string;
        return [[string componentsSeparatedByCharactersInSet:functionKeys] componentsJoinedByString:@""];
    }

}

@implementation MacTextInputClient {
    // The view owns the client, so a strong reference here would be a cycle
    __weak NSView* _view;
    // The open composition, nil when there is none; the engine has it too
    NSString* _markedText;
    NSRange _markedSelection;
    // Set while the input method is told to drop its session, so whatever it
    // reports back is not taken as text
    BOOL _discarding;
    NSRect _area;
    BOOL _hasArea;
}

- (instancetype)initWithView:(NSView*)view {
    self = [super init];
    if (self) {
        _view = view;
        _markedText = nil;
        _markedSelection = NSMakeRange(NSNotFound, 0);
        _discarding = NO;
        _area = NSZeroRect;
        _hasArea = NO;
    }
    return self;
}

// Committed text: typed characters, an IME commit, the character viewer. While
// composing it replaces the composition, which the engine reads as a confirm.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if (_discarding) return;

    const BOOL composing = _markedText != nil;
    _markedText = nil;
    _markedSelection = NSMakeRange(NSNotFound, 0);

    NSString* text = withoutFunctionKeys(plainString(string));
    if ([text length] > 0) Engine::systemTextInput(utf8Of(text));
    else if (composing) Engine::systemImeCancel();
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    if (_discarding) return;

    NSString* text = plainString(string);
    if ([text length] == 0) {
        // The input method emptied its composition (Escape, or everything deleted)
        const BOOL composing = _markedText != nil;
        _markedText = nil;
        _markedSelection = NSMakeRange(NSNotFound, 0);
        if (composing) Engine::systemImeCancel();
        return;
    }

    _markedText = [text copy];
    _markedSelection = selectedRange;

    // selectedRange is relative to the marked string; the caret is its start
    const int cursor = selectedRange.location == NSNotFound
        ? codepointsBefore(text, [text length])
        : codepointsBefore(text, selectedRange.location);
    Engine::systemImeComposition(utf8Of(text), cursor);
}

// The input method gives up its composition as it stands: that is a commit
- (void)unmarkText {
    if (_discarding) return;

    NSString* text = _markedText;
    _markedText = nil;
    _markedSelection = NSMakeRange(NSNotFound, 0);
    if (!text) return;

    Engine::systemTextInput(utf8Of(text));
}

- (BOOL)hasMarkedText {
    return _markedText != nil;
}

// The engine keeps the document text, so the only text this client knows about is
// the composition: it starts at 0 and the selection is the IME's own, inside it
- (NSRange)markedRange {
    if (!_markedText) return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(0, [_markedText length]);
}

- (NSRange)selectedRange {
    if (!_markedText) return NSMakeRange(NSNotFound, 0);
    return _markedSelection;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    (void)actualRange;
    return nil;
}

// Every character is at the caret area the engine last reported; the candidate
// window opens next to it.
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange) *actualRange = range;

    NSView* view = _view;
    NSWindow* window = [view window];
    if (!view || !window) return NSZeroRect;

    NSRect inView;
    if (_hasArea) {
        // The inverse of MacInputRouter's mousePointOf: engine pixels are backing
        // pixels from the top left, flipped against the backing height
        const CGFloat backingHeight = [view convertSizeToBacking:view.bounds.size].height;
        const NSRect inBacking = NSMakeRect(_area.origin.x,
                                            backingHeight - _area.origin.y - _area.size.height,
                                            _area.size.width,
                                            _area.size.height);
        inView = [view convertRectFromBacking:inBacking];
    } else {
        // No text field has reported a caret yet: the bottom left of the view
        inView = NSMakeRect(NSMinX(view.bounds), NSMinY(view.bounds), 0.0, 0.0);
    }

    const NSRect inWindow = [view convertRect:inView toView:nil];
    return [window convertRectToScreen:inWindow];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}

- (void)setTextInputArea:(NSRect)area {
    _area = area;
    _hasArea = YES;
    // The candidate window asks firstRectForCharacterRange: again
    [[_view inputContext] invalidateCharacterCoordinates];
}

- (void)discardMarkedText {
    _markedText = nil;
    _markedSelection = NSMakeRange(NSNotFound, 0);

    _discarding = YES;
    [[_view inputContext] discardMarkedText];
    _discarding = NO;
}

@end

void jasperengine::macSetTextInputArea(NSView* view, float x, float y, float width, float height) {
    if (![view conformsToProtocol:@protocol(MacTextInputView)]) return;
    [[(id<MacTextInputView>)view engineTextInputClient] setTextInputArea:NSMakeRect(x, y, width, height)];
}

void jasperengine::macDiscardMarkedText(NSView* view) {
    if (![view conformsToProtocol:@protocol(MacTextInputView)]) return;
    [[(id<MacTextInputView>)view engineTextInputClient] discardMarkedText];
}
