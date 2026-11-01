// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The humanoid bones beside the 22 slots (spec: anim-retarget 6.3): 30 finger joints, the eyes
// and the jaw (Unity's other 33 HumanBodyBones). A map of their own, so the 22-slot pose
// contract (HumanBones.h) is untouched. Humanoid clips carry their rotations as rest deltas in the
// canonical frame; the pose tool does not animate them.
//
// The order is a contract (humanoid clip track sort order, MCP, tests): append only.

#ifndef POSE_HUMAN_EXTRA_BONES_H
#define POSE_HUMAN_EXTRA_BONES_H

#include "Export.h"
#include "pose/HumanBones.h"
#include "pose/HumanoidMap.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::pose {

    enum class HumanExtraBone : uint8_t {
        LeftThumbProximal = 0, LeftThumbIntermediate, LeftThumbDistal,
        LeftIndexProximal, LeftIndexIntermediate, LeftIndexDistal,
        LeftMiddleProximal, LeftMiddleIntermediate, LeftMiddleDistal,
        LeftRingProximal, LeftRingIntermediate, LeftRingDistal,
        LeftLittleProximal, LeftLittleIntermediate, LeftLittleDistal,
        RightThumbProximal, RightThumbIntermediate, RightThumbDistal,
        RightIndexProximal, RightIndexIntermediate, RightIndexDistal,
        RightMiddleProximal, RightMiddleIntermediate, RightMiddleDistal,
        RightRingProximal, RightRingIntermediate, RightRingDistal,
        RightLittleProximal, RightLittleIntermediate, RightLittleDistal,
        LeftEye, RightEye, Jaw,
        Count
    };

    constexpr int HUMAN_EXTRA_BONE_COUNT = static_cast<int>(HumanExtraBone::Count);
    constexpr int HUMAN_FINGER_BONE_COUNT = 30;

    struct HumanExtraBoneInfo {
        const char* name;       // snake_case, e.g. "left_index_proximal"
        HumanBone parent;       // the slot it hangs from: the side's hand, or the head
        HumanExtraBone mirror;  // left/right counterpart; itself for the jaw
        BodySide side;
        int finger;             // 0 thumb .. 4 little; -1 for the eyes and the jaw
        int joint;              // 0 proximal, 1 intermediate, 2 distal; -1 for the eyes and the jaw
    };

    JASPERENGINE_API const HumanExtraBoneInfo& humanExtraBoneInfo(HumanExtraBone bone);
    JASPERENGINE_API const char* humanExtraBoneName(HumanExtraBone bone);
    JASPERENGINE_API bool humanExtraBoneFromName(const std::string& name, HumanExtraBone& bone);

    inline int extraIndex(HumanExtraBone bone){ return static_cast<int>(bone); }
    inline HumanExtraBone extraAt(int index){ return static_cast<HumanExtraBone>(index); }

    // extra bone -> skeleton bone name ("" = unmapped)
    struct HumanExtraMap {
        std::array<std::string, HUMAN_EXTRA_BONE_COUNT> bones;

        int mappedCount(int first = 0, int count = HUMAN_EXTRA_BONE_COUNT) const {
            int mapped = 0;
            for (int i = first; i < first + count && i < HUMAN_EXTRA_BONE_COUNT; i++){
                if (!bones[(size_t)i].empty()) mapped++;
            }
            return mapped;
        }
        bool operator==(const HumanExtraMap& other) const { return bones == other.bones; }
        bool operator!=(const HumanExtraMap& other) const { return !(*this == other); }
    };

    // Auto-detect from bone names under the mapped hands and head (spec 6.3):
    // - fingers: descendants of the side's hand whose humanoidNameTokens name the finger
    //   (thumb/thb, index/pointer, middle, ring, little/pinky), ordered by depth along their
    //   chain: the first three are proximal, intermediate and distal. Tips (end, nub, tip tokens or
    //   a trailing ordinal 4) and metacarpals (metacarpal, meta, carpal tokens or ordinal 0) are
    //   skipped, so Mixamo's LeftHandThumb1/2/3 are proximal/intermediate/distal as in Unity;
    // - eyes: bones under the head with an eye token and a side token (not lid/brow/lash);
    // - jaw: a bone under the head with a jaw token.
    // Several candidates for one bone: the one nearer the root wins (then skeleton order), with a
    // note. Bones already mapped to a slot are never used.
    JASPERENGINE_API HumanExtraMap autoDetectExtraBones(const Skeleton& skeleton, const HumanoidMap& humanoid,
                                                        std::vector<std::string>* notes = nullptr);

    // true when every mapped name exists exactly once, no bone is used twice or is also a slot
    // bone, the fingers descend from their side's mapped hand and the eyes and the jaw from the
    // mapped head. `error` lists the problems.
    JASPERENGINE_API bool validateExtraBones(const Skeleton& skeleton, const HumanoidMap& humanoid, const HumanExtraMap& extra,
                                             std::string* error);

    // Auto-detect, then the sidecar's extra_bones overrides (a name forces the bone, "" unmaps it;
    // a bone forced elsewhere leaves the bone auto-detect gave it), then validation. `notes` gets
    // one line per override applied. False with `error` when an override names a bone the skeleton
    // does not have or the result is invalid.
    JASPERENGINE_API bool resolveExtraBones(const Skeleton& skeleton, const HumanoidMap& humanoid,
                                            const std::array<std::optional<std::string>, HUMAN_EXTRA_BONE_COUNT>& overrides,
                                            HumanExtraMap& out, std::string* error, std::vector<std::string>* notes = nullptr);

    // resolveExtraBones, or (when the overrides do not validate) auto-detect alone, `error` saying
    // why and a note: what the runtime and the editor both use, so a bad override shows and plays
    // the same everywhere. True when the overrides were used.
    JASPERENGINE_API bool resolveExtraBonesOrAuto(const Skeleton& skeleton, const HumanoidMap& humanoid,
                                                  const std::array<std::optional<std::string>, HUMAN_EXTRA_BONE_COUNT>& overrides,
                                                  HumanExtraMap& out, std::string* error, std::vector<std::string>* notes);


}

#endif // POSE_HUMAN_EXTRA_BONES_H
