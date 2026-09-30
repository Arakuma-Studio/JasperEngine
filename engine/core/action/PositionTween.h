// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef POSITIONTWEEN_H
#define POSITIONTWEEN_H

#include "Tween.h"

namespace jasperengine{
    class JASPERENGINE_API PositionTween: public Tween{

    public:
        PositionTween(Scene* scene);
        PositionTween(Scene* scene, Entity entity);

        void setAction(Vector3 startPosition, Vector3 endPosition, float duration, bool loop=false);
    };
}

#endif //POSITIONTWEEN_H