// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "platform/LaunchKeys.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

bool jasperengine::editor::platform::shiftHeld() {
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}
