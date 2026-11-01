// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R1, R2, R14, D2, D4)
// - Purpose: an Animator's rig (the entities its clips animate, as bones in parents-first order
//   with their rest local TRS, plus float channels for morph weights and lattice offsets), the
//   binding of an upstream AnimationClip onto a rig, and the allocation-free sampler that writes
//   a clip at a time into a Pose.
// - Binding rules are the Animation Player's: tracks bind by path relative to the Animator's
//   entity (ClipBinding grammar), relative tracks combine with the rest value (position: rest +
//   value, rotation: rest * value, scale: rest * value), a track whose path does not resolve is
//   skipped, the root itself ("") is bone 0 of the rig when a track animates it (`rootBone`).
//   Anchored UI layouts are not animated through the Animator (no layout offset correction).
// - Rest values are taken from the scene before the Animator first writes an entity and kept in
//   the AnimatorRestMap across rebinds (the upstream rest rule).
// - The rig holds every ancestor of a bound entity below the root, so bones with parent -1 are
//   children of the root (rig space = the root's local space); the root bone, when present, is
//   bone 0 with parent -1 and is not an ancestor of anything in rig space.
// - Thread-safety: building reads the scene (main thread); sampling is pure and runs on workers.

#ifndef ANIMATOR_RIG_H
#define ANIMATOR_RIG_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimatorPose.h"
#include "animation/KeyframeSampling.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::anim{

    struct JASPERENGINE_API RigBone{
        Entity entity = NULL_ENTITY;
        int parent = -1;
        std::string path;           // relative to the root ("" for the root bone)
        std::string name;           // the entity name
        Entity parentEntity = NULL_ENTITY;  // its Transform parent at build
    };

    // a float channel of the rig: one morph weight, or one axis of one lattice point offset
    struct JASPERENGINE_API RigChannel{
        Entity entity = NULL_ENTITY;
        ClipChannel kind = ClipChannel::Morph;
        int index = 0;              // morph target, or flat lattice point
        int axis = 0;               // lattice: 0..2
    };

    // A ModelSkinBinding whose joints are all rig bones: its skin matrices come from the pose
    struct JASPERENGINE_API RigSkin{
        Entity model = NULL_ENTITY;         // the ModelComponent's entity
        size_t binding = 0;                 // index into ModelComponent::skinBindings
        Entity mesh = NULL_ENTITY;
        int meshBone = -1;                  // the mesh node's bone, -1 when it is not animated
        // when meshBone < 0: the mesh node and its ancestors up to (not including) the nearest
        // rig bone (meshParentBone) or the root (meshParentBone -1), mesh node first
        std::vector<Entity> meshChain;
        int meshParentBone = -1;
        std::vector<int> joints;            // rig bone per joint
        std::vector<Matrix4> inverseBindMatrices;
    };

    // rest values by entity (what the Animator blends from), kept across rebinds
    struct JASPERENGINE_API RigRest{
        bool hasTransform = false;
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        std::vector<float> morph;           // MAX_MORPHTARGETS when taken
        std::vector<Vector3> lattice;
    };
    using AnimatorRestMap = std::unordered_map<Entity, RigRest>;

    class JASPERENGINE_API AnimatorRig{
    public:
        std::vector<RigBone> bones;
        std::vector<int> parents;           // parallel to bones (the pose kernels read this array)
        std::vector<RigChannel> channels;
        Pose rest;                          // local rest TRS and channel rest values
        bool rootBone = false;              // bones[0] is the root itself
        std::vector<RigSkin> skins;
        size_t unresolved = 0;              // requested paths that did not resolve at build

        // every bone still exists under the same parent with the same name (what a hierarchy
        // change elsewhere in the scene leaves alone)
        bool stillValid(Scene* scene) const;

        int findBone(const std::string& path) const;            // -1 when not in the rig
        int findBone(Entity entity) const;
        // the first channel slot of (entity, kind, index); -1 when none
        int findChannel(Entity entity, ClipChannel kind, int index) const;
        // the mesh or lattice entity a channel track path names (NULL_ENTITY when none)
        Entity channelEntity(const std::string& path) const;
        // morph slots of an entity (0 when none)
        int morphWidth(Entity entity) const;
        bool empty() const { return bones.empty() && channels.empty(); }

        // Builds the rig of `root` from the paths it must hold (tracks of every clip, mask and IK
        // paths) and the skins of the ModelComponents on or under the root (their joints become
        // bones). Rest values come from `rest` (filled from the scene for new entities).
        void build(Scene* scene, Entity root, const std::vector<std::string>& paths,
                   const std::vector<std::pair<std::string, std::pair<ClipChannel, int>>>& channelPaths, AnimatorRestMap& rest);

    private:
        std::unordered_map<std::string, int> byPath;
        std::unordered_map<Entity, int> byEntity;
        std::unordered_map<uint64_t, int> channelSlots;
        std::unordered_map<std::string, Entity> channelPathEntities;
        std::unordered_map<Entity, int> morphWidths;
    };

    // One clip's tracks resolved on a rig (D4): tracks with the same key times, easings and loop
    // share one key search per sample
    struct JASPERENGINE_API RigClipBinding{
        struct Target{
            ClipChannel channel = ClipChannel::Position;
            bool relative = false;
            // bone (TRS), first morph slot, or the first of `width` entries of latticeSlots; -1 unbound
            int index = -1;
            int width = 0;              // morph / lattice values
            int group = 0;              // key-time group
            size_t track = 0;           // index into clip.getTracks()
        };
        struct Group{
            size_t track = 0;           // the track whose times/easings the group uses
        };
        std::shared_ptr<const AnimationClip> clip;
        std::vector<Target> targets;    // sampled tracks only, bound or not
        std::vector<Group> groups;
        std::vector<int> latticeSlots;  // per lattice point of a track: its first channel slot (-1 none)
        size_t boundCount = 0;
        size_t trackCount = 0;          // sampled tracks of the clip

        // `include` (per clip track, may be null): only the tracks it marks (a retargeted clip's
        // generic tracks); humanoid tracks never bind by path
        void bind(const std::shared_ptr<const AnimationClip>& clip, const AnimatorRig& rig, const std::vector<uint8_t>* include = nullptr);
    };

    // A sampling cursor set (one KeyCursor per group of a binding)
    struct JASPERENGINE_API RigClipCursors{
        struct Location{
            size_t base = 0;            // a shift of the key arrays (the last key past the end)
            KeyLocation at;
        };
        std::vector<KeyCursor> cursors;
        std::vector<Location> locations;
        void resize(size_t groups);
    };

    // Writes the clip at `clipTime` (seconds, already wrapped into the clip) into `out`, which
    // holds the rest pose (or another base) for everything the clip does not animate
    JASPERENGINE_API void sampleClip(const RigClipBinding& binding, const AnimatorRig& rig, float clipTime,
                                     RigClipCursors& cursors, Pose& out);

}

#endif //ANIMATOR_RIG_H
