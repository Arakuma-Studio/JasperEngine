// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/HumanoidMapIO.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jasperengine::pose {

    namespace {

        using Json = nlohmann::json;

        bool fail(std::string* error, const std::string& message){
            if (error){
                *error = message;
            }
            return false;
        }

        std::string quote(const std::string& text){
            return Json(text).dump(-1, ' ', false, Json::error_handler_t::replace);
        }

        std::string number(float value){
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.6g", std::isfinite(value) ? value : 0.0f);
            const std::string text = buffer;
            return text == "-0" ? "0" : text;
        }

        // an object of name -> bone name overrides ("" forces unmapped, null = auto)
        template<typename FromName>
        void readNames(const Json& object, const char* key, FromName fromName, std::vector<std::string>& warnings){
            if (!object.is_object()){
                warnings.push_back(std::string(key) + ": expected an object, ignored");
                return;
            }
            for (auto it = object.begin(); it != object.end(); ++it){
                fromName(it.key(), *it);
            }
        }

    }

    const char* animationTypeName(AnimationType type){
        return type == AnimationType::Humanoid ? "humanoid" : "generic";
    }

    bool animationTypeFromName(const std::string& name, AnimationType& out){
        if (name == "generic"){
            out = AnimationType::Generic;
            return true;
        }
        if (name == "humanoid"){
            out = AnimationType::Humanoid;
            return true;
        }
        return false;
    }

    std::string humanoidSidecarPath(const std::string& modelPath){
        return modelPath + ".humanoid";
    }

    bool parseHumanoidOverrides(const std::string& text, HumanoidOverrides& out,
                                std::vector<std::string>& warnings, std::string* error){
        out = HumanoidOverrides();

        Json root = Json::parse(text, nullptr, false);
        if (root.is_discarded()){
            return fail(error, "not valid JSON");
        }
        if (!root.is_object()){
            return fail(error, "not a humanoid sidecar: the top level must be a JSON object");
        }
        auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != HUMANOID_FORMAT){
            return fail(error, std::string("not a humanoid sidecar: \"format\" must be \"") + HUMANOID_FORMAT + "\"");
        }
        auto version = root.find("version");
        if (version == root.end()){
            warnings.push_back("version: missing, assuming 1");
        }else{
            if (!version->is_number()){
                return fail(error, "\"version\" must be a number");
            }
            const double value = version->get<double>();
            if (value != std::floor(value) || value < 1.0){
                return fail(error, "invalid \"version\"");
            }
            if (value > HUMANOID_VERSION){
                return fail(error, "unsupported humanoid sidecar version " + std::to_string(static_cast<long long>(value)) +
                                   " (this engine reads versions up to " + std::to_string(HUMANOID_VERSION) + ")");
            }
        }

        for (auto it = root.begin(); it != root.end(); ++it){
            const std::string& key = it.key();
            if (key != "format" && key != "version" && key != "bones" && key != "animation_type" && key != "extra_bones" && key != "twist"){
                warnings.push_back("unknown key '" + key + "' ignored");
            }
        }

        auto type = root.find("animation_type");
        if (type != root.end()){
            AnimationType value = AnimationType::Generic;
            if (!type->is_string() || !animationTypeFromName(type->get<std::string>(), value)){
                warnings.push_back("animation_type: expected \"generic\" or \"humanoid\", ignored");
            }else{
                out.animationType = value;
            }
        }

        auto bones = root.find("bones");
        if (bones != root.end()){
            readNames(*bones, "bones", [&](const std::string& key, const Json& value){
                HumanBone bone;
                if (!humanBoneFromName(key, bone)){
                    warnings.push_back("bones: unknown slot '" + key + "' ignored");
                }else if (value.is_null()){
                    return true;   // null = auto, the same as absent
                }else if (!value.is_string()){
                    warnings.push_back("bones." + key + ": expected a bone name string, ignored");
                }else{
                    out.bones[boneIndex(bone)] = value.get<std::string>();
                }
                return true;
            }, warnings);
        }

        auto extra = root.find("extra_bones");
        if (extra != root.end()){
            readNames(*extra, "extra_bones", [&](const std::string& key, const Json& value){
                HumanExtraBone bone;
                if (!humanExtraBoneFromName(key, bone)){
                    warnings.push_back("extra_bones: unknown bone '" + key + "' ignored (fingers <side>_<thumb|index|middle|ring|little>_<proximal|intermediate|distal>, left_eye, right_eye, jaw)");
                }else if (value.is_null()){
                    return true;
                }else if (!value.is_string()){
                    warnings.push_back("extra_bones." + key + ": expected a bone name string, ignored");
                }else{
                    out.extraBones[(size_t)extraIndex(bone)] = value.get<std::string>();
                }
                return true;
            }, warnings);
        }

        auto twist = root.find("twist");
        if (twist != root.end()){
            readNames(*twist, "twist", [&](const std::string& key, const Json& value){
                HumanBone bone;
                if (!humanBoneFromName(key, bone)){
                    warnings.push_back("twist: unknown slot '" + key + "' ignored");
                }else if (value.is_null()){
                    return true;
                }else if (!value.is_number() || !std::isfinite(value.get<double>())){
                    warnings.push_back("twist." + key + ": expected degrees, ignored");
                }else{
                    double degrees = value.get<double>();
                    if (degrees < -HUMANOID_MAX_TWIST || degrees > HUMANOID_MAX_TWIST){
                        warnings.push_back("twist." + key + ": " + number((float)degrees) + " is outside -180..180, clamped");
                        degrees = std::min<double>(std::max<double>(degrees, -HUMANOID_MAX_TWIST), HUMANOID_MAX_TWIST);
                    }
                    out.twist[(size_t)boneIndex(bone)] = (float)degrees;
                }
                return true;
            }, warnings);
        }
        return true;
    }

    std::string writeHumanoidOverrides(const HumanoidOverrides& overrides){
        // version 1 (the phase 2 form, byte for byte) unless a version 2 field is set
        const bool v1 = overrides.onlyBones();
        std::string text = "{\n";
        text += std::string("  \"format\": ") + quote(HUMANOID_FORMAT) + ",\n";
        text += "  \"version\": " + std::to_string(v1 ? 1 : HUMANOID_VERSION) + ",\n";
        if (overrides.animationType.has_value()){
            text += std::string("  \"animation_type\": ") + quote(animationTypeName(*overrides.animationType)) + ",\n";
        }
        // one object member per set entry, in table order ("{}" when none)
        auto object = [&](const char* key, int count, auto name, auto value, bool last){
            std::string members;
            for (int i = 0; i < count; i++){
                const std::string item = value(i);
                if (item.empty()) continue;
                members += (members.empty() ? "" : ",\n") + std::string("    ") + quote(name(i)) + ": " + item;
            }
            std::string out = std::string("  ") + quote(key) + ": " + (members.empty() ? "{}" : "{\n" + members + "\n  }");
            return out + (last ? "\n" : ",\n");
        };
        bool hasExtra = false;
        for (const auto& bone : overrides.extraBones) hasExtra = hasExtra || bone.has_value();
        bool hasTwist = false;
        for (const auto& value : overrides.twist) hasTwist = hasTwist || value.has_value();
        text += object("bones", HUMAN_BONE_COUNT, [](int i){ return std::string(humanBoneName(boneAt(i))); },
                       [&](int i){ return overrides.bones[(size_t)i].has_value() ? quote(*overrides.bones[(size_t)i]) : std::string(); },
                       !hasExtra && !hasTwist);
        if (hasExtra){
            text += object("extra_bones", HUMAN_EXTRA_BONE_COUNT, [](int i){ return std::string(humanExtraBoneName(extraAt(i))); },
                           [&](int i){ return overrides.extraBones[(size_t)i].has_value() ? quote(*overrides.extraBones[(size_t)i]) : std::string(); },
                           !hasTwist);
        }
        if (hasTwist){
            text += object("twist", HUMAN_BONE_COUNT, [](int i){ return std::string(humanBoneName(boneAt(i))); },
                           [&](int i){ return overrides.twist[(size_t)i].has_value() ? number(*overrides.twist[(size_t)i]) : std::string(); },
                           true);
        }
        text += "}\n";
        return text;
    }

}
