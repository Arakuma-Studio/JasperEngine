// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSITIONTWEEN_COMPONENT_H
#define POSITIONTWEEN_COMPONENT_H

#include "math/Vector3.h"

namespace jasperengine{

    struct JASPERENGINE_API PositionTweenComponent{
        Vector3 endPosition;
        Vector3 startPosition;
    };

}

#endif //POSITIONTWEEN_COMPONENT_H