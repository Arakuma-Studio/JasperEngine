// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "EngineView.h"
#import "KeyCodesIOS.h"

#include "Engine.h"

#include <string>

#define MAX_TOUCHES 10

// The hidden field always holds this, so the keyboard has something to delete and a
// backspace in an empty text field still reaches the engine
static NSString* const textFieldSentinel = @" ";

namespace {

    std::string utf8Of(NSString* string) {
        const char* utf8 = [string UTF8String];
        return utf8 ? std::string(utf8) : std::string();
    }

    // UIKit offsets count UTF-16 units; the engine counts code points
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

    // What the field holds beyond what the engine was already given: everything
    // after their common prefix, without splitting a surrogate pair
    NSString* addedText(NSString* reported, NSString* text) {
        const NSUInteger common = MIN([reported length], [text length]);
        NSUInteger match = 0;
        while (match < common && [reported characterAtIndex:match] == [text characterAtIndex:match])
            match++;
        if (match > 0 && CFStringIsSurrogateHighCharacter([text characterAtIndex:match - 1]))
            match--;
        return [text substringFromIndex:match];
    }

}

@class EngineView;

// Hardware key presses go to the first responder first. While the hidden field has
// the focus it reports them to the view before UIKit turns them into text, so a key
// down comes before its text, as on desktop.
@interface EngineTextField : UITextField
@property (nonatomic, weak) EngineView* engineView;
@end

@interface EngineView () <UITextFieldDelegate>
- (void)reportPresses:(NSSet<UIPress*>*)presses down:(BOOL)down unhandled:(NSMutableSet<UIPress*>*)unhandled;
@end

@implementation EngineTextField

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self.engineView reportPresses:presses down:YES unhandled:nil];
    [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self.engineView reportPresses:presses down:NO unhandled:nil];
    [super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self.engineView reportPresses:presses down:NO unhandled:nil];
    [super pressesCancelled:presses withEvent:event];
}

@end

@implementation EngineView {
    EngineTextField* _textField;
    // The part of the field's text the engine already has: the sentinel, plus what
    // an input method committed in front of a composition it keeps open
    NSString* _reportedText;
    // The engine has an open composition
    BOOL _composing;
    // Set while the field is changed from here, which is not typing
    BOOL _updatingField;
    // Hardware keys held, and those whose down went to a composition instead
    bool _keysDown[D_KEY_LAST + 1];
    bool _imeKeys[D_KEY_LAST + 1];
}

static UITouch* touches[MAX_TOUCHES];

- (id)initWithCoder:(NSCoder *)coder{
    self = [super initWithCoder:coder];
    
    [self engineInit];
    
    return self;
}

- (id)initWithFrame:(CGRect)frameRect{
    self = [super initWithFrame:frameRect];
    
    [self engineInit];
    
    return self;
}

- (void)engineInit{
    [self setUserInteractionEnabled:YES];
    [self setMultipleTouchEnabled:YES];

    if (_textField)
        return;

    _textField = [[EngineTextField alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
    _textField.engineView = self;
    _textField.delegate = self;
    // Nearly transparent rather than hidden, and without a visible caret
    _textField.alpha = 0.01;
    _textField.tintColor = UIColor.clearColor;
    // Corrections would rewrite text the engine already has
    _textField.autocorrectionType = UITextAutocorrectionTypeNo;
    _textField.spellCheckingType = UITextSpellCheckingTypeNo;
    _textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    _textField.smartQuotesType = UITextSmartQuotesTypeNo;
    _textField.smartDashesType = UITextSmartDashesTypeNo;
    _textField.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
    _textField.text = textFieldSentinel;
    _reportedText = textFieldSentinel;
    [self addSubview:_textField];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(textFieldTextDidChange:)
                                                 name:UITextFieldTextDidChangeNotification
                                               object:_textField];
}

- (void)dealloc{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// Not a UIKeyInput, so holding the focus shows no keyboard; it brings the hardware
// key presses here while no text field is being edited
- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (void)didMoveToWindow{
    [super didMoveToWindow];
    if (self.window && ![_textField isFirstResponder])
        [self becomeFirstResponder];
}

// Text input (System::showVirtualKeyboard / hideVirtualKeyboard / setTextInputArea).
// UIKit on the main thread, where the engine runs.

- (void)showTextInput{
    [self resetTextField];
    [_textField becomeFirstResponder];
}

- (void)hideTextInput{
    // The engine cancelled its composition before hiding: whatever UIKit commits
    // while the field resigns is not text
    _updatingField = YES;
    [_textField resignFirstResponder];
    _updatingField = NO;
    [self resetTextField];

    // Hardware keys come back to the view
    if (self.window)
        [self becomeFirstResponder];
}

- (void)setTextInputArea:(CGRect)area{
    // Touches are view points times the native scale (getTouchPoint:); this undoes it
    const CGFloat scale = [UIScreen mainScreen].nativeScale;
    if (scale <= 0.0)
        return;

    // At the bottom left of the caret, where an input method on a hardware keyboard
    // shows its candidates
    const CGFloat x = area.origin.x / scale;
    const CGFloat top = area.origin.y / scale;
    const CGFloat bottom = (area.origin.y + area.size.height) / scale;
    _textField.frame = CGRectMake(x, MAX(top, bottom - 1.0), 1.0, 1.0);
}

- (void)resetTextField{
    _updatingField = YES;
    _textField.text = textFieldSentinel;
    _updatingField = NO;
    _reportedText = textFieldSentinel;
    _composing = NO;
}

// A key the keyboard turned into an edit. A hardware key reported its own down and
// up (pressesBegan), so then only the character is left, for the component UI.
- (void)sendEditingKey:(int)key character:(char32_t)character{
    const bool hardware = _keysDown[key] || (key == D_KEY_ENTER && _keysDown[D_KEY_KP_ENTER]);
    if (!hardware)
        jasperengine::Engine::systemKeyDown(key, false, 0);
    jasperengine::Engine::systemCharInput(character);
    if (!hardware)
        jasperengine::Engine::systemKeyUp(key, false, 0);
}

- (BOOL)textField:(UITextField*)textField shouldChangeCharactersInRange:(NSRange)range replacementString:(NSString*)string{
    if (_updatingField)
        return YES;

    // The input method edits its composition in the field; textFieldTextDidChange:
    // reports it
    if (textField.markedTextRange != nil)
        return YES;

    if ([string length] == 0){
        // Deleting the sentinel: a backspace, and the field keeps its sentinel
        if (range.length > 0)
            [self sendEditingKey:D_KEY_BACKSPACE character:L'\b'];
        return NO;
    }

    // UITextField reports Return through textFieldShouldReturn:
    if ([string isEqualToString:@"\n"])
        return NO;

    // Typed text, or the first character of a composition: they cannot be told apart
    // before the field changes, so textFieldTextDidChange: reports either, and puts
    // the sentinel back
    return YES;
}

- (BOOL)textFieldShouldReturn:(UITextField*)textField{
    (void)textField;
    [self sendEditingKey:D_KEY_ENTER character:L'\r'];
    return NO;
}

// The caret stays after the sentinel, so a backspace always has it to delete and
// text never lands in front of it
- (void)textFieldDidChangeSelection:(UITextField*)textField{
    if (_updatingField || textField.markedTextRange != nil)
        return;

    // A composition that ended without a text change still has to be committed.
    // Checked again on the next turn of the run loop, so a selection change in the
    // middle of an input method's update is not taken for the end.
    if (_composing){
        __weak EngineView* weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            EngineView* view = weakSelf;
            if (view)
                [view commitEndedComposition];
        });
        return;
    }

    UITextPosition* end = textField.endOfDocument;
    UITextRange* selection = textField.selectedTextRange;
    if (selection && selection.empty && [textField comparePosition:selection.start toPosition:end] == NSOrderedSame)
        return;

    _updatingField = YES;
    textField.selectedTextRange = [textField textRangeFromPosition:end toPosition:end];
    _updatingField = NO;
}

- (void)commitEndedComposition{
    if (_composing && !_updatingField && _textField.markedTextRange == nil)
        [self textFieldTextDidChange:nil];
}

- (void)textFieldTextDidChange:(NSNotification*)notification{
    (void)notification;
    if (_updatingField)
        return;

    NSString* text = _textField.text ?: @"";
    UITextRange* marked = _textField.markedTextRange;

    if (marked != nil){
        UITextPosition* begin = _textField.beginningOfDocument;
        const NSInteger start = [_textField offsetFromPosition:begin toPosition:marked.start];
        const NSInteger end = [_textField offsetFromPosition:begin toPosition:marked.end];
        if (start < 0 || end < start || end > (NSInteger)[text length])
            return;

        // An input method can commit part of its composition and keep composing the
        // rest (Japanese, clause by clause): the engine confirms with that text
        NSString* prefix = [text substringToIndex:(NSUInteger)start];
        NSString* committed = addedText(_reportedText, prefix);
        if ([committed length] > 0){
            jasperengine::Engine::systemTextInput(utf8Of(committed));
            _composing = NO;
        }
        _reportedText = prefix;

        NSString* composition = [text substringWithRange:NSMakeRange((NSUInteger)start, (NSUInteger)(end - start))];
        if ([composition length] == 0){
            if (_composing)
                jasperengine::Engine::systemImeCancel();
            _composing = NO;
            return;
        }

        // The caret, from the start of the composition
        NSInteger caret = (NSInteger)[composition length];
        UITextRange* selection = _textField.selectedTextRange;
        if (selection)
            caret = [_textField offsetFromPosition:marked.start toPosition:selection.start];
        caret = MAX((NSInteger)0, MIN(caret, (NSInteger)[composition length]));

        _composing = YES;
        jasperengine::Engine::systemImeComposition(utf8Of(composition), codepointsBefore(composition, (NSUInteger)caret));
        return;
    }

    // No composition: what the field gained is committed text (typed, pasted,
    // dictated, or a composition's commit, which the engine reads as a confirm), and
    // a composition that vanished without any was cancelled
    NSString* committed = addedText(_reportedText, text);
    if ([committed length] > 0)
        jasperengine::Engine::systemTextInput(utf8Of(committed));
    else if (_composing)
        jasperengine::Engine::systemImeCancel();

    [self resetTextField];
}

// Hardware keyboard (iOS 13.4: UIPress.key)

- (void)hardwareKeyDown:(int)key modifiers:(int)mods{
    _keysDown[key] = true;

    // While a document text field shows a composition, the keys typed into it are the
    // input method's (as on macOS, MacInputRouter); the document gets the composition
    // and the commit
    if (_textField.markedTextRange != nil && jasperengine::Engine::wantsTextComposition()){
        _imeKeys[key] = true;
        return;
    }
    // UIKit sends no presses for auto-repeat
    jasperengine::Engine::systemKeyDown(key, false, mods);
}

- (void)hardwareKeyUp:(int)key modifiers:(int)mods{
    _keysDown[key] = false;

    if (_imeKeys[key]){
        _imeKeys[key] = false;
        return;
    }
    jasperengine::Engine::systemKeyUp(key, false, mods);
}

// Presses without a key (or before iOS 13.4) go to unhandled
- (void)reportPresses:(NSSet<UIPress*>*)presses down:(BOOL)down unhandled:(NSMutableSet<UIPress*>*)unhandled{
    if (@available(iOS 13.4, *)){
        for (UIPress* press in presses){
            UIKey* uiKey = press.key;
            const int key = uiKey ? jasperengine::iosKeyFromHIDUsage((long)uiKey.keyCode) : D_KEY_UNKNOWN;
            if (key == D_KEY_UNKNOWN){
                [unhandled addObject:press];
                continue;
            }

            const int mods = jasperengine::iosKeyModifiers(uiKey.modifierFlags);
            if (down)
                [self hardwareKeyDown:key modifiers:mods];
            else
                [self hardwareKeyUp:key modifiers:mods];
        }
    }else{
        [unhandled unionSet:presses];
    }
}

// While the hidden field has the focus it reported the presses already; what UIKit
// does not consume there travels on up the responder chain, through this view.
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event{
    if ([_textField isFirstResponder]){
        [super pressesBegan:presses withEvent:event];
        return;
    }

    NSMutableSet<UIPress*>* unhandled = [NSMutableSet set];
    [self reportPresses:presses down:YES unhandled:unhandled];
    if ([unhandled count] > 0)
        [super pressesBegan:unhandled withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event{
    if ([_textField isFirstResponder]){
        [super pressesEnded:presses withEvent:event];
        return;
    }

    NSMutableSet<UIPress*>* unhandled = [NSMutableSet set];
    [self reportPresses:presses down:NO unhandled:unhandled];
    if ([unhandled count] > 0)
        [super pressesEnded:unhandled withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event{
    if ([_textField isFirstResponder]){
        [super pressesCancelled:presses withEvent:event];
        return;
    }

    NSMutableSet<UIPress*>* unhandled = [NSMutableSet set];
    [self reportPresses:presses down:NO unhandled:unhandled];
    if ([unhandled count] > 0)
        [super pressesCancelled:unhandled withEvent:event];
}

-(CGPoint)getTouchPoint:(UITouch*)touch{
    const CGPoint pointInView = [touch locationInView:self];
    const CGFloat scale = [UIScreen mainScreen].nativeScale;
    
    const CGFloat pointX = pointInView.x * scale;
    const CGFloat pointY = pointInView.y * scale;
    
    return CGPointMake(pointX, pointY);
}

-(NSInteger)getTouchId:(UITouch*)touch andRemove:(bool)remove{
    NSInteger next = -1;
    for (NSInteger i = 0; i < MAX_TOUCHES; i++) {
        if (touches[i] == touch){
            if (remove)
                touches[i] = NULL;
            return i+1;
        }
        if (next == -1 && touches[i] == NULL) {
            next = i;
        }
    }
    
    if (next != -1) {
        touches[next] = touch;
        return next+1;
    }
    
    return -1;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent*)event{
    [super touchesBegan:touches withEvent:event];
    for (UITouch *touch in touches) {
        const CGPoint point = [self getTouchPoint:touch];
        const int identifier = (int)[self getTouchId:touch andRemove:false];
        jasperengine::Engine::systemTouchStart(identifier, point.x, point.y);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent*)event{
    [super touchesMoved:touches withEvent:event];
    for (UITouch *touch in touches) {
        const CGPoint point = [self getTouchPoint:touch];
        const int identifier = (int)[self getTouchId:touch andRemove:false];
        jasperengine::Engine::systemTouchMove(identifier, point.x, point.y);
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent*)event{
    [super touchesEnded:touches withEvent:event];
    for (UITouch *touch in touches) {
        const CGPoint point = [self getTouchPoint:touch];
        const int identifier = (int)[self getTouchId:touch andRemove:true];
        jasperengine::Engine::systemTouchEnd(identifier, point.x, point.y);
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)cancelledTouches withEvent:(UIEvent*)event{
    [super touchesCancelled:cancelledTouches withEvent:event];
    int cancelledIds[MAX_TOUCHES];
    int cancelledCount = 0;
    bool anyHeld = false;
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (!touches[i])
            continue;
        if ([cancelledTouches containsObject:touches[i]]) {
            cancelledIds[cancelledCount++] = i + 1;
            touches[i] = NULL;
        } else {
            anyHeld = true;
        }
    }

    // Release native IDs before callbacks, preserving IDs for fingers still held.
    if (!anyHeld) {
        jasperengine::Engine::systemTouchCancel();
    } else {
        for (int i = 0; i < cancelledCount; i++)
            jasperengine::Engine::systemTouchCancel(cancelledIds[i]);
    }
}

@end
