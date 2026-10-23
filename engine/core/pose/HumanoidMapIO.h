// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .humanoid sidecar (spec: pose animation phase 2, section 2.2), next to the model file
// ("models/hero.gltf.humanoid"). Only overrides are stored: a slot present with a bone name
// forces that mapping, a slot present with "" forces it unmapped, an absent slot uses
// auto-detect. Applying the overrides to an auto-detected map is resolveHumanoid's job.

#ifndef HUMANOID_MAP_IO_H
#define HUMANOID_MAP_IO_H

#include "Export.h"
#include "pose/HumanBones.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::pose {

    constexpr const char* HUMANOID_FORMAT = "jasperengine.humanoid";
    constexpr int HUMANOID_VERSION = 1;

    struct HumanoidOverrides {
        // per slot (HumanBone order): nullopt = auto-detect, "" = forced unmapped, else the bone name
        std::array<std::optional<std::string>, HUMAN_BONE_COUNT> bones;

        bool empty() const {
            for (const auto& bone : bones){
                if (bone.has_value()){
                    return false;
                }
            }
            return true;
        }
        bool operator==(const HumanoidOverrides& other) const { return bones == other.bones; }
        bool operator!=(const HumanoidOverrides& other) const { return !(*this == other); }
    };

    // The sidecar path of a model file: "models/hero.gltf" -> "models/hero.gltf.humanoid"
    JASPERENGINE_API std::string humanoidSidecarPath(const std::string& modelPath);

    // Unknown slot names, wrongly typed values and unknown keys are ignored with a warning;
    // text that is not JSON, a wrong `format` or a `version` above 1 fails
    JASPERENGINE_API bool parseHumanoidOverrides(const std::string& text, HumanoidOverrides& out,
                                                 std::vector<std::string>& warnings, std::string* error);
    // Deterministic: slots in table order, 2-space indent, trailing newline
    JASPERENGINE_API std::string writeHumanoidOverrides(const HumanoidOverrides& overrides);

    JASPERENGINE_API bool loadHumanoidOverridesFile(const std::string& path, HumanoidOverrides& out,
                                                    std::vector<std::string>& warnings, std::string* error);
    JASPERENGINE_API bool saveHumanoidOverridesFile(const std::string& path, const HumanoidOverrides& overrides,
                                                    std::string* error);

}

#endif // HUMANOID_MAP_IO_H
