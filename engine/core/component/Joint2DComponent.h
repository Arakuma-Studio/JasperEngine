// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JOINT2D_COMPONENT_H
#define JOINT2D_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "box2d/box2d.h"
#endif

namespace jasperengine{

    enum class Joint2DType{
        DISTANCE,
        REVOLUTE,
        PRISMATIC,
        //PULLEY,
        //GEAR,
        MOUSE,
        WHEEL,
        WELD,
        MOTOR
    };

    struct JASPERENGINE_API Joint2DComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

#ifdef JASPERENGINE_PHYSICS_2D
        b2JointId joint = b2_nullJointId;
#endif
        Joint2DType type = Joint2DType::DISTANCE;

        Entity bodyA = NULL_ENTITY;
        Entity bodyB = NULL_ENTITY;

        Vector2 anchorA = Vector2::ZERO;
        Vector2 anchorB = Vector2::ZERO;
        Vector2 axis = Vector2::ZERO;
        Vector2 target = Vector2::ZERO;

        bool autoAnchors = true;
        bool rope = false;
        bool needUpdateJoint = true;
    };

}

#endif //JOINT2D_COMPONENT_H
