// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ROTATIONTWEEN_COMPONENT_H
#define ROTATIONTWEEN_COMPONENT_H

#include "math/Quaternion.h"

namespace jasperengine{

    struct JASPERENGINE_API RotationTweenComponent{
        Quaternion endRotation;
        Quaternion startRotation;

        bool shortestPath = false;
    };

}

#endif //ROTATIONTWEEN_COMPONENT_H