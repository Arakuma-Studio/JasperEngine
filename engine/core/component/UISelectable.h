// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UI_SELECTABLE_H
#define UI_SELECTABLE_H

#include <cstdint>

namespace jasperengine{

    // Unity's Selectable.Transition, plus Jasper's combined mode (the default: what a Button did
    // before transitions existed). The mode decides what the Button writes: NONE nothing,
    // COLOR_TINT the UI colour, SPRITE_SWAP the UI texture, both for the combined one
    enum class ButtonTransition : uint8_t{
        NONE,
        COLOR_TINT,
        SPRITE_SWAP,
        COLOR_TINT_AND_SPRITE_SWAP
    };

    // Unity's Navigation.Mode
    enum class NavigationMode : uint8_t{
        NONE,
        HORIZONTAL,
        VERTICAL,
        AUTOMATIC,
        EXPLICIT
    };

    // Unity's Selectable.SelectionState
    enum class SelectionState : uint8_t{
        NORMAL,
        HIGHLIGHTED,
        PRESSED,
        SELECTED,
        DISABLED
    };

    // Unity's MoveDirection (its order)
    enum class MoveDirection : uint8_t{
        LEFT,
        UP,
        RIGHT,
        DOWN,
        NONE
    };

}

#endif //UI_SELECTABLE_H
