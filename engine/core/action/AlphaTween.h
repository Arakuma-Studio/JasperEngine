// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ALPHATWEEN_H
#define ALPHATWEEN_H

#include "Tween.h"
#include "component/AlphaTweenComponent.h"

namespace jasperengine{
    class JASPERENGINE_API AlphaTween: public Tween{

    public:
        AlphaTween(Scene* scene);
        AlphaTween(Scene* scene, Entity entity);

        void setAction(float startAlpha, float endAlpha, float duration, bool loop=false);

        // what the tween writes: AUTO (the target's enabled Canvas Group, else its colour
        // alpha), COLOR or CANVAS_GROUP
        void setAlphaTarget(AlphaTweenTarget alphaTarget);
        AlphaTweenTarget getAlphaTarget() const;
    };
}

#endif //ALPHATWEEN_H