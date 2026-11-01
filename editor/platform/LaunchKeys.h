// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Whether Shift is held as the editor starts (docs/specs/S27-editor-tools.md R16: the Project
// Hub opens): GetAsyncKeyState on Windows, [NSEvent modifierFlags] on macOS, XQueryKeymap on X11.

namespace jasperengine::editor::platform {

    bool shiftHeld();

}
