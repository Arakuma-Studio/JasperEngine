// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where a Joint 2D's anchors, axis and target are in the world, as PhysicsSystem builds the joint
// from the bodies' Transforms (LOCAL: each body's model matrix, scale included; WORLD: the points
// as stored), for the Scene view gizmos and the anchor handles the Move tool drags.

#pragma once

#include "Scene.h"
#include "component/Joint2DComponent.h"
#include "component/Transform.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cmath>

namespace jasperengine::editor {

    struct Joint2DAnchorsWorld {
        Vector3 anchorA;
        Vector3 anchorB;
        // the handle of anchor B drags its own point (else it follows anchor A or is the target)
        bool anchorBEditable = false;
        // WHEEL / PRISMATIC: the unit line of motion in the world
        Vector3 axis = Vector3(1.0f, 0.0f, 0.0f);
        bool hasAxis = false;
        // bodyA's world angle (radians), the frame of revolute limits
        float bodyAAngle = 0.0f;
    };

    inline Transform* joint2DBodyTransform(Scene* scene, Entity body){
        return body != NULL_ENTITY ? scene->findComponent<Transform>(body) : nullptr;
    }

    inline Joint2DAnchorsWorld joint2DAnchorsWorld(Scene* scene, const Joint2DComponent& joint){
        Joint2DAnchorsWorld out;
        Transform* transformA = joint2DBodyTransform(scene, joint.bodyA);
        Transform* transformB = joint2DBodyTransform(scene, joint.bodyB);
        const Vector3 originA = transformA ? transformA->worldPosition : Vector3::ZERO;
        const Vector3 originB = transformB ? transformB->worldPosition : Vector3::ZERO;
        const float z = originA.z;
        if (transformA){
            // the roll of the body's world rotation
            const Vector3 right = transformA->worldRotation.getRotationMatrix() * Vector3(1.0f, 0.0f, 0.0f);
            out.bodyAAngle = std::atan2(right.y, right.x);
        }

        auto local = [z](Transform* transform, const Vector2& point){
            if (!transform) return Vector3(point.x, point.y, z);
            return transform->modelMatrix * Vector3(point.x, point.y, 0.0f);
        };

        const bool usesAnchors = joint.type != Joint2DType::MOUSE && joint.type != Joint2DType::MOTOR;
        if (joint.anchorSpace == JointAnchorSpace::LOCAL){
            out.anchorA = usesAnchors ? local(transformA, joint.anchorA) : originA;
            if (!usesAnchors){
                out.anchorB = joint.type == Joint2DType::MOUSE ? Vector3(joint.target.x, joint.target.y, z) : originB;
            }else if (joint.type == Joint2DType::DISTANCE || !joint.autoAnchors){
                out.anchorB = local(transformB, joint.anchorB);
                out.anchorBEditable = true;
            }else{
                out.anchorB = out.anchorA;
            }
        }else{
            switch (joint.type){
                case Joint2DType::DISTANCE:
                    if (joint.autoAnchors){
                        out.anchorA = originA;
                        out.anchorB = originB;
                    }else{
                        out.anchorA = Vector3(joint.anchorA.x, joint.anchorA.y, z);
                        out.anchorB = Vector3(joint.anchorB.x, joint.anchorB.y, z);
                        out.anchorBEditable = true;
                    }
                    break;
                case Joint2DType::MOUSE:
                    out.anchorA = originA;
                    out.anchorB = Vector3(joint.target.x, joint.target.y, z);
                    break;
                case Joint2DType::MOTOR:
                    out.anchorA = originA;
                    out.anchorB = originB;
                    break;
                default:
                    out.anchorA = out.anchorB = Vector3(joint.anchorA.x, joint.anchorA.y, z);
                    break;
            }
        }

        if (joint.type == Joint2DType::WHEEL || joint.type == Joint2DType::PRISMATIC){
            Vector2 axis = joint.axis;
            if (axis.x * axis.x + axis.y * axis.y < 1.0e-6f) axis = Vector2(1.0f, 0.0f);
            Vector3 world(axis.x, axis.y, 0.0f);
            if (joint.anchorSpace == JointAnchorSpace::LOCAL && transformA){
                world = transformA->modelMatrix * Vector3(axis.x, axis.y, 0.0f) - transformA->modelMatrix * Vector3(0.0f, 0.0f, 0.0f);
                world.z = 0.0f;
            }
            const float length = std::sqrt(world.x * world.x + world.y * world.y);
            out.axis = length > 0.0f ? world * (1.0f / length) : Vector3(1.0f, 0.0f, 0.0f);
            out.hasAxis = true;
        }
        return out;
    }

    // The point a handle drag ends at, as the joint stores it (local to its body or world)
    inline Vector2 joint2DAnchorFromWorld(Scene* scene, const Joint2DComponent& joint, bool anchorB, const Vector3& world){
        if (joint.anchorSpace == JointAnchorSpace::WORLD || (joint.type == Joint2DType::MOUSE && anchorB)){
            return Vector2(world.x, world.y);
        }
        Transform* transform = joint2DBodyTransform(scene, anchorB ? joint.bodyB : joint.bodyA);
        if (!transform){
            return Vector2(world.x, world.y);
        }
        const Vector3 local = transform->modelMatrix.inverse() * world;
        return Vector2(local.x, local.y);
    }

}
