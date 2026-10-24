// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pose rig of a loaded Model (spec: pose animation phase 2, section 3.1): its skeleton from
// the bone entities, a CPU skinning sample from the glTF data the ModelComponent keeps, the
// humanoid map (auto-detect, then the .humanoid sidecar's overrides, then validation) and a cache
// that shares rigs between the animators and previews of the same model.
//
// Model space is the Model entity's local space: bones whose Transform parent is not a bone (the
// glTF scene root node, an armature node) have those ancestors' current local transforms folded
// into their rest local TRS, and a bone under a non-bone node below its parent bone gets that
// node folded in as well. `bridges` (optional) gives, per skeleton bone, the folded transform
// (identity when there is none): skeleton local = bridge * Transform local.

#ifndef POSE_MODEL_RIG_H
#define POSE_MODEL_RIG_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "pose/HumanoidMap.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseRig.h"
#include "pose/PoseSkin.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::pose {

    // Skeleton of the Model entity `model`: one bone per bone entity (ModelComponent bones, glTF
    // node order), BoneComponent bind TRS as the rest local TRS, entity names as bone names,
    // parents from the Transform hierarchy restricted to bones. `boneEntities[i]` is the entity
    // of skeleton bone i. False when the entity has no Model or the Model has no bones yet.
    JASPERENGINE_API bool buildSkeleton(Scene* scene, Entity model, Skeleton& skeleton, std::vector<Entity>& boneEntities);
    JASPERENGINE_API bool buildSkeleton(Scene* scene, Entity model, Skeleton& skeleton, std::vector<Entity>& boneEntities,
                                        std::vector<Matrix4>* bridges);

    // CPU skinning sample of the Model's skinned meshes, from the glTF positions, JOINTS_0 and
    // WEIGHTS_0 the ModelComponent keeps loaded (at most PoseSkin::MAX_VERTICES vertices,
    // PoseSkin::sampleStride over all skinned vertices). Inverse bind matrices from the bones'
    // BoneComponent. nullptr when there is no glTF skin data (OBJ, no skinned mesh): the rig then
    // uses its sole estimate.
    JASPERENGINE_API std::shared_ptr<PoseSkin> buildSkin(Scene* scene, Entity model, const Skeleton& skeleton,
                                                         const std::vector<Entity>& boneEntities);

    // Auto-detect, then the sidecar overrides (a bone name forces the slot, "" unmaps it), then
    // validation. `notes` gets the auto-detect notes and one line per override applied. False
    // (with `error`) when an override names a bone the skeleton does not have or the map is not
    // a valid humanoid.
    JASPERENGINE_API bool resolveHumanoid(const Skeleton& skeleton, const HumanoidOverrides& overrides, HumanoidMap& out,
                                          std::string* error, std::vector<std::string>* notes = nullptr);

    // The sidecar's content in `stamp`, "" when there is none: what decides whether a cached rig
    // is current (the engine file API has no modification times in exported games, the text is
    // tiny). False with `error` when the sidecar exists but cannot be read (as the editor fails).
    JASPERENGINE_API bool humanoidSidecarStamp(const std::string& modelPath, std::string& stamp, std::string* error);

    // A cached rig is shared: read-only (const) for everyone
    struct JASPERENGINE_API ModelRigInfo {
        std::shared_ptr<const PoseRig> rig;    // built, skin set (when there is one)
        std::shared_ptr<const PoseSkin> skin;  // may be nullptr
        HumanoidMap humanoid;
        std::vector<std::string> notes;
    };

    // Rigs shared by model file and sidecar: get() returns the cached rig when the model path, the
    // sidecar stamp, the model's loaded data (`source`: the ModelComponent's gltfModel/objModel,
    // held weakly, so a reloaded file is new data) and the skeleton (names, parents, rest TRS)
    // all match, else builds it (resolveHumanoid + setSkin + PoseRig::build) outside the lock and
    // caches it. Entries whose source data is gone (model reloaded or unloaded) are dropped on
    // every get and by prune() (a scene's PoseAnimatorSystem prunes when the scene goes).
    // Thread safe. The editor clears it when a project closes.
    class JASPERENGINE_API PoseRigCache {
    public:
        static PoseRigCache& shared();

        // `skinFactory` builds the skin when a new rig is needed (may return nullptr); `source`
        // nullptr: no content identity (the path, stamp and skeleton decide)
        bool get(const std::string& modelPath, const std::string& sidecarStamp, const std::shared_ptr<const void>& source,
                 const Skeleton& skeleton, const HumanoidOverrides& overrides,
                 const std::function<std::shared_ptr<PoseSkin>()>& skinFactory, ModelRigInfo& out, std::string* error);
        bool get(const std::string& modelPath, const std::string& sidecarStamp, const Skeleton& skeleton,
                 const HumanoidOverrides& overrides, const std::function<std::shared_ptr<PoseSkin>()>& skinFactory,
                 ModelRigInfo& out, std::string* error);

        void prune();
        void clear();
        size_t size() const;

    private:
        struct Entry {
            std::string modelPath;
            std::string stamp;
            std::weak_ptr<const void> source;
            bool hasSource = false;
            Skeleton skeleton;
            ModelRigInfo info;
        };
        void pruneLocked();
        bool find(const std::string& modelPath, const std::string& sidecarStamp, const std::shared_ptr<const void>& source,
                  const Skeleton& skeleton, ModelRigInfo& out);
        mutable std::mutex mutex;
        std::vector<Entry> entries;
    };

    // The whole path for a Model entity of a scene: skeleton, sidecar (next to the model file),
    // cached rig. `modelPath` is the ModelComponent's file. False with `error` on any failure.
    JASPERENGINE_API bool buildModelRig(Scene* scene, Entity model, ModelRigInfo& out, std::vector<Entity>& boneEntities,
                                        std::vector<Matrix4>* bridges, std::string* error);

}

#endif // POSE_MODEL_RIG_H
