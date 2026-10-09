// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOINT2D_H
#define JOINT2D_H

#include "EntityHandle.h"

namespace jasperengine{

    class JASPERENGINE_API Joint2D: public EntityHandle{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Joint2D is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Joint2D(Scene* scene);
        Joint2D(Scene* scene, Entity entity);
        virtual ~Joint2D();

        Joint2D(const Joint2D& rhs);
        Joint2D& operator=(const Joint2D& rhs);
        Joint2D(Joint2D&& rhs) noexcept;
        Joint2D& operator=(Joint2D&& rhs) noexcept;

        b2JointId getBox2DJoint() const;

        void setDistanceJoint(Entity bodyA, Entity bodyB);
        void setDistanceJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchorOnBodyA, Vector2 worldAnchorOnBodyB, bool rope);
        void setRevoluteJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor);
        void setPrismaticJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor, Vector2 worldAxis);
        void setMouseJoint(Entity bodyA, Entity bodyB, Vector2 target);
        void setWheelJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor, Vector2 worldAxis);
        void setWeldJoint(Entity bodyA, Entity bodyB, Vector2 worldAnchor);
        void setMotorJoint(Entity bodyA, Entity bodyB);

        Joint2DType getType();
    };
}

#endif //RIGIDBODY2D_H