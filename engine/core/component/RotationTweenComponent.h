// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ROTATIONTWEEN_COMPONENT_H
#define ROTATIONTWEEN_COMPONENT_H

#include "math/Quaternion.h"
#include "math/Vector3.h"

namespace jasperengine{

    struct JASPERENGINE_API RotationTweenComponent{
        Quaternion endRotation;
        Quaternion startRotation;

        bool shortestPath = false;

        // turns startRotation by angle (degrees, any size) around axis instead of reaching endRotation
        bool spin = false;
        Vector3 axis = Vector3(0, 1, 0);
        float angle = 360;
    };

}

#endif //ROTATIONTWEEN_COMPONENT_H