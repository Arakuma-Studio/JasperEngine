// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MacTextInputClient_h
#define MacTextInputClient_h

#import <Cocoa/Cocoa.h>

// The NSTextInputClient logic of the macOS drawables (MacViewMetal, MacViewGL and
// MacViewVulkan; the Xcode app's EngineView through MacViewMetal). The protocol has
// to be adopted by the view itself, so each view forwards its methods here.
// Marked text (an IME composition, a dead key) goes to Engine::systemImeComposition,
// committed text to Engine::systemTextInput. doCommandBySelector: stays with
// MacInputRouter, which turns editing commands into characters.
@interface MacTextInputClient : NSObject

- (instancetype)initWithView:(NSView*)view;

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange;
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange;
- (void)unmarkText;
- (BOOL)hasMarkedText;
- (NSRange)markedRange;
- (NSRange)selectedRange;
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText;
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange;
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange;
- (NSUInteger)characterIndexForPoint:(NSPoint)point;

// Where the caret is, in engine pixels: the view's backing pixels from its top left,
// the space MacInputRouter reports the mouse in. The IME places its candidate window
// next to it.
- (void)setTextInputArea:(NSRect)area;
// Drops the marked text without telling the engine, which cancelled its composition
// already, and ends the input method's session
- (void)discardMarkedText;

@end

// The views that own a MacTextInputClient, so the System side can reach it from the
// content view alone
@protocol MacTextInputView <NSObject>
- (MacTextInputClient*)engineTextInputClient;
@end

#ifdef __cplusplus
namespace jasperengine {

    // For the System subclasses (SystemMac, and JasperEngineApple on macOS); main thread.
    // System::setTextInputArea: engine pixels, top-left origin.
    void macSetTextInputArea(NSView* view, float x, float y, float width, float height);
    // System::hideVirtualKeyboard: the focused document text field went away
    void macDiscardMarkedText(NSView* view);

}
#endif

#endif /* MacTextInputClient_h */
