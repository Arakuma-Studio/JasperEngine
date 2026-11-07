// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BUTTON_COMPONENT_H
#define BUTTON_COMPONENT_H

#include "util/FunctionSubscribe.h"
#include "texture/Texture.h"
#include "component/UISelectable.h"

namespace jasperengine{

    struct JASPERENGINE_API ButtonComponent{
        Entity label = NULL_ENTITY;

        Texture textureNormal;
        Texture textureHovered;
        Texture texturePressed;
        Texture textureDisabled;
        Texture textureSelected;

        Vector4 colorNormal = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color
        Vector4 colorHovered = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color
        Vector4 colorPressed = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color
        // Unity ColorBlock.disabledColor: sRGB (200, 200, 200, 128), stored linear. Also shown
        // under a non-interactable Canvas Group (ui canvas group spec decision 8)
        Vector4 colorDisabled = Vector4(0.57758f, 0.57758f, 0.57758f, 0.50196f);  //linear color
        // white until set: the first load gives it the Highlighted colour (a coloured button never
        // turns white when selected)
        Vector4 colorSelected = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color

        // what the Button writes (ui-components spec decision 2); colours and scales ease over
        // fadeDuration seconds of the UI clock (0: at once)
        ButtonTransition transition = ButtonTransition::COLOR_TINT_AND_SPRITE_SWAP;
        float fadeDuration = 0.0f;
        // the Transform's scale is multiplied by these in their state (1: no change)
        float scaleHovered = 1.0f;
        float scalePressed = 1.0f;
        float scaleSelected = 1.0f;

        // keyboard, gamepad and TV navigation (Unity's Navigation)
        NavigationMode navigationMode = NavigationMode::AUTOMATIC;
        bool wrapAround = false;
        Entity selectOnUp = NULL_ENTITY;
        Entity selectOnDown = NULL_ENTITY;
        Entity selectOnLeft = NULL_ENTITY;
        Entity selectOnRight = NULL_ENTITY;
        // becomes its scene's selection when it comes on (Unity's firstSelectedGameObject, HTML autofocus)
        bool selectOnEnable = false;

        FunctionSubscribe<void()> onPress;
        FunctionSubscribe<void()> onRelease;
        // the click: released over it, or a Submit (Enter, Space, gamepad A) while selected
        FunctionSubscribe<void()> onClick;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onPress); f(onRelease); f(onClick); }

        bool pressed = false;
        bool hovered = false;
        bool disabled = false;

        bool needUpdateButton = true;

        // runtime, never saved: the scene's selection is this Button, a Submit holds it pressed,
        // it was live in the last update (Select On Enable), the state shown
        bool selected = false;
        bool submitPressed = false;
        bool wasLive = false;
        SelectionState state = SelectionState::NORMAL;
        // a running transition: the colour and scale it started from and when (UI clock)
        Vector4 fadeFromColor = Vector4(1.0, 1.0, 1.0, 1.0);
        float fadeFromScale = 1.0f;
        double fadeStart = 0.0;
        bool fading = false;
        // the scale factor applied to the Transform, and the transition last applied
        float appliedScale = 1.0f;
        ButtonTransition appliedTransition = ButtonTransition::COLOR_TINT_AND_SPRITE_SWAP;
    };

}

#endif //BUTTON_COMPONENT_H