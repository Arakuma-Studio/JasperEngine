// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pose rigs for the editor (spec: pose animation phase 2, section 5): the rig (PoseRig + skin
// sample) of a model file, built from the model loaded off-scene into a private Scene and
// cached until the model file or its .humanoid sidecar changes; and the rig of a model
// GameObject of an open scene (built on that scene, with its bone entities, for baking).
// Both go through pose/ModelRig (buildSkeleton, buildSkin, resolveHumanoid).
//
// Main thread only.

#pragma once

#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "pose/HumanExtraBones.h"
#include "pose/HumanoidMap.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseRig.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class Project;

    struct PoseRigInfo{
        std::shared_ptr<pose::PoseRig> rig;     // null when the model is not a usable humanoid (error)
        std::string model;                      // project-relative model file
        std::string error;
        std::vector<std::string> notes;         // auto-detect notes and sidecar warnings
        pose::HumanoidMap humanoid;             // the resolved map (also when validation failed)
        std::vector<std::string> missing;       // required slots without a bone
        std::vector<std::string> boneNames;     // every skeleton bone (set_humanoid checks names against it)
        std::vector<Entity> boneEntities;       // scene rigs: the bone entity per skeleton bone
        // scene rigs: per skeleton bone, the non-bone ancestors folded into its rest TRS
        // (skeleton local = bridge * Transform local; ModelRig's buildSkeleton)
        std::vector<Matrix4> bridges;
        // the Avatar (anim-retarget): the sidecar as read, the extra bones (auto-detect +
        // overrides; `extraError` when an override did not validate: auto-detect is used)
        // `overrides` is set whenever the sidecar reads (also when the body map does not
        // validate); `sidecarError` when the sidecar exists but cannot be read (writers refuse
        // then: rewriting it from `overrides` would wipe the Avatar)
        pose::HumanoidOverrides overrides;
        std::string sidecarError;
        pose::HumanExtraMap extra;
        std::string extraError;
        pose::Skeleton skeleton;                // the model's skeleton (empty when it has none)
    };

    class PoseRigProvider{
    public:
        // The rig of a model file (absolute path), cached by the file's and its sidecar's stamps
        static std::shared_ptr<const PoseRigInfo> forModelFile(Project* project, const std::filesystem::path& model);
        // The rig of a model GameObject of `scene` (its loaded skeleton and skin, its model
        // file's sidecar). Not cached: the bone entities belong to the scene.
        static std::shared_ptr<const PoseRigInfo> forSceneModel(Project* project, Scene* scene, Entity model);
        // Drops every cached rig (project closed)
        static void clear();
    };

}
