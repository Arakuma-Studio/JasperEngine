// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TWEEN_COMPONENT_H
#define TWEEN_COMPONENT_H

#include "action/Ease.h"
#include "math/Curve.h"

namespace jasperengine{

    struct JASPERENGINE_API TweenComponent{
        float time = 0;
        float value = 0;
        
        float duration = 0;
        bool loop = false;
        bool pingPong = false; // plays forward and then back, in twice the duration
        // advances with the unscaled frame delta: runs at Engine time scale 0 (a pause menu fade)
        bool useUnscaledTime = false;

        Ease function;
        // the ease when function is EaseType::CURVE: value = curve.evaluate(time)
        Curve curve = Curve::linear(0.0f, 0.0f, 1.0f, 1.0f);

        // a script's ease function, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) { f(function); }
    };

}

#endif //TWEEN_COMPONENT_H