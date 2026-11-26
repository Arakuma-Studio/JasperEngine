// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Engine.h"

namespace jasperengine {
    // TEST-ONLY seam (Engine.h befriends it): a view loaded without a GPU, the per-frame gamepad
    // step. One definition for the whole test binary (unity builds and the ODR)
    struct EngineTestAccess {
        static bool setViewLoaded(bool loaded) {
            const bool previous = Engine::viewLoaded;
            Engine::viewLoaded = loaded;
            return previous;
        }
        static void updateGamepadNavigation() { Engine::updateGamepadNavigation(); }
        static bool cursorMove(float x, float y) { return Engine::dispatchCursorPointer(Engine::CURSOR_MOVE, x, y); }
    };
}
