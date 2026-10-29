// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_ANIMATE_H
#define SPLINE_ANIMATE_H

#include "object/EntityHandle.h"
#include "component/SplineAnimateComponent.h"

#include <string>

namespace jasperengine{

    // A GameObject's Spline Animate (SplineAnimateComponent): Lua self.entity:getSplineAnimate(). Moves the GameObject along another GameObject's Spline in Play.
    class JASPERENGINE_API SplineAnimate: public EntityHandle{
    private:
        SplineAnimateComponent& component() const;

    public:
        SplineAnimate(Scene* scene, Entity entity);
        virtual ~SplineAnimate();

        SplineAnimate(const SplineAnimate& rhs);
        SplineAnimate& operator=(const SplineAnimate& rhs);
        SplineAnimate(SplineAnimate&& rhs) noexcept;
        SplineAnimate& operator=(SplineAnimate&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setSpline(Entity value);
        Entity getSpline() const;
        void setMethod(SplineAnimateMethod value);
        SplineAnimateMethod getMethod() const;
        void setDuration(float value);
        float getDuration() const;
        void setSpeed(float value);
        float getSpeed() const;
        void setLoop(SplineLoop value);
        SplineLoop getLoop() const;
        void setEasing(SplineEasing value);
        SplineEasing getEasing() const;
        void setAlignment(SplineAlignment value);
        SplineAlignment getAlignment() const;
        void setStartOffset(float value);
        float getStartOffset() const;
        void setLateralOffset(float value);
        float getLateralOffset() const;
        void setVerticalOffset(float value);
        float getVerticalOffset() const;
        void setPlayOnAwake(bool value);
        bool isPlayOnAwake() const;

        void play();
        void pause();
        // to the start offset, playing when it was
        void restart();
        bool isPlaying() const;
        // 0-1 progress of the current pass, before easing
        float getNormalizedTime() const;
        // world units along the spline now
        float getDistance() const;
        void setDistance(float distance);
        bool isCompleted() const;
        // Once: called when the end is reached (Lua: a:setOnComplete(function() ... end))
        FunctionSubscribe<void()>& getOnComplete();
    };

}

#endif //SPLINE_ANIMATE_H
