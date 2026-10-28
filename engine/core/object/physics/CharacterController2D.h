// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_CONTROLLER_2D_H
#define CHARACTER_CONTROLLER_2D_H

#include "object/EntityHandle.h"
#include "component/CharacterController2DComponent.h"

namespace jasperengine{

    // A GameObject's 2D CharacterController: Lua self.entity:getCharacterController2D() or
    // CharacterController2D(scene, entity). move/simpleMove act at once and write the Transform;
    // the runtime fields are those of the last move and the last physics step. Points, as 2D bodies.
    class JASPERENGINE_API CharacterController2D: public EntityHandle{
    private:
        CharacterController2DComponent& component() const;

    public:
        CharacterController2D(Scene* scene, Entity entity);
        virtual ~CharacterController2D();

        CharacterController2D(const CharacterController2D& rhs);
        CharacterController2D& operator=(const CharacterController2D& rhs);
        CharacterController2D(CharacterController2D&& rhs) noexcept;
        CharacterController2D& operator=(CharacterController2D&& rhs) noexcept;

        // the entity has the component
        bool isValid() const;

        // Slides the capsule by motion (points, no gravity) and returns CollisionFlags
        // (SIDES 1, ABOVE 2, BELOW 4)
        int move(const Vector2& motion);
        // Moves at speed (points per second; y ignored) with gravity of its own; returns isGrounded
        bool simpleMove(const Vector2& speed);

        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setCenter(const Vector2& center);
        Vector2 getCenter() const;
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
        void setPushStrength(float strength);
        float getPushStrength() const;
        void setPushBodies(bool push);
        bool isPushBodies() const;
        void setMovingPlatforms(bool moving);
        bool isMovingPlatforms() const;
        void setBitsFilter(uint16_t categoryBits, uint16_t maskBits);
        uint16_t getCategoryBits() const;
        uint16_t getMaskBits() const;

        bool isGrounded() const;
        bool isOnSteepSlope() const;
        int getCollisionFlags() const;
        Vector2 getVelocity() const;
        Vector2 getGroundNormal() const;
        Entity getGroundEntity() const;
        Vector2 getGroundVelocity() const;
    };

}

#endif //CHARACTER_CONTROLLER_2D_H
