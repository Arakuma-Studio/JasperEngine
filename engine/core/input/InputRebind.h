// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTREBIND_H
#define INPUTREBIND_H

// Interactive rebinding (InputActions::startRebind, R15): the next control the player actuates
// in a frame's window, as a control path. A key, a mouse button or a gamepad button pressed in
// the window, or a gamepad axis crossing 0.5 (a held stick has to come back first).

#include "Export.h"
#include "Input.h"
#include "input/InputActions.h"

#include <functional>
#include <string>

namespace jasperengine {

    struct JASPERENGINE_API InputRebindState {
        bool active = false;
        std::string action;          // "Map/Action"
        std::string bindingId;
        InputRebindOptions options;
        std::function<void(const InputRebindResult&)> callback;
        float elapsed = 0.0f;
        // a stick binding is rebound to a stick, an axis to an axis
        bool wantsStick = false;
        float axisBaseline[D_INPUT_MAX_GAMEPADS][Input::GAMEPAD_AXES] = {};
    };

    class JASPERENGINE_API InputRebind {
    public:
        // the axes as they are now: only a crossing after this counts
        static void begin(InputRebindState& state);
        // the window's candidate: COMPLETED with its path, CANCELED (the cancel control),
        // TIMED_OUT, or false when nothing happened yet. Advances the timeout by dt.
        static bool scan(InputRebindState& state, float dt, InputRebindResult& result);
        // a candidate path the options allow (groups, exclusions)
        static bool accepts(const InputRebindOptions& options, const std::string& path);
    };

}

#endif // INPUTREBIND_H
