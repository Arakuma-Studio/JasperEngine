// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ALPHATWEEN_H
#define ALPHATWEEN_H

#include "Tween.h"

namespace jasperengine{
    class JASPERENGINE_API AlphaTween: public Tween{

    public:
        AlphaTween(Scene* scene);
        AlphaTween(Scene* scene, Entity entity);

        void setAction(float startAlpha, float endAlpha, float duration, bool loop=false);
    };
}

#endif //ALPHATWEEN_H