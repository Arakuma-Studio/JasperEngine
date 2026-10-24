// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TWEEN_H
#define TWEEN_H

#include "Action.h"
#include "Ease.h"

namespace jasperengine{
    class JASPERENGINE_API Tween: public Action{

    protected:
        void setAction(float duration, bool loop);

    public:
        Tween(Scene* scene);
        Tween(Scene* scene, Entity entity);

        void setFunctionType(EaseType functionType);

        float getValue() const;
        float getTime() const;

        void setDuration(float duration);
        float getDuration() const;

        void setLoop(bool loop);
        bool isLoop() const;

        // plays forward and then back, in twice the duration
        void setPingPong(bool pingPong);
        bool isPingPong() const;
    };
}

#endif //TWEEN_H