// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 8, phase 2)
// - Purpose: the Camera Follow object (Lua `CameraFollow`, `object:getCameraFollow()`): reads and
//   writes a follower's settings; snap() places it on the next update without damping.
// - The constructor ADDS the component when the entity has none; Lua `object:getCameraFollow()`
//   returns nil when the GameObject has none.
// - target: a GameObject of the same scene or of the persistent (DontDestroyOnLoad) scene, or nil
//   (NULL_ENTITY): the follower holds. A target in any other scene is refused with a warning.
// - Main thread (ECS).

#ifndef CAMERAFOLLOW_H
#define CAMERAFOLLOW_H

#include "EntityHandle.h"
#include "component/CameraFollowComponent.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <string>

namespace jasperengine{

    class JASPERENGINE_API CameraFollow: public EntityHandle{
    public:
        // adds the Camera Follow component to the entity when it has none
        CameraFollow(Scene* scene, Entity entity);
        virtual ~CameraFollow();

        CameraFollow(const CameraFollow& rhs);
        CameraFollow& operator=(const CameraFollow& rhs);
        CameraFollow(CameraFollow&& rhs) noexcept;
        CameraFollow& operator=(CameraFollow&& rhs) noexcept;

        void snap();

        // a GameObject of this scene
        void setTarget(Entity target);
        // a GameObject of this scene or of the persistent DontDestroyOnLoad scene; false (and a
        // warning) for another scene, which cannot be followed
        bool setTarget(Scene* targetScene, Entity target);
        Entity getTarget() const;
        // the scene of getTarget(): this one, or the persistent scene
        Scene* getTargetScene() const;
        void setOffset(Vector3 offset);
        Vector3 getOffset() const;
        // "world", "target" or "target_yaw" (another value logs a warning and is ignored)
        void setOffsetSpace(const std::string& space);
        std::string getOffsetSpace() const;
        void setDamping(Vector3 damping);
        Vector3 getDamping() const;
        void setMaxSpeed(float maxSpeed);
        float getMaxSpeed() const;
        void setLookAt(bool lookAt);
        bool isLookAt() const;
        void setLookAtOffset(Vector3 offset);
        Vector3 getLookAtOffset() const;
        void setRotationDamping(float damping);
        float getRotationDamping() const;
        void setDeadZone(Vector2 deadZone);
        Vector2 getDeadZone() const;
        void setUseBounds(bool useBounds);
        bool isUseBounds() const;
        void setBoundsMin(Vector3 boundsMin);
        Vector3 getBoundsMin() const;
        void setBoundsMax(Vector3 boundsMax);
        Vector3 getBoundsMax() const;
        void setUnscaledTime(bool unscaled);
        bool isUnscaledTime() const;
        void setEnabled(bool enabled);
        bool isEnabled() const;

        static std::string offsetSpaceToString(FollowOffsetSpace space);
        static bool offsetSpaceFromString(const std::string& text, FollowOffsetSpace& out);
    };

}

#endif //CAMERAFOLLOW_H
