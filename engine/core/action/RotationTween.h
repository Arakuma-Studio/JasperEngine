// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ROTATIONTWEEN_H
#define ROTATIONTWEEN_H

#include "Tween.h"

namespace jasperengine{
    class JASPERENGINE_API RotationTween: public Tween{

    public:
        RotationTween(Scene* scene);
        RotationTween(Scene* scene, Entity entity);

        void setAction(Quaternion startRotation, Quaternion endRotation, float duration, bool loop=false);
    };
}

#endif //ROTATIONTWEEN_H