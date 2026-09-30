// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COLORTWEEN_COMPONENT_H
#define COLORTWEEN_COMPONENT_H

#include "math/Vector3.h"

namespace jasperengine{

    struct JASPERENGINE_API ColorTweenComponent{
        Vector3 endColor;
        Vector3 startColor;

        bool useSRGB = true;
    };

}

#endif //COLORTWEEN_COMPONENT_H