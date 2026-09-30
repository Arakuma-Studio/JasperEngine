// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COLORTWEEN_H
#define COLORTWEEN_H

#include "Tween.h"

namespace jasperengine{
    class JASPERENGINE_API ColorTween: public Tween{

    public:
        ColorTween(Scene* scene);
        ColorTween(Scene* scene, Entity entity);

        void setAction(Vector3 startColor, Vector3 endColor, float duration, bool loop=false);
        void setAction(Vector4 startColor, Vector4 endColor, float duration, bool loop=false);
    };
}

#endif //COLORTWEEN_H