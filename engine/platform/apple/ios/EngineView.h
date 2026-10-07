// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import <MetalKit/MetalKit.h>

// The iOS Metal drawable. Text comes from a hidden UITextField the view owns (the
// approach SDL uses), which holds the keyboard focus while a text field of the game
// has it; hardware keys arrive through pressesBegan/pressesEnded (iOS 13.4).
@interface EngineView : MTKView

// System::showVirtualKeyboard and hideVirtualKeyboard
- (void)showTextInput;
- (void)hideTextInput;
// System::setTextInputArea: engine pixels from the top left, as touches report them
- (void)setTextInputArea:(CGRect)area;

@end
