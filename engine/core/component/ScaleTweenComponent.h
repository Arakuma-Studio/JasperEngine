// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCALETWEEN_COMPONENT_H
#define SCALETWEEN_COMPONENT_H

#include "math/Vector3.h"

namespace jasperengine{

    struct JASPERENGINE_API ScaleTweenComponent{
        Vector3 endScale = Vector3::UNIT_SCALE;
        Vector3 startScale = Vector3::UNIT_SCALE;
    };

}

#endif //SCALETWEEN_COMPONENT_H