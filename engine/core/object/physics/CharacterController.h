// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_CONTROLLER_H
#define CHARACTER_CONTROLLER_H

#include "object/EntityHandle.h"
#include "component/CharacterControllerComponent.h"

namespace jasperengine{

    // A GameObject's CharacterController (Unity's): Lua self.entity:getCharacterController()
    // or CharacterController(scene, entity). move/simpleMove act at once and write the
    // Transform; the runtime fields are those of the last move and the last physics step.
    class JASPERENGINE_API CharacterController: public EntityHandle{
    private:
        CharacterControllerComponent& component() const;

    public:
        CharacterController(Scene* scene, Entity entity);
        virtual ~CharacterController();

        CharacterController(const CharacterController& rhs);
        CharacterController& operator=(const CharacterController& rhs);
        CharacterController(CharacterController&& rhs) noexcept;
        CharacterController& operator=(CharacterController&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        // Sweeps the capsule by motion (world units, no gravity) and returns CollisionFlags
        // (SIDES 1, ABOVE 2, BELOW 4)
        int move(const Vector3& motion);
        // Moves at speed (world units per second; y ignored) with gravity of its own;
        // returns isGrounded
        bool simpleMove(const Vector3& speed);
        // A new capsule (a crouch, a stand), refused when it would not fit there
        bool trySetSize(float height, float radius, const Vector3& center);

        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setCenter(const Vector3& center);
        Vector3 getCenter() const;
        void setHeight(float height);
        float getHeight() const;
        void setRadius(float radius);
        float getRadius() const;
        void setSlopeLimit(float degrees);
        float getSlopeLimit() const;
        void setStepOffset(float stepOffset);
        float getStepOffset() const;
        void setSkinWidth(float skinWidth);
        float getSkinWidth() const;
        void setMinMoveDistance(float distance);
        float getMinMoveDistance() const;
        void setMass(float mass);
        float getMass() const;
        void setPushStrength(float strength);
        float getPushStrength() const;
        void setPushBodies(bool push);
        bool isPushBodies() const;
        void setMovingPlatforms(bool moving);
        bool isMovingPlatforms() const;
        void setInheritPlatformYaw(bool inherit);
        bool isInheritPlatformYaw() const;
        void setDetectCollisions(bool detect);
        bool isDetectCollisions() const;
        void setCharacterCollisions(bool collide);
        bool isCharacterCollisions() const;
        void setEnhancedEdgeRemoval(bool enhanced);
        bool isEnhancedEdgeRemoval() const;
        void setBitsFilter(uint16_t categoryBits, uint16_t maskBits);
        uint16_t getCategoryBits() const;
        uint16_t getMaskBits() const;

        bool isGrounded() const;
        bool isOnSteepSlope() const;
        int getCollisionFlags() const;
        Vector3 getVelocity() const;
        Vector3 getGroundNormal() const;
        Entity getGroundEntity() const;
        Vector3 getGroundVelocity() const;
    };

}

#endif //CHARACTER_CONTROLLER_H
