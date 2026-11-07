// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BUTTON_COMPONENT_H
#define BUTTON_COMPONENT_H

#include "util/FunctionSubscribe.h"
#include "texture/Texture.h"

namespace jasperengine{

    struct JASPERENGINE_API ButtonComponent{
        Entity label = NULL_ENTITY;

        Texture textureNormal;
        Texture textureHovered;
        Texture texturePressed;
        Texture textureDisabled;

        Vector4 colorNormal = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color
        Vector4 colorHovered = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color
        Vector4 colorPressed = Vector4(1.0, 1.0, 1.0, 1.0);  //linear color
        // Unity ColorBlock.disabledColor: sRGB (200, 200, 200, 128), stored linear. Also shown
        // under a non-interactable Canvas Group (ui canvas group spec decision 8)
        Vector4 colorDisabled = Vector4(0.57758f, 0.57758f, 0.57758f, 0.50196f);  //linear color

        FunctionSubscribe<void()> onPress;
        FunctionSubscribe<void()> onRelease;

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(onPress); f(onRelease); }

        bool pressed = false;
        bool hovered = false;
        bool disabled = false;

        bool needUpdateButton = true;
    };

}

#endif //BUTTON_COMPONENT_H