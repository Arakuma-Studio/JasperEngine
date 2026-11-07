// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ACTION_H
#define ACTION_H

#include "object/EntityHandle.h"
#include "object/GameObject.h"

namespace jasperengine{
    class JASPERENGINE_API Action: public EntityHandle{
    public:
        Action(Scene* scene);
        Action(Scene* scene, Entity entity);
        virtual ~Action();

        Action(const Action& rhs);
        Action& operator=(const Action& rhs);
        Action(Action&& rhs) noexcept;
        Action& operator=(Action&& rhs) noexcept;

        void start();
        void pause();
        void stop();
        // stops a running or paused action and starts it again from the beginning
        void restart();

        void setOwnedTarget(bool ownedTarget);
        bool getOwnedTarget() const;

        void setTarget(GameObject* target);
        void setTarget(Entity target);
        Entity getTarget() const;

        void setSpeed(float speed);
        float getSpeed() const;

        void setWeight(float weight);
        float getWeight() const;

        // Seconds into the action each new run starts at (Play mode and builds; a negative or
        // non-finite value becomes 0 with a warning)
        void setStartOffset(float startOffset);
        float getStartOffset() const;
        // Each new run starts at a random point of one cycle (overrides the start offset)
        void setRandomStart(bool randomStart);
        bool isRandomStart() const;

        bool isRunning() const;
        bool isStopped() const;
        bool isPaused() const;

        float getTimeCount() const;
    };
}

#endif //ACTION_H