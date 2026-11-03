// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .humanoid sidecar (spec: pose animation phase 2, section 2.2; anim-retarget 6.2), next to
// the model file ("models/hero.gltf.humanoid"): the model's Avatar. Only overrides are stored: a
// slot present with a bone name forces that mapping, a slot present with "" forces it unmapped,
// an absent slot uses auto-detect; the same for `extra_bones` (fingers, eyes, jaw). Version 2
// adds `animation_type` (generic | humanoid: a humanoid model serves its clips as humanoid
// clips), `extra_bones` and `twist` (per slot, degrees about the slot's neutral direction,
// applied to the neutral pose by PoseRig::build). A sidecar with only `bones` is still written
// as version 1, byte for byte as before. File access is in PoseFile.h. Applying the overrides to
// an auto-detected map is resolveHumanoid's job.

#ifndef HUMANOID_MAP_IO_H
#define HUMANOID_MAP_IO_H

#include "Export.h"
#include "pose/HumanBones.h"
#include "pose/HumanExtraBones.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::pose {

    constexpr const char* HUMANOID_FORMAT = "jasperengine.humanoid";
    constexpr int HUMANOID_VERSION = 2;
    // twist corrections are clamped to this (degrees, both ways)
    constexpr float HUMANOID_MAX_TWIST = 180.0f;

    // Unity's Rig tab "Animation Type" (Legacy is the Generic of the Animation Player)
    enum class AnimationType : uint8_t { Generic = 0, Humanoid };

    JASPERENGINE_API const char* animationTypeName(AnimationType type);    // "generic" / "humanoid"
    JASPERENGINE_API bool animationTypeFromName(const std::string& name, AnimationType& out);

    struct HumanoidOverrides {
        // per slot (HumanBone order): nullopt = auto-detect, "" = forced unmapped, else the bone name
        std::array<std::optional<std::string>, HUMAN_BONE_COUNT> bones;
        // nullopt = generic (not written)
        std::optional<AnimationType> animationType;
        // per extra bone (HumanExtraBone order): the same meaning as `bones`
        std::array<std::optional<std::string>, HUMAN_EXTRA_BONE_COUNT> extraBones;
        // per slot: degrees about the slot's neutral direction (nullopt = none)
        std::array<std::optional<float>, HUMAN_BONE_COUNT> twist;

        // no slot override (the other fields aside)
        bool empty() const {
            for (const auto& bone : bones){
                if (bone.has_value()){
                    return false;
                }
            }
            return true;
        }
        // only `bones` (or nothing) is set: written as version 1
        bool onlyBones() const {
            if (animationType.has_value()) return false;
            for (const auto& bone : extraBones){
                if (bone.has_value()) return false;
            }
            for (const auto& value : twist){
                if (value.has_value()) return false;
            }
            return true;
        }
        bool isHumanoid() const { return animationType == AnimationType::Humanoid; }
        // the twist corrections in degrees (0 for absent slots)
        std::array<float, HUMAN_BONE_COUNT> twistDegrees() const {
            std::array<float, HUMAN_BONE_COUNT> out{};
            for (int i = 0; i < HUMAN_BONE_COUNT; i++) out[(size_t)i] = twist[(size_t)i].value_or(0.0f);
            return out;
        }
        bool operator==(const HumanoidOverrides& other) const {
            return bones == other.bones && animationType == other.animationType && extraBones == other.extraBones && twist == other.twist;
        }
        bool operator!=(const HumanoidOverrides& other) const { return !(*this == other); }
    };

    // The sidecar path of a model file: "models/hero.gltf" -> "models/hero.gltf.humanoid"
    JASPERENGINE_API std::string humanoidSidecarPath(const std::string& modelPath);

    // Unknown slot and extra bone names, wrongly typed values and unknown keys are ignored with a
    // warning (twists outside -180..180 are clamped); text that is not JSON, a wrong `format` or a
    // `version` above HUMANOID_VERSION fails
    JASPERENGINE_API bool parseHumanoidOverrides(const std::string& text, HumanoidOverrides& out,
                                                 std::vector<std::string>& warnings, std::string* error);
    // Deterministic: slots in table order, 2-space indent, trailing newline; version 1 (the
    // phase 2 bytes) when only `bones` is set, else version 2 with animation_type, bones,
    // extra_bones and twist (each only when set)
    JASPERENGINE_API std::string writeHumanoidOverrides(const HumanoidOverrides& overrides);

}

#endif // HUMANOID_MAP_IO_H
