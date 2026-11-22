// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A skeleton as the pose core sees it (rest local TRS of every bone) and the mapping of the 22
// humanoid slots to its bones, auto-detected from bone names (spec: pose animation phase 1,
// section 4.6). Overrides come from the .humanoid sidecar (phase 2, HumanoidMapIO).

#ifndef POSE_HUMANOID_MAP_H
#define POSE_HUMANOID_MAP_H

#include "Export.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "pose/HumanBones.h"

#include <array>
#include <string>
#include <vector>

namespace jasperengine::pose {

    // Rest local TRS, relative to the parent bone (model space for a root bone)
    struct SkeletonBone {
        std::string name;
        int parent = -1;
        Vector3 position;
        Quaternion rotation;
        Vector3 scale{1, 1, 1};
    };

    // Bones in any order; parent = index into bones or -1. Names must be unique for mapping.
    struct Skeleton {
        std::vector<SkeletonBone> bones;
    };

    // slot -> skeleton bone name ("" = unmapped)
    struct HumanoidMap {
        std::array<std::string, HUMAN_BONE_COUNT> bones;
    };

    // The auto-detect's naming rules, numbered by the change that made them: 1 is the original set;
    // 2 (HUMANOID_NAME_RULES_B_RIGS) also strips a leading "b-" (Kevin Iglesias' "B-hips" rigs) and
    // never maps a "proxy" bone. A model keeps the rules it was imported with: its .humanoid
    // sidecar's name_rules (HumanoidOverrides::nameRules), absent = 1, so a model imported before
    // the change keeps its status; the editor records the current rules on a new import or an
    // explicit reimport, where they change the model's map.
    constexpr int HUMANOID_NAME_RULES_ORIGINAL = 1;
    constexpr int HUMANOID_NAME_RULES_B_RIGS = 2;
    constexpr int HUMANOID_NAME_RULES = HUMANOID_NAME_RULES_B_RIGS;

    // Deterministic, case-insensitive auto-detect from bone names, then a structural pass (hips =
    // lowest common ancestor of both upper legs when no hips name matched). `notes`, when given,
    // receives one line per slot mapped by the structural pass and per ambiguity resolved.
    // Ambiguities (several bones matching one slot) go to the bone nearest the root (then the
    // first in skeleton order). Spine chains longer than three: Spine = first, Chest = the bone
    // at index n / 2, UpperChest = last before the neck. Body-part synonyms match the whole name
    // (minus side tokens: "LeftHandIndex1" is not a hand), except the spine*, neck* and toe*
    // prefixes. Besides the spec's helper tokens (twist, roll, helper, ik, pole, target, end,
    // nub), proxy (Unity's Spine Proxy bone, from rules 2) and finger tokens (index, middle, ring,
    // pinky, little, thumb, finger) are never mapped;
    // "c"/"center"/"centre" tokens are ignored (VRM "J_Bip_C_Hips"); an all-lower-case name
    // starting with "left"/"right" counts as sided ("leftarm"). Neck and head must descend from
    // the hips (head from the neck when there is one). When a mapped shoulder or upper arm
    // descends from the mapped neck (3ds Max biped), the neck is left unmapped (with a note).
    JASPERENGINE_API HumanoidMap autoDetectHumanoid(const Skeleton& skeleton, std::vector<std::string>* notes = nullptr,
                                                    int nameRules = HUMANOID_NAME_RULES);

    // true when every required slot is mapped, every mapped name exists exactly once in the
    // skeleton, no bone is used twice, the hierarchy is acyclic and every mapped slot descends
    // from its effective parent slot's bone with no other mapped slot in between. `error` lists
    // the problems (e.g. "missing required slots: left_hand, right_hand").
    JASPERENGINE_API bool validateHumanoid(const Skeleton& skeleton, const HumanoidMap& map, std::string* error);

    // Helpers shared with PoseRig and the IO/editor code
    JASPERENGINE_API int findSkeletonBone(const Skeleton& skeleton, const std::string& name);   // -1 when missing
    // the nearest mapped ancestor slot (HumanBone::Count for Hips or when none is mapped)
    JASPERENGINE_API HumanBone effectiveParentSlot(const HumanoidMap& map, HumanBone bone);

    // The name normalisation of the auto-detect, exposed for tests and tools: lower-case tokens
    // after stripping namespaces and rig prefixes and splitting camelCase/separators/digits.
    JASPERENGINE_API std::vector<std::string> humanoidNameTokens(const std::string& boneName, int nameRules = HUMANOID_NAME_RULES);
    // The auto-detect's reading of one name: true for a hips or spine-chain bone (hips, pelvis,
    // spine*, chest, upper chest, torso, after the same stripping: "B-hips", "mixamorig:Spine1"),
    // never for a proxy or helper. The torso test of rigs without a humanoid map (the Mesh Space
    // Rotation warnings).
    JASPERENGINE_API bool isTorsoBoneName(const std::string& boneName, int nameRules = HUMANOID_NAME_RULES);

}

#endif // POSE_HUMANOID_MAP_H
