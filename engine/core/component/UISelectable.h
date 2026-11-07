// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UI_SELECTABLE_H
#define UI_SELECTABLE_H

namespace jasperengine{

    // (int-sized enums: the reflection and the cooked format read an enum field as an int)

    // Unity's Selectable.Transition, plus Jasper's combined mode (the default: what a Button did
    // before transitions existed). The mode decides what the Button writes: NONE nothing,
    // COLOR_TINT the UI colour, SPRITE_SWAP the UI texture, both for the combined one
    enum class ButtonTransition{
        NONE,
        COLOR_TINT,
        SPRITE_SWAP,
        COLOR_TINT_AND_SPRITE_SWAP
    };

    // Unity's Navigation.Mode
    enum class NavigationMode{
        NONE,
        HORIZONTAL,
        VERTICAL,
        AUTOMATIC,
        EXPLICIT
    };

    // Unity's Selectable.SelectionState
    enum class SelectionState{
        NORMAL,
        HIGHLIGHTED,
        PRESSED,
        SELECTED,
        DISABLED
    };

    // Unity's MoveDirection (its order)
    enum class MoveDirection{
        LEFT,
        UP,
        RIGHT,
        DOWN,
        NONE
    };

}

#endif //UI_SELECTABLE_H
