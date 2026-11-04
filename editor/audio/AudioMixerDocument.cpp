// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioMixerDocument.h"

#include "Project.h"
#include "audio/AudioMixer.h"
#include "audio/LoudnessMonitor.h"
#include "Generator.h"
#include "command/type/WriteFilesCmd.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    std::string joinNames(const std::vector<std::string>& names) {
        std::string out;
        for (const std::string& name : names) out += (out.empty() ? "" : ", ") + name;
        return out.empty() ? "none" : out;
    }

    std::vector<std::string> groupNames(const AudioMixerDef& def) {
        std::vector<std::string> names;
        for (const AudioMixerGroupDef* group : def.treeOrder()) names.push_back(group->name);
        return names;
    }

    std::vector<std::string> effectNames(const AudioMixerGroupDef& group) {
        std::vector<std::string> names;
        for (size_t i = 0; i < group.effects.size(); i++) {
            names.push_back(std::to_string(i) + ": " + (group.effects[i].type == AudioEffectType::Unknown ? group.effects[i].typeName
                                                                                                            : AudioEffects::typeName(group.effects[i].type)));
        }
        return names;
    }

    std::string typeNames() {
        std::vector<std::string> names;
        for (const AudioEffectInfo& info : AudioEffects::all()) names.push_back(info.name);
        return joinNames(names);
    }

    // a number argument (finite)
    bool number(const Json& value, float& out) {
        if (!value.is_number()) return false;
        const double v = value.get<double>();
        if (!std::isfinite(v)) return false;
        out = (float)v;
        return true;
    }

    AudioMixerGroupDef* group(AudioMixerDef& def, const Json& op, const char* key, std::string* error, const std::string& where) {
        if (!op.contains(key) || !op[key].is_string()) {
            fail(error, where + ": `" + key + "` (a group name) is required. Groups: " + joinNames(groupNames(def)) + ".");
            return nullptr;
        }
        const std::string name = op[key].get<std::string>();
        AudioMixerGroupDef* found = def.findGroup(name);
        if (!found) fail(error, where + ": no group '" + name + "'. Groups: " + joinNames(groupNames(def)) + ".");
        return found;
    }

    // params {name: number, sidechainGroup: name} into an effect
    bool setParams(AudioMixerDef& def, AudioMixerEffectDef& effect, const Json& params, std::string* error, const std::string& where) {
        if (!params.is_object()) return fail(error, where + ": `params` must be an object {name: value}");
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it.key() == "sidechainGroup" && effect.type == AudioEffectType::Duck) {
                if (!it.value().is_string() || !def.findGroupByName(it.value().get<std::string>())) {
                    return fail(error, where + ": params.sidechainGroup must name a group (" + joinNames(groupNames(def)) + ")");
                }
                effect.sidechainGroup = it.value().get<std::string>();
                continue;
            }
            const AudioEffectParamInfo* param = AudioEffects::findParam(effect.type, it.key());
            if (!param) {
                return fail(error, where + ": " + AudioEffects::typeName(effect.type) + " has no parameter '" + it.key() + "' (" +
                                   AudioEffects::paramNames(effect.type) + ")");
            }
            float value = 0.0f;
            if (!number(it.value(), value)) return fail(error, where + ": params." + it.key() + " must be a number");
            value = std::clamp(value, param->minValue, param->maxValue);
            if (std::string(param->name) == "wet") effect.wet = value;
            else effect.params[param->name] = value;
        }
        return true;
    }

    // the effect of `group` the op names: `index` (0-based) or `type`
    int effectOf(const AudioMixerGroupDef& group, const Json& op, std::string* error, const std::string& where) {
        Json which;
        if (op.contains("index")) which = op["index"];
        else if (op.contains("effect")) which = op["effect"];
        else if (op.contains("type")) which = op["type"];
        const int index = AudioMixerDocument::findEffect(group, which);
        if (index < 0) {
            fail(error, where + ": no such effect in '" + group.name + "' (give `index` 0-based or `type`). Effects: " +
                        joinNames(effectNames(group)) + ".");
        }
        return index;
    }

    bool validName(const std::string& name) {
        if (name.empty() || name.size() > 64) return false;
        for (char c : name) {
            if ((unsigned char)c < 0x20 || c == '/') return false;
        }
        return true;
    }

}

// ---------------------------------------------------------------- new sources

namespace {
    std::string& defaultOutputGroup() {
        static std::string group;
        return group;
    }
}

void AudioMixerDocument::setDefaultOutputGroup(const std::string& group) {
    defaultOutputGroup() = group;
}

AudioSourceComponent AudioMixerDocument::newAudioSource() {
    AudioSourceComponent audio;
    if (!defaultOutputGroup().empty()) {
        audio.outputGroupName = defaultOutputGroup();
        audio.outputGroup = AudioMixer::findGroupId(defaultOutputGroup());
    }
    return audio;
}

// ---------------------------------------------------------------- files

bool AudioMixerDocument::read(const fs::path& file, AudioMixerDef& def, std::string* error, std::vector<std::string>* warnings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return fail(error, "cannot open " + FileUtils::pathToGenericUtf8(file));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return AudioMixerIO::parse(text, def, error, warnings);
}

bool AudioMixerDocument::ensureProjectMixer(Project* project, fs::path& file, std::string* error, bool* created) {
    if (created) *created = false;
    if (!project) return fail(error, "no project is open");
    AudioSettings settings = project->getAudioSettings();
    if (settings.mixer.empty()) {
        settings.mixer = Project::defaultAudioMixerPath;
        project->setAudioSettings(settings);
        project->markGeneratedSourcesDirty();
        if (!project->getProjectPath().empty()) project->saveProjectFile();
    }
    file = project->getAudioMixerFile();
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) return true;
    if (!commit(project, file, AudioMixerIO::createDefault(), "", error)) return false;
    if (created) *created = true;
    return true;
}

AudioMixerDef AudioMixerDocument::newProjectMixer() {
    AudioMixerDef def = AudioMixerIO::createDefault();
    AudioMixerEffectDef limiter;
    limiter.type = AudioEffectType::Limiter;
    limiter.typeName = AudioEffects::typeName(limiter.type);
    limiter.id = AudioMixerIO::newId(def);
    limiter.params = {{"threshold", -1.0f}, {"release", 0.1f}, {"lookahead", 1.5f}, {"truePeak", 1.0f}};
    def.groups.front().effects.push_back(limiter);
    return def;
}

AudioSettings AudioMixerDocument::newProjectAudioSettings() {
    AudioSettings settings;
    settings.mixer = Project::defaultAudioMixerPath;
    settings.outputStage = "linear";
    settings.iosSessionCategory = "ambient";
    return settings;
}

bool AudioMixerDocument::writeNewProjectMixer(const fs::path& assetsDir, std::string* error) {
    const fs::path file = assetsDir / FileUtils::pathFromUtf8(Project::defaultAudioMixerPath);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const io::WriteResult result = io::writeFileAtomic(file, AudioMixerIO::write(newProjectMixer()));
    if (!result.ok()) return fail(error, result.message);
    return true;
}

bool AudioMixerDocument::writeNewProjectAudio(const fs::path& projectDir, std::string* error) {
    const fs::path yamlFile = projectDir / "project.yaml";
    YAML::Node root;
    try {
        root = YAML::LoadFile(yamlFile.string());
    } catch (const std::exception& e) {
        return fail(error, "cannot read " + yamlFile.string() + ": " + e.what());
    }
    if (root["audio"]) return true;
    const std::string assets = root["assetsDir"] ? root["assetsDir"].as<std::string>() : std::string(".");
    if (!writeNewProjectMixer(projectDir / FileUtils::pathFromUtf8(assets), error)) return false;
    const AudioSettings settings = newProjectAudioSettings();
    YAML::Node audio;
    audio["mixer"] = settings.mixer;
    audio["outputStage"] = settings.outputStage;
    audio["iosSessionCategory"] = settings.iosSessionCategory;
    root["audio"] = audio;
    YAML::Emitter out;
    out << root;
    const io::WriteResult result = io::writeFileAtomic(yamlFile, std::string(out.c_str()) + "\n");
    if (!result.ok()) return fail(error, result.message);
    return true;
}

bool AudioMixerDocument::commit(Project* project, const fs::path& file, const AudioMixerDef& def, const std::string& mergeKey, std::string* error) {
    if (!project) return fail(error, "no project is open");
    const std::string invalid = AudioMixerIO::validate(def);
    if (!invalid.empty()) return fail(error, invalid);
    const std::string text = AudioMixerIO::write(def);

    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        const std::string current((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (current == text) return true;
    }

    auto* command = new WriteFilesCmd({{file, text}}, [project](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(project, changed);
    });
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey("audio-mixer\n" + FileUtils::pathToGenericUtf8(file) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        return fail(error, "writing " + FileUtils::pathToGenericUtf8(file) + " failed" + (failure->empty() ? "" : ": " + *failure));
    }
    return true;
}

// ---------------------------------------------------------------- helpers

int AudioMixerDocument::findEffect(const AudioMixerGroupDef& group, const Json& which) {
    if (which.is_number_integer()) {
        const int index = which.get<int>();
        return index >= 0 && index < (int)group.effects.size() ? index : -1;
    }
    if (which.is_string()) {
        const std::string text = which.get<std::string>();
        const AudioEffectType type = AudioEffects::typeFromName(text);
        for (size_t i = 0; i < group.effects.size(); i++) {
            if (group.effects[i].id == text) return (int)i;
            if (type != AudioEffectType::Unknown && group.effects[i].type == type) return (int)i;
        }
    }
    return -1;
}

std::vector<std::string> AudioMixerDocument::subtree(const AudioMixerDef& def, const std::string& groupId) {
    std::vector<std::string> out;
    for (const AudioMixerGroupDef& group : def.groups) {
        if (def.isUnder(group.id, groupId)) out.push_back(group.id);
    }
    return out;
}

void AudioMixerDocument::pruneTargets(AudioMixerDef& def) {
    AudioMixerTarget target;
    def.exposed.erase(std::remove_if(def.exposed.begin(), def.exposed.end(), [&](const AudioMixerExposedDef& exposed) {
        return !AudioMixerIO::resolveTarget(def, exposed.target, target);
    }), def.exposed.end());
    for (AudioMixerSnapshotDef& snapshot : def.snapshots) {
        for (auto it = snapshot.values.begin(); it != snapshot.values.end();) {
            if (!AudioMixerIO::resolveTarget(def, it->first, target)) it = snapshot.values.erase(it);
            else ++it;
        }
    }
    // a duck listening to a group that went listens to nothing
    for (AudioMixerGroupDef& group : def.groups) {
        for (AudioMixerEffectDef& effect : group.effects) {
            if (!effect.sidechainGroup.empty() && !def.findGroupByName(effect.sidechainGroup)) effect.sidechainGroup.clear();
        }
    }
}

void AudioMixerDocument::removeGroup(AudioMixerDef& def, const std::string& groupId) {
    const std::vector<std::string> ids = subtree(def, groupId);
    def.groups.erase(std::remove_if(def.groups.begin(), def.groups.end(), [&](const AudioMixerGroupDef& group) {
        return std::find(ids.begin(), ids.end(), group.id) != ids.end();
    }), def.groups.end());
    pruneTargets(def);
}

void AudioMixerDocument::removeEffect(AudioMixerDef& def, const std::string& groupId, size_t index) {
    AudioMixerGroupDef* group = def.findGroup(groupId);
    if (!group || index >= group->effects.size()) return;
    group->effects.erase(group->effects.begin() + (long)index);
    pruneTargets(def);
}

std::string AudioMixerDocument::uniqueGroupName(const AudioMixerDef& def, const std::string& base) {
    if (!def.findGroupByName(base)) return base;
    for (int n = 2;; n++) {
        const std::string name = base + " " + std::to_string(n);
        if (!def.findGroupByName(name)) return name;
    }
}

std::map<std::string, float> AudioMixerDocument::captureValues(const AudioMixerDef& def, bool runtime) {
    std::map<std::string, float> values;
    for (const std::string& target : AudioMixerIO::allTargets(def)) {
        float value = 0.0f;
        if (runtime && AudioMixer::getTarget(target, value)) {
            values[target] = value;
        } else if (AudioMixerIO::authoredValue(def, target, value)) {
            values[target] = value;
        }
    }
    return values;
}

std::string AudioMixerDocument::resolveKey(const AudioMixerDef& def, const std::string& key) {
    AudioMixerTarget target;
    if (const AudioMixerExposedDef* exposed = def.findExposed(key)) return exposed->target;
    if (AudioMixerIO::resolveTarget(def, key, target)) return key;
    // "<group>/volume", "<group>/<effect>/<param>" (effect: type or 0-based index)
    const size_t first = key.find('/');
    if (first == std::string::npos) return "";
    const AudioMixerGroupDef* group = def.findGroup(key.substr(0, first));
    if (!group) return "";
    const std::string rest = key.substr(first + 1);
    if (rest == "volume") return AudioMixerIO::volumeTarget(group->id);
    const size_t second = rest.find('/');
    if (second == std::string::npos) return "";
    const std::string effectKey = rest.substr(0, second);
    const std::string param = rest.substr(second + 1);
    Json which = effectKey;
    if (!effectKey.empty() && std::all_of(effectKey.begin(), effectKey.end(), ::isdigit)) which = std::stoi(effectKey);
    const int index = findEffect(*group, which);
    if (index < 0) return "";
    const AudioMixerEffectDef& effect = group->effects[index];
    const AudioEffectParamInfo* info = AudioEffects::findParam(effect.type, param);
    if (!info) return "";
    const std::string resolved = AudioMixerIO::paramTarget(effect.id, info->name);
    return AudioMixerIO::resolveTarget(def, resolved, target) ? resolved : "";
}

// ---------------------------------------------------------------- ops

bool AudioMixerDocument::applyOps(AudioMixerDef& def, const Json& ops, std::string* error, std::vector<std::string>* notes) {
    if (!ops.is_array() || ops.empty()) return fail(error, "`ops` must be a non-empty array of {op: ...}");
    AudioMixerDef work = def;
    for (size_t i = 0; i < ops.size(); i++) {
        std::string opError;
        if (!applyOp(work, ops[i], &opError, notes)) {
            return fail(error, "ops[" + std::to_string(i) + "] " + (ops[i].is_object() ? ops[i].value("op", std::string("?")) : std::string("?")) +
                               ": " + opError + (i > 0 ? " (nothing was changed)" : ""));
        }
    }
    const std::string invalid = AudioMixerIO::validate(work);
    if (!invalid.empty()) return fail(error, invalid + " (nothing was changed)");
    def = std::move(work);
    return true;
}

bool AudioMixerDocument::applyOp(AudioMixerDef& def, const Json& op, std::string* error, std::vector<std::string>* notes) {
    if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) return fail(error, "each op is an object with `op`");
    const std::string kind = op["op"].get<std::string>();
    auto note = [notes](const std::string& text) {
        if (notes) notes->push_back(text);
    };

    if (kind == "add_group") {
        if (!op.contains("name") || !op["name"].is_string() || !validName(op["name"].get<std::string>())) {
            return fail(error, "`name` (1-64 characters, no '/') is required");
        }
        const std::string name = op["name"].get<std::string>();
        if (def.findGroupByName(name)) return fail(error, "a group named '" + name + "' exists (names are unique)");
        const AudioMixerGroupDef* parent = def.master();
        if (op.contains("parent")) {
            if (!op["parent"].is_string() || !(parent = def.findGroup(op["parent"].get<std::string>()))) {
                return fail(error, "no parent group '" + op.value("parent", std::string()) + "'. Groups: " + joinNames(groupNames(def)) + ".");
            }
        }
        AudioMixerGroupDef group;
        group.id = AudioMixerIO::newId(def);
        group.name = name;
        group.parentId = parent->id;
        float volume = 0.0f;
        if (op.contains("volume")) {
            if (!number(op["volume"], volume)) return fail(error, "`volume` must be a number (dB)");
            group.volumeDb = std::clamp(volume, AudioMath::minDb, AudioMath::maxDb);
        }
        def.groups.push_back(group);
        return true;
    }

    if (kind == "remove_group") {
        AudioMixerGroupDef* found = group(def, op, "name", error, "remove_group");
        if (!found) return false;
        if (found->parentId.empty()) return fail(error, "Master cannot be removed");
        const std::vector<std::string> ids = subtree(def, found->id);
        if (ids.size() > 1) note("removed '" + found->name + "' and its " + std::to_string(ids.size() - 1) + " subgroup(s)");
        removeGroup(def, found->id);
        return true;
    }

    if (kind == "set_group") {
        AudioMixerGroupDef* found = group(def, op, "name", error, "set_group");
        if (!found) return false;
        if (op.contains("volume")) {
            float volume = 0.0f;
            if (!number(op["volume"], volume)) return fail(error, "`volume` must be a number (dB, -80..20)");
            found->volumeDb = std::clamp(volume, AudioMath::minDb, AudioMath::maxDb);
        }
        for (const char* key : {"mute", "solo", "bypass", "bypassEffects"}) {
            if (!op.contains(key)) continue;
            if (!op[key].is_boolean()) return fail(error, std::string("`") + key + "` must be true or false");
            const bool value = op[key].get<bool>();
            if (std::string(key) == "mute") found->mute = value;
            else if (std::string(key) == "solo") found->solo = value;
            else found->bypassEffects = value;
        }
        if (op.contains("parent")) {
            if (found->parentId.empty()) return fail(error, "Master has no parent");
            const AudioMixerGroupDef* parent = op["parent"].is_string() ? def.findGroup(op["parent"].get<std::string>()) : nullptr;
            if (!parent) return fail(error, "no parent group '" + op.value("parent", std::string()) + "'");
            if (def.isUnder(parent->id, found->id)) return fail(error, "'" + parent->name + "' is inside '" + found->name + "'");
            found->parentId = parent->id;
        }
        if (op.contains("new_name")) {
            if (!op["new_name"].is_string() || !validName(op["new_name"].get<std::string>())) return fail(error, "`new_name` must be 1-64 characters, no '/'");
            const std::string newName = op["new_name"].get<std::string>();
            if (newName != found->name && def.findGroupByName(newName)) return fail(error, "a group named '" + newName + "' exists");
            // ducks that listen to it follow
            for (AudioMixerGroupDef& other : def.groups) {
                for (AudioMixerEffectDef& effect : other.effects) {
                    if (effect.sidechainGroup == found->name) effect.sidechainGroup = newName;
                }
            }
            found->name = newName;
        }
        return true;
    }

    if (kind == "add_effect") {
        AudioMixerGroupDef* found = group(def, op, "group", error, "add_effect");
        if (!found) return false;
        const std::string typeName = op.value("type", std::string());
        const AudioEffectInfo* info = AudioEffects::find(typeName);
        if (!info) return fail(error, "`type` must be one of: " + typeNames());
        if (found->effects.size() >= (size_t)AudioEffects::maxEffectsPerGroup) {
            return fail(error, "'" + found->name + "' has " + std::to_string(AudioEffects::maxEffectsPerGroup) + " effects, the most a group holds");
        }
        AudioMixerEffectDef effect;
        effect.id = AudioMixerIO::newId(def);
        effect.type = info->type;
        effect.typeName = info->name;
        if (op.contains("params") && !setParams(def, effect, op["params"], error, "add_effect")) return false;
        if (op.contains("wet")) {
            float wet = 1.0f;
            if (!number(op["wet"], wet)) return fail(error, "`wet` must be a number 0..1");
            effect.wet = std::clamp(wet, 0.0f, 1.0f);
        }
        if (op.contains("bypass")) {
            if (!op["bypass"].is_boolean()) return fail(error, "`bypass` must be true or false");
            effect.bypass = op["bypass"].get<bool>();
        }
        size_t index = found->effects.size();
        if (op.contains("index")) {
            if (!op["index"].is_number_integer()) return fail(error, "`index` must be an integer (0-based)");
            index = (size_t)std::clamp(op["index"].get<int>(), 0, (int)found->effects.size());
        }
        found->effects.insert(found->effects.begin() + (long)index, effect);
        if (effect.type == AudioEffectType::Duck && effect.sidechainGroup.empty()) {
            note("the duck in '" + found->name + "' listens to no group yet: set params.sidechainGroup");
        }
        return true;
    }

    if (kind == "set_effect" || kind == "remove_effect") {
        AudioMixerGroupDef* found = group(def, op, "group", error, kind);
        if (!found) return false;
        const int index = effectOf(*found, op, error, kind);
        if (index < 0) return false;
        if (kind == "remove_effect") {
            removeEffect(def, found->id, (size_t)index);
            return true;
        }
        AudioMixerEffectDef& effect = found->effects[index];
        if (effect.type == AudioEffectType::Unknown) return fail(error, "the effect type '" + effect.typeName + "' is unknown here (kept, not edited)");
        if (op.contains("params") && !setParams(def, effect, op["params"], error, kind)) return false;
        if (op.contains("wet")) {
            float wet = 1.0f;
            if (!number(op["wet"], wet)) return fail(error, "`wet` must be a number 0..1");
            effect.wet = std::clamp(wet, 0.0f, 1.0f);
        }
        if (op.contains("bypass")) {
            if (!op["bypass"].is_boolean()) return fail(error, "`bypass` must be true or false");
            effect.bypass = op["bypass"].get<bool>();
        }
        if (op.contains("move_to")) {
            if (!op["move_to"].is_number_integer()) return fail(error, "`move_to` must be an index (0-based)");
            const int to = std::clamp(op["move_to"].get<int>(), 0, (int)found->effects.size() - 1);
            AudioMixerEffectDef moved = effect;
            found->effects.erase(found->effects.begin() + index);
            found->effects.insert(found->effects.begin() + to, moved);
        }
        return true;
    }

    if (kind == "expose") {
        if (!op.contains("name") || !op["name"].is_string() || op["name"].get<std::string>().empty()) {
            return fail(error, "`name` (what AudioMixer.setFloat takes) is required");
        }
        const std::string name = op["name"].get<std::string>();
        AudioMixerGroupDef* found = group(def, op, "group", error, "expose");
        if (!found) return false;
        std::string target;
        const bool volume = op.contains("volume") ? op["volume"].is_boolean() && op["volume"].get<bool>() : !op.contains("param");
        if (volume) {
            target = AudioMixerIO::volumeTarget(found->id);
        } else {
            if (!op["param"].is_string()) return fail(error, "`param` (an effect parameter name) or `volume: true` is required");
            const std::string param = op["param"].get<std::string>();
            int index = -1;
            if (op.contains("effect") || op.contains("index") || op.contains("type")) {
                index = effectOf(*found, op, error, "expose");
                if (index < 0) return false;
            } else {
                // the one effect of the group with that parameter
                for (size_t i = 0; i < found->effects.size(); i++) {
                    if (AudioEffects::findParam(found->effects[i].type, param) && param != "wet") {
                        if (index >= 0) return fail(error, "two effects of '" + found->name + "' have '" + param + "': give `effect` (index or type)");
                        index = (int)i;
                    }
                }
                if (index < 0) return fail(error, "no effect of '" + found->name + "' has '" + param + "'. Effects: " + joinNames(effectNames(*found)) + ".");
            }
            const AudioMixerEffectDef& effect = found->effects[index];
            const AudioEffectParamInfo* info = AudioEffects::findParam(effect.type, param);
            if (!info) return fail(error, AudioEffects::typeName(effect.type) + " has no parameter '" + param + "' (" + AudioEffects::paramNames(effect.type) + ")");
            target = AudioMixerIO::paramTarget(effect.id, info->name);
        }
        const bool remember = op.value("remember", false);
        for (AudioMixerExposedDef& exposed : def.exposed) {
            if (exposed.name == name) {
                // exposing a name again moves it
                exposed.target = target;
                if (op.contains("remember")) exposed.remember = remember;
                return true;
            }
            if (exposed.target == target) {
                note("'" + exposed.name + "' exposes the same parameter");
            }
        }
        def.exposed.push_back({name, target, remember});
        return true;
    }

    if (kind == "unexpose") {
        const std::string name = op.value("name", std::string());
        auto it = std::find_if(def.exposed.begin(), def.exposed.end(), [&](const AudioMixerExposedDef& e) { return e.name == name; });
        if (it == def.exposed.end()) {
            std::vector<std::string> names;
            for (const AudioMixerExposedDef& exposed : def.exposed) names.push_back(exposed.name);
            return fail(error, "no exposed parameter '" + name + "'. Exposed: " + joinNames(names) + ".");
        }
        def.exposed.erase(it);
        return true;
    }

    if (kind == "set_exposed") {
        const std::string name = op.value("name", std::string());
        auto it = std::find_if(def.exposed.begin(), def.exposed.end(), [&](const AudioMixerExposedDef& e) { return e.name == name; });
        if (it == def.exposed.end()) return fail(error, "no exposed parameter '" + name + "'");
        if (op.contains("remember")) {
            if (!op["remember"].is_boolean()) return fail(error, "`remember` must be true or false");
            it->remember = op["remember"].get<bool>();
        }
        if (op.contains("new_name")) {
            const std::string newName = op["new_name"].is_string() ? op["new_name"].get<std::string>() : "";
            if (newName.empty()) return fail(error, "`new_name` must be a non-empty string");
            if (newName != name && def.findExposed(newName)) return fail(error, "'" + newName + "' is exposed already");
            it->name = newName;
        }
        return true;
    }

    auto snapshotIndex = [&def](const std::string& name) {
        for (size_t i = 0; i < def.snapshots.size(); i++) {
            if (def.snapshots[i].name == name) return (int)i;
        }
        return -1;
    };
    auto snapshotNames = [&def]() {
        std::vector<std::string> names;
        for (const AudioMixerSnapshotDef& snapshot : def.snapshots) names.push_back(snapshot.name);
        return joinNames(names);
    };
    // values {key: number}: exposed names, targets, "<group>/volume", "<group>/<effect>/<param>"
    auto readValues = [&def, error](const Json& values, std::map<std::string, float>& out) {
        if (!values.is_object()) return fail(error, "`values` must be an object {parameter: value}");
        for (auto it = values.begin(); it != values.end(); ++it) {
            const std::string target = resolveKey(def, it.key());
            if (target.empty()) {
                return fail(error, "'" + it.key() + "' is not a parameter: an exposed name, \"<group>/volume\" or \"<group>/<effect type or index>/<param>\"");
            }
            float value = 0.0f;
            if (!number(it.value(), value)) return fail(error, "values." + it.key() + " must be a number");
            out[target] = AudioMixerIO::clampValue(def, target, value);
        }
        return true;
    };

    if (kind == "add_snapshot") {
        const std::string name = op.value("name", std::string());
        if (name.empty()) return fail(error, "`name` is required");
        if (snapshotIndex(name) >= 0) return fail(error, "a snapshot named '" + name + "' exists");
        AudioMixerSnapshotDef snapshot;
        snapshot.name = name;
        // every parameter as the mixer has it now (the asset's values; in Play the ones heard)
        const bool runtime = AudioMixer::hasAsset() && AudioMixer::getDef().master() && def.master() &&
                             AudioMixer::getDef().master()->id == def.master()->id;
        snapshot.values = captureValues(def, runtime && op.value("live", false));
        if (op.contains("values") && !readValues(op["values"], snapshot.values)) return false;
        def.snapshots.push_back(snapshot);
        if (def.startSnapshot.empty() && op.value("start", def.snapshots.size() == 1)) {
            def.startSnapshot = name;
            note("'" + name + "' is the start snapshot");
        }
        return true;
    }

    if (kind == "set_snapshot") {
        const int index = snapshotIndex(op.value("name", std::string()));
        if (index < 0) return fail(error, "no snapshot '" + op.value("name", std::string()) + "'. Snapshots: " + snapshotNames() + ".");
        AudioMixerSnapshotDef& snapshot = def.snapshots[index];
        if (op.value("capture", false)) snapshot.values = captureValues(def, false);
        if (op.contains("values") && !readValues(op["values"], snapshot.values)) return false;
        if (op.contains("new_name")) {
            const std::string newName = op["new_name"].is_string() ? op["new_name"].get<std::string>() : "";
            if (newName.empty()) return fail(error, "`new_name` must be a non-empty string");
            if (newName != snapshot.name && snapshotIndex(newName) >= 0) return fail(error, "a snapshot named '" + newName + "' exists");
            if (def.startSnapshot == snapshot.name) def.startSnapshot = newName;
            snapshot.name = newName;
        }
        return true;
    }

    if (kind == "remove_snapshot") {
        const int index = snapshotIndex(op.value("name", std::string()));
        if (index < 0) return fail(error, "no snapshot '" + op.value("name", std::string()) + "'. Snapshots: " + snapshotNames() + ".");
        if (def.startSnapshot == def.snapshots[index].name) def.startSnapshot.clear();
        def.snapshots.erase(def.snapshots.begin() + index);
        return true;
    }

    if (kind == "set_start_snapshot") {
        const std::string name = op.contains("name") && op["name"].is_string() ? op["name"].get<std::string>() : "";
        if (!name.empty() && snapshotIndex(name) < 0) return fail(error, "no snapshot '" + name + "'. Snapshots: " + snapshotNames() + ".");
        def.startSnapshot = name;
        return true;
    }

    if (kind == "set_mixer") {
        if (op.contains("updateMode") || op.contains("update_mode")) {
            const Json& mode = op.contains("updateMode") ? op["updateMode"] : op["update_mode"];
            if (mode == "scaled") def.unscaled = false;
            else if (mode == "unscaled") def.unscaled = true;
            else return fail(error, "update_mode must be \"scaled\" or \"unscaled\"");
        }
        return true;
    }

    return fail(error, "unknown op '" + kind + "' (add_group, remove_group, set_group, add_effect, set_effect, remove_effect, "
                       "expose, unexpose, set_exposed, add_snapshot, set_snapshot, remove_snapshot, set_start_snapshot, set_mixer)");
}

// ---------------------------------------------------------------- describe

Json AudioMixerDocument::describe(const AudioMixerDef& def, bool live, float targetLufs) {
    Json out;
    out["update_mode"] = def.unscaled ? "unscaled" : "scaled";
    Json groups = Json::array();
    for (const AudioMixerGroupDef* group : def.treeOrder()) {
        Json node;
        node["name"] = group->name;
        node["id"] = group->id;
        const AudioMixerGroupDef* parent = def.findGroupById(group->parentId);
        node["parent"] = parent ? Json(parent->name) : Json(nullptr);
        node["volume"] = group->volumeDb;
        if (group->mute) node["mute"] = true;
        if (group->solo) node["solo"] = true;
        if (group->bypassEffects) node["bypass"] = true;
        Json effects = Json::array();
        for (size_t i = 0; i < group->effects.size(); i++) {
            const AudioMixerEffectDef& effect = group->effects[i];
            Json e;
            e["index"] = i;
            e["type"] = effect.type == AudioEffectType::Unknown ? effect.typeName : AudioEffects::typeName(effect.type);
            e["id"] = effect.id;
            if (effect.type == AudioEffectType::Unknown) e["unknown"] = true;
            if (effect.bypass) e["bypass"] = true;
            e["wet"] = effect.wet;
            Json params = Json::object();
            if (const AudioEffectInfo* info = AudioEffects::find(effect.type)) {
                for (const AudioEffectParamInfo& param : info->params) params[param.name] = AudioMixerIO::effectParam(effect, param);
            }
            if (effect.type == AudioEffectType::Duck) params["sidechainGroup"] = effect.sidechainGroup;
            e["params"] = params;
            effects.push_back(e);
        }
        node["effects"] = effects;
        // the frames its limiters' lookahead delays this group, at the rate that plays
        float latencyMs = 0.0f;
        if (!group->bypassEffects) {
            for (const AudioMixerEffectDef& effect : group->effects) {
                if (effect.type != AudioEffectType::Limiter || effect.bypass) continue;
                auto lookahead = effect.params.find("lookahead");
                if (lookahead != effect.params.end()) latencyMs += lookahead->second;
            }
        }
        node["latency_ms"] = latencyMs;
        if (live) {
            float now = 0.0f;
            if (AudioMixer::getTarget(AudioMixerIO::volumeTarget(group->id), now)) node["volume_now"] = now;
            if (AudioMixer::getGroupMute(group->id) != group->mute) node["mute_now"] = AudioMixer::getGroupMute(group->id);
            node["meter_db"] = std::round(AudioMixer::getMeterDb(group->id) * 10.0f) / 10.0f;
        }
        groups.push_back(node);
    }
    out["groups"] = groups;
    if (live) {
        // from the first read until Stop: a first read's values are null, the next ones measure
        LoudnessMonitor& monitor = LoudnessMonitor::get();
        monitor.requestByAgent();
        const LoudnessMonitor::Reading reading = monitor.reading();
        const auto lufs = [](const std::optional<double>& value) {
            return value ? Json(std::round(*value * 100.0) / 100.0) : Json(nullptr);
        };
        out["loudness"] = {{"momentary_lufs", lufs(reading.momentary)},
                           {"short_term_lufs", lufs(reading.shortTerm)},
                           {"integrated_lufs", lufs(reading.integrated)},
                           {"true_peak_dbtp", std::round(reading.truePeakDb * 100.0) / 100.0},
                           {"gain_reduction_db", std::round(reading.gainReductionDb * 100.0f) / 100.0f},
                           {"target_lufs", targetLufs}};
    }

    Json exposed = Json::array();
    for (const AudioMixerExposedDef& entry : def.exposed) {
        Json node;
        node["name"] = entry.name;
        node["target"] = AudioMixerIO::describeTarget(def, entry.target);
        if (entry.remember) node["remember"] = true;
        float value = 0.0f;
        if (AudioMixerIO::authoredValue(def, entry.target, value)) node["value"] = value;
        if (live) {
            float now = 0.0f;
            if (AudioMixer::getTarget(entry.target, now)) node["value_now"] = now;
            if (AudioMixer::isTargetOverridden(entry.target)) node["set_by_script"] = true;
        }
        exposed.push_back(node);
    }
    out["exposed"] = exposed;

    Json snapshots = Json::array();
    for (const AudioMixerSnapshotDef& snapshot : def.snapshots) {
        Json node;
        node["name"] = snapshot.name;
        Json values = Json::object();
        for (const auto& [target, value] : snapshot.values) values[AudioMixerIO::describeTarget(def, target)] = value;
        node["values"] = values;
        snapshots.push_back(node);
    }
    out["snapshots"] = snapshots;
    out["start_snapshot"] = def.startSnapshot.empty() ? Json(nullptr) : Json(def.startSnapshot);
    if (live) {
        const std::string current = AudioMixer::getCurrentSnapshot();
        out["current_snapshot"] = current.empty() ? Json(nullptr) : Json(current);
        out["transitioning"] = AudioMixer::isTransitioning();
    }
    return out;
}
