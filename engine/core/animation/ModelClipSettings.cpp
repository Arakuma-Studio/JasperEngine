// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "animation/ModelClipSettings.h"

#include "Log.h"
#include "animation/AnimationClipIO.h"
#include "pose/PoseFile.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"

#include <algorithm>
#include <mutex>

using namespace jasperengine;

namespace{

    using SidecarJson = nlohmann::ordered_json;

    const char* const rootMotionMembers[] = {"bone", "bake_rotation", "bake_position_y", "bake_position_xz"};

    bool failSidecar(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    std::string dumpMember(const SidecarJson& value){
        return value.dump(-1, ' ', false, SidecarJson::error_handler_t::replace);
    }

    void putUnknown(SidecarJson& object, const anim::RawJsonMembers& members){
        for (const auto& member : members){
            SidecarJson value = SidecarJson::parse(member.second, nullptr, false);
            if (!value.is_discarded()) object[member.first] = std::move(value);
        }
    }

    bool sameRootMotion(const std::optional<anim::ClipRootMotion>& a, const std::optional<anim::ClipRootMotion>& b){
        if (a.has_value() != b.has_value()) return false;
        if (!a) return true;
        return a->bone == b->bone && a->bakeRotation == b->bakeRotation && a->bakePositionY == b->bakePositionY &&
               a->bakePositionXZ == b->bakePositionXZ;
    }

    void readClipSetting(const std::string& name, const SidecarJson& value, anim::ModelClipSetting& out, std::vector<std::string>& warnings){
        const std::string where = "clips." + name;
        if (!value.is_object()){
            warnings.push_back(where + ": expected an object, ignored");
            return;
        }
        for (auto it = value.begin(); it != value.end(); ++it){
            if (it.key() == "wrap"){
                ClipWrapMode wrap = ClipWrapMode::Once;
                if (!it.value().is_string() || !anim::wrapFromName(it.value().get<std::string>(), wrap) || wrap == ClipWrapMode::Default){
                    warnings.push_back(where + ".wrap: expected \"once\", \"loop\", \"ping_pong\" or \"clamp_forever\", ignored");
                    continue;
                }
                out.wrap = wrap;
            }else if (it.key() == "root_motion"){
                if (!it.value().is_object()){
                    warnings.push_back(where + ".root_motion: expected an object, ignored");
                    continue;
                }
                anim::ClipRootMotion rootMotion;
                rootMotion.present = true;
                for (auto member = it.value().begin(); member != it.value().end(); ++member){
                    const std::string& key = member.key();
                    const SidecarJson& field = member.value();
                    if (key == "bone"){
                        if (field.is_string()) rootMotion.bone = field.get<std::string>();
                        else warnings.push_back(where + ".root_motion.bone: expected a string, ignored");
                        continue;
                    }
                    bool* flag = key == "bake_rotation" ? &rootMotion.bakeRotation : key == "bake_position_y" ? &rootMotion.bakePositionY
                               : key == "bake_position_xz" ? &rootMotion.bakePositionXZ : nullptr;
                    if (!flag){
                        out.rootMotionUnknownKeys.push_back({key, dumpMember(field)});
                    }else if (field.is_boolean()){
                        *flag = field.get<bool>();
                    }else{
                        warnings.push_back(where + ".root_motion." + key + ": expected true or false, ignored");
                    }
                }
                out.rootMotion = rootMotion;
            }else{
                out.unknownKeys.push_back({it.key(), dumpMember(it.value())});
            }
        }
    }

    struct SidecarCache{
        std::mutex mutex;
        // model key -> its sidecar (null: none, or it did not parse)
        std::map<std::string, std::shared_ptr<const anim::ModelClipSettings>> models;
        uint64_t epoch = 0;     // bumped by every drop: a read that started before it is not kept
    };

    // never destroyed: loads may run during static destruction
    SidecarCache& sidecarCache(){
        static SidecarCache* cache = new SidecarCache();
        return *cache;
    }

}

bool anim::ModelClipSetting::operator==(const ModelClipSetting& other) const{
    return wrap == other.wrap && sameRootMotion(rootMotion, other.rootMotion) && unknownKeys == other.unknownKeys &&
           rootMotionUnknownKeys == other.rootMotionUnknownKeys;
}

const anim::ModelClipSetting* anim::ModelClipSettings::find(const std::string& clip) const{
    const auto found = clips.find(clip);
    return found == clips.end() ? nullptr : &found->second;
}

std::string anim::modelClipSettingsPath(const std::string& modelPath){
    return modelPath + ".clips";
}

bool anim::parseModelClipSettings(const std::string& text, ModelClipSettings& out, std::vector<std::string>& warnings, std::string* error){
    out = ModelClipSettings();
    const SidecarJson root = SidecarJson::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        return failSidecar(error, "not a model clips sidecar: the top level must be a JSON object");
    }
    if (root.contains("version")){
        const SidecarJson& version = root["version"];
        if (!version.is_number_integer() || version.get<int64_t>() < 1){
            return failSidecar(error, "\"version\" must be a whole number from 1");
        }
        if (version.get<int64_t>() > MODEL_CLIP_SETTINGS_VERSION){
            return failSidecar(error, "unsupported model clips sidecar version " + std::to_string(version.get<int64_t>()) +
                                      " (this engine reads up to " + std::to_string(MODEL_CLIP_SETTINGS_VERSION) + ")");
        }
    }
    for (auto it = root.begin(); it != root.end(); ++it){
        if (it.key() == "version") continue;
        if (it.key() != "clips"){
            out.unknownKeys.push_back({it.key(), dumpMember(it.value())});
            continue;
        }
        if (!it.value().is_object()){
            warnings.push_back("clips: expected an object of clip names, ignored");
            continue;
        }
        for (auto clip = it.value().begin(); clip != it.value().end(); ++clip){
            ModelClipSetting setting;
            readClipSetting(clip.key(), clip.value(), setting, warnings);
            if (!setting.empty()) out.clips[clip.key()] = std::move(setting);
        }
    }
    return true;
}

std::string anim::writeModelClipSettings(const ModelClipSettings& settings){
    SidecarJson root = SidecarJson::object();
    root["version"] = MODEL_CLIP_SETTINGS_VERSION;
    SidecarJson clips = SidecarJson::object();
    for (const auto& entry : settings.clips){
        const ModelClipSetting& setting = entry.second;
        if (setting.empty()) continue;
        SidecarJson clip = SidecarJson::object();
        if (setting.wrap) clip["wrap"] = wrapName(*setting.wrap);
        if (setting.rootMotion){
            SidecarJson rootMotion = SidecarJson::object();
            rootMotion[rootMotionMembers[0]] = setting.rootMotion->bone;
            rootMotion[rootMotionMembers[1]] = setting.rootMotion->bakeRotation;
            rootMotion[rootMotionMembers[2]] = setting.rootMotion->bakePositionY;
            rootMotion[rootMotionMembers[3]] = setting.rootMotion->bakePositionXZ;
            putUnknown(rootMotion, setting.rootMotionUnknownKeys);
            clip["root_motion"] = std::move(rootMotion);
        }
        putUnknown(clip, setting.unknownKeys);
        clips[entry.first] = std::move(clip);
    }
    root["clips"] = std::move(clips);
    putUnknown(root, settings.unknownKeys);
    return root.dump(2, ' ', false, SidecarJson::error_handler_t::replace) + "\n";
}

std::shared_ptr<const anim::ModelClipSettings> anim::modelClipSettings(const std::string& modelPath){
    const std::string key = MeshSystem::getModelFilenameKey(modelPath);
    SidecarCache& cache = sidecarCache();
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto found = cache.models.find(key);
        if (found != cache.models.end()) return found->second;
        epoch = cache.epoch;
    }
    // read outside the lock (a pack read can take a while); the first read to finish is kept
    const std::string path = modelClipSettingsPath(modelPath);
    std::shared_ptr<ModelClipSettings> settings;
    std::string text;
    std::string error;
    bool notFound = false;
    if (pose::readTextFile(path, text, &error, &notFound)){
        auto parsed = std::make_shared<ModelClipSettings>();
        std::vector<std::string> warnings;
        if (parseModelClipSettings(text, *parsed, warnings, &error)){
            settings = std::move(parsed);
            for (const std::string& warning : warnings) Log::warn("%s: %s", path.c_str(), warning.c_str());
        }else{
            Log::warn("%s: %s (its clips play with the importer's settings)", path.c_str(), error.c_str());
        }
    }else if (!notFound){
        Log::warn("%s (its clips play with the importer's settings)", error.c_str());
    }
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.epoch != epoch) return settings;
    return cache.models.emplace(key, std::move(settings)).first->second;
}

void anim::dropModelClipSettings(const std::string& modelPath){
    SidecarCache& cache = sidecarCache();
    const std::string key = modelPath.empty() ? std::string() : MeshSystem::getModelFilenameKey(modelPath);
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.epoch++;
    if (key.empty()) cache.models.clear();
    else cache.models.erase(key);
}

void anim::applyModelClipSetting(const ModelClipSetting& setting, ClipData& data){
    if (setting.wrap) data.wrap = *setting.wrap;
    if (setting.rootMotion){
        std::string block = writeClipRootMotion(*setting.rootMotion);
        auto at = std::find_if(data.unknownKeys.begin(), data.unknownKeys.end(), [](const auto& entry){ return entry.first == "root_motion"; });
        if (at != data.unknownKeys.end()) at->second = std::move(block);
        else data.unknownKeys.push_back({"root_motion", std::move(block)});
    }
}
