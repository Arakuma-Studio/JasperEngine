// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCALETWEEN_H
#define SCALETWEEN_H

#include "Tween.h"

namespace jasperengine{
    class JASPERENGINE_API ScaleTween: public Tween{

    public:
        ScaleTween(Scene* scene);
        ScaleTween(Scene* scene, Entity entity);

        void setAction(Vector3 startScale, Vector3 endScale, float duration, bool loop=false);
    };
}

#endif //SCALETWEEN_H