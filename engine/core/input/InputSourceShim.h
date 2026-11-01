// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTSOURCESHIM_H
#define INPUTSOURCESHIM_H

// SYNC4-ABSORB(input): upstream's on-screen controls (the native UI's OnScreenStick / Button /
// TouchArea and the HTML UI's <onscreen-*> elements) fed upstream's Input Manager actions through
// per-owner sources (InputActions::setSourceValue / addSourceDelta / clearSource). S22 has no
// such sources; until the Input absorption maps the controls onto S22 (InputVirtualControls,
// inject), a value a control sets goes to S22's virtual/<action> control, deltas and the
// per-owner clearing do nothing. This header goes with that absorption.

#include "input/InputActions.h"

#include <cstdint>
#include <string>

namespace jasperengine::sync4input {

    inline void setSourceValue(uintptr_t /*owner*/, const std::string& action, float value){
        if (!action.empty())
            InputActions::setVirtual(action, value);
    }

    inline void addSourceDelta(uintptr_t /*owner*/, const std::string& /*action*/, float /*delta*/){}

    inline void clearSource(uintptr_t /*owner*/){}

    // upstream: the action has a mouse-movement binding (the mouse already moves it)
    inline bool mouseBound(const std::string& /*action*/){ return false; }

}

#endif //INPUTSOURCESHIM_H
