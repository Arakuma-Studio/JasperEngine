// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (physics-3d 6.4 "Ragdoll wizard", T13)
// - Purpose: Unity's Ragdoll Wizard (RagdollBuilder.cs, 2022.3) without the editor: from the
//   bone GameObjects of a character (pelvis, hips, knees, feet, arms, elbows, middle spine, head)
//   and their current world poses, the Rigidbody, collider and SWINGTWIST joint of every bone
//   and the Ragdoll component of the root. The editor's dialog, MCP create_ragdoll and the tests
//   all use it; applying the result is the caller's (the editor through undoable commands,
//   `apply` for engine-only code).
// - Bodies: masses = density x totalMass / (sum of the made bones' densities) (Unity's table: pelvis
//   and spine 2.5, hips and knees 1.5, arms, elbows and head 1; 20 kg in all by default), automatic
//   mass off, interpolated, kinematic when starting animated, else dynamic. Feet get no body.
// - Colliders, in bone-local units (world sizes divided by the bone's world scale): capsules from a
//   bone to its child bone (knees and elbows: mirrored, reaching their children), boxes for the
//   pelvis and the spine (the breast bounds split at the spine), a sphere for the head (radius a
//   quarter of the arm span).
// - Joints: on the child bone, bodyA = it, bodyB = the parent bone's body, LOCAL anchors (0,0,0),
//   automatic connected anchor, no collision between the two. The twist and swing axes are the
//   character's right / up / forward (up = pelvis to head, right = pelvis to right elbow,
//   forward = right x up, negated with flipForward) in the bone's frame, snapped to its nearest
//   local axis. Limits per Unity's table (degrees, the engine's angle unit in the component).
// - Missing optional bones (spine, head, arms, elbows) are skipped with a note; pelvis, hips and
//   knees are required. The output depends only on the bone poses and the settings (table order).
// - Main thread; reads Transforms only (world poses must be up to date).

#ifndef RAGDOLL_BUILDER_H
#define RAGDOLL_BUILDER_H

#include "Export.h"
#include "ecs/Entity.h"
#include "component/Joint3DComponent.h"
#include "component/RagdollComponent.h"
#include "component/RigidbodyComponent.h"

#include <string>
#include <vector>

namespace jasperengine{

    class Scene;

    // The wizard's bone slots (Unity's names)
    struct JASPERENGINE_API RagdollBones{
        Entity pelvis = NULL_ENTITY;
        Entity leftHips = NULL_ENTITY;
        Entity leftKnee = NULL_ENTITY;
        Entity leftFoot = NULL_ENTITY;
        Entity rightHips = NULL_ENTITY;
        Entity rightKnee = NULL_ENTITY;
        Entity rightFoot = NULL_ENTITY;
        Entity leftArm = NULL_ENTITY;
        Entity leftElbow = NULL_ENTITY;
        Entity rightArm = NULL_ENTITY;
        Entity rightElbow = NULL_ENTITY;
        Entity middleSpine = NULL_ENTITY;
        Entity head = NULL_ENTITY;

        // "pelvis", "left_hips", ... in the wizard's order; the slot's entity by that name
        static const std::vector<const char*>& slotNames();
        Entity& slot(const std::string& name);
        Entity slot(const std::string& name) const;
    };

    struct JASPERENGINE_API RagdollBuildSettings{
        float totalMass = 20.0f;
        bool flipForward = false;
        // false: animated (kinematic parts), true: simulated (dynamic parts)
        bool startSimulated = false;
        bool selfCollision = true;
    };

    struct JASPERENGINE_API RagdollBuiltPart{
        std::string slot;
        Entity entity = NULL_ENTITY;
        RigidbodyComponent body;
        bool hasJoint = false;
        Joint3DComponent joint;
        // "capsule", "box", "sphere"
        std::string collider;
        float mass = 0.0f;
    };

    struct JASPERENGINE_API RagdollBuildResult{
        bool ok = false;
        std::string error;
        std::vector<RagdollBuiltPart> parts;
        // the optional bones left out ("middle_spine: none given")
        std::vector<std::string> skipped;
        std::vector<std::string> warnings;
        RagdollComponent ragdoll;
    };

    class JASPERENGINE_API RagdollBuilder{
    public:
        // The bones of the character under root: its model's humanoid mapping (the model's
        // .humanoid sidecar, else the auto-detect from bone names: Mixamo "mixamorig:LeftUpLeg",
        // Unity "LeftUpperLeg", Blender "thigh.L"...). notes: how they were found.
        static RagdollBones guessBones(Scene* scene, Entity root, std::vector<std::string>* notes = nullptr);

        static RagdollBuildResult build(Scene* scene, Entity root, const RagdollBones& bones, const RagdollBuildSettings& settings);

        // Engine-only (Lua Ragdoll.create): the parts' components on their bones (replacing
        // their Rigidbody and Joint 3D) and the Ragdoll on root, all at once, so no step sees a
        // part without its joint. The editor applies a result through its commands instead.
        // False (and error) when nothing could be applied: a replacement while the scene defers
        // removals (a UI event handler) would leave the old components in place.
        static bool apply(Scene* scene, Entity root, const RagdollBuildResult& result, std::string* error = nullptr);
    };

}

#endif //RAGDOLL_BUILDER_H
