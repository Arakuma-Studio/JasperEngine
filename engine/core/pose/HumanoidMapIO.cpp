// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/HumanoidMapIO.h"

#include "json.hpp"
#include "pose/PoseAnimationIO.h"

#include <cmath>

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
            if (it.key() != "format" && it.key() != "version" && it.key() != "bones"){
                warnings.push_back("unknown key '" + it.key() + "' ignored");
            }
        }

        auto bones = root.find("bones");
        if (bones == root.end()){
            return true;
        }
        if (!bones->is_object()){
            warnings.push_back("bones: expected an object, ignored");
            return true;
        }
        for (auto it = bones->begin(); it != bones->end(); ++it){
            HumanBone bone;
            if (!humanBoneFromName(it.key(), bone)){
                warnings.push_back("bones: unknown slot '" + it.key() + "' ignored");
            }else if (it->is_null()){
                continue;   // null = auto, the same as absent
            }else if (!it->is_string()){
                warnings.push_back("bones." + it.key() + ": expected a bone name string, ignored");
            }else{
                out.bones[boneIndex(bone)] = it->get<std::string>();
            }
        }
        return true;
    }

    std::string writeHumanoidOverrides(const HumanoidOverrides& overrides){
        std::string text = "{\n";
        text += std::string("  \"format\": ") + quote(HUMANOID_FORMAT) + ",\n";
        text += "  \"version\": " + std::to_string(HUMANOID_VERSION) + ",\n";
        if (overrides.empty()){
            text += "  \"bones\": {}\n";
        }else{
            text += "  \"bones\": {\n";
            bool first = true;
            for (int i = 0; i < HUMAN_BONE_COUNT; i++){
                if (!overrides.bones[i].has_value()){
                    continue;
                }
                text += first ? "" : ",\n";
                text += "    " + quote(humanBoneName(boneAt(i))) + ": " + quote(*overrides.bones[i]);
                first = false;
            }
            text += "\n  }\n";
        }
        text += "}\n";
        return text;
    }

    bool loadHumanoidOverridesFile(const std::string& path, HumanoidOverrides& out,
                                   std::vector<std::string>& warnings, std::string* error){
        std::string text;
        if (!readTextFile(path, text, error)){
            return false;
        }
        std::string parseError;
        if (!parseHumanoidOverrides(text, out, warnings, &parseError)){
            return fail(error, path + ": " + parseError);
        }
        return true;
    }

    bool saveHumanoidOverridesFile(const std::string& path, const HumanoidOverrides& overrides, std::string* error){
        return writeTextFile(path, writeHumanoidOverrides(overrides), error);
    }

}
