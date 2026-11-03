// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "platform/LaunchKeys.h"

#import <AppKit/AppKit.h>

bool jasperengine::editor::platform::shiftHeld() {
    return ([NSEvent modifierFlags] & NSEventModifierFlagShift) != 0;
}
