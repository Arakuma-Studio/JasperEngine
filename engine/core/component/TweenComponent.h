// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TWEEN_COMPONENT_H
#define TWEEN_COMPONENT_H

#include "action/Ease.h"

namespace jasperengine{

    struct JASPERENGINE_API TweenComponent{
        float time = 0;
        float value = 0;
        
        float duration = 0;
        bool loop = false;
        bool pingPong = false; // plays forward and then back, in twice the duration

        Ease function;
    };

}

#endif //TWEEN_COMPONENT_H