// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioMixerIO.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

using namespace jasperengine;

using Json = nlohmann::json;
using OJson = nlohmann::ordered_json;

// ---------------------------------------------------------------- AudioMixerDef lookups

const AudioMixerGroupDef* AudioMixerDef::findGroupById(const std::string& id) const {
    if (id.empty()) return nullptr;
    for (const AudioMixerGroupDef& group : groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

const AudioMixerGroupDef* AudioMixerDef::findGroupByName(const std::string& name) const {
    if (name.empty()) return nullptr;
    for (const AudioMixerGroupDef& group : groups) {
        if (group.name == name) return &group;
    }
    return nullptr;
}

const AudioMixerGroupDef* AudioMixerDef::findGroup(const std::string& idOrName) const {
    if (const AudioMixerGroupDef* group = findGroupByName(idOrName)) return group;
    return findGroupById(idOrName);
}

AudioMixerGroupDef* AudioMixerDef::findGroup(const std::string& idOrName) {
    return const_cast<AudioMixerGroupDef*>(static_cast<const AudioMixerDef*>(this)->findGroup(idOrName));
}

const AudioMixerGroupDef* AudioMixerDef::master() const {
    for (const AudioMixerGroupDef& group : groups) {
        if (group.parentId.empty()) return &group;
    }
    return nullptr;
}

const AudioMixerGroupDef* AudioMixerDef::findEffect(const std::string& effectId, size_t* index) const {
    if (effectId.empty()) return nullptr;
    for (const AudioMixerGroupDef& group : groups) {
        for (size_t i = 0; i < group.effects.size(); i++) {
            if (group.effects[i].id == effectId) {
                if (index) *index = i;
                return &group;
            }
        }
    }
    return nullptr;
}

const AudioMixerExposedDef* AudioMixerDef::findExposed(const std::string& name) const {
    for (const AudioMixerExposedDef& exposedDef : exposed) {
        if (exposedDef.name == name) return &exposedDef;
    }
    return nullptr;
}

const AudioMixerSnapshotDef* AudioMixerDef::findSnapshot(const std::string& name) const {
    for (const AudioMixerSnapshotDef& snapshot : snapshots) {
        if (snapshot.name == name) return &snapshot;
    }
    return nullptr;
}

std::vector<const AudioMixerGroupDef*> AudioMixerDef::children(const std::string& groupId) const {
    std::vector<const AudioMixerGroupDef*> out;
    for (const AudioMixerGroupDef& group : groups) {
        if (!group.parentId.empty() && group.parentId == groupId) out.push_back(&group);
    }
    return out;
}

bool AudioMixerDef::isUnder(const std::string& groupId, const std::string& ancestorId) const {
    const AudioMixerGroupDef* group = findGroupById(groupId);
    // a cycle cannot hang: at most one step per group
    for (size_t steps = 0; group && steps <= groups.size(); steps++) {
        if (group->id == ancestorId) return true;
        group = findGroupById(group->parentId);
    }
    return false;
}

std::vector<const AudioMixerGroupDef*> AudioMixerDef::treeOrder() const {
    std::vector<const AudioMixerGroupDef*> out;
    const AudioMixerGroupDef* root = master();
    if (!root) return out;
    out.push_back(root);
    for (size_t i = 0; i < out.size(); i++) {
        for (const AudioMixerGroupDef* child : children(out[i]->id)) {
            if (std::find(out.begin(), out.end(), child) == out.end()) out.push_back(child);
        }
    }
    return out;
}

// ---------------------------------------------------------------- math

float AudioMath::dbToLinear(float db) {
    if (!(db > minDb)) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

float AudioMath::linearToDb(float linear) {
    if (!(linear > 0.0f)) return minDb;
    return std::max(minDb, 20.0f * std::log10(linear));
}

float AudioMath::interpolate(AudioParamCurve curve, float a, float b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (curve == AudioParamCurve::Frequency && a > 0.0f && b > 0.0f) {
        const float la = std::log2(a);
        const float lb = std::log2(b);
        return std::exp2(la + (lb - la) * t);
    }
    return a + (b - a) * t;
}

float AudioMath::blend(AudioParamCurve curve, const std::vector<float>& values, const std::vector<float>& weights) {
    if (values.empty()) return 0.0f;
    double sum = 0.0;
    double total = 0.0;
    for (size_t i = 0; i < values.size(); i++) {
        const double weight = i < weights.size() ? std::max(0.0f, weights[i]) : 0.0;
        if (weight <= 0.0) continue;
        const bool logScale = curve == AudioParamCurve::Frequency && values[i] > 0.0f;
        sum += weight * (logScale ? std::log2(values[i]) : values[i]);
        total += weight;
    }
    if (total <= 0.0) return values[0];
    const double mean = sum / total;
    if (curve == AudioParamCurve::Frequency) {
        // all positive (frequencies always are): back from log2
        bool allPositive = true;
        for (float value : values) allPositive = allPositive && value > 0.0f;
        if (allPositive) return (float)std::exp2(mean);
    }
    return (float)mean;
}

// ---------------------------------------------------------------- targets

std::string AudioMixerIO::volumeTarget(const std::string& groupId) {
    return groupId + "/volume";
}

std::string AudioMixerIO::paramTarget(const std::string& effectId, const std::string& param) {
    return effectId + "/" + param;
}

bool AudioMixerIO::resolveTarget(const AudioMixerDef& def, const std::string& target, AudioMixerTarget& out) {
    out = AudioMixerTarget();
    const size_t slash = target.find('/');
    if (slash == std::string::npos) return false;
    const std::string owner = target.substr(0, slash);
    const std::string param = target.substr(slash + 1);
    if (param == "volume") {
        if (const AudioMixerGroupDef* group = def.findGroupById(owner)) {
            out.group = group;
            out.volume = true;
            return true;
        }
    }
    size_t index = 0;
    const AudioMixerGroupDef* group = def.findEffect(owner, &index);
    if (!group) return false;
    const AudioMixerEffectDef& effect = group->effects[index];
    const AudioEffectParamInfo* info = AudioEffects::findParam(effect.type, param);
    if (!info || effect.type == AudioEffectType::Unknown) return false;
    out.group = group;
    out.effect = &effect;
    out.effectIndex = index;
    out.param = info;
    return true;
}

float AudioMixerIO::effectParam(const AudioMixerEffectDef& effect, const AudioEffectParamInfo& param) {
    if (std::string(param.name) == "wet") return effect.wet;
    auto it = effect.params.find(param.name);
    return it != effect.params.end() ? it->second : param.defaultValue;
}

bool AudioMixerIO::authoredValue(const AudioMixerDef& def, const std::string& target, float& value) {
    AudioMixerTarget resolved;
    if (!resolveTarget(def, target, resolved)) return false;
    value = resolved.volume ? resolved.group->volumeDb : effectParam(*resolved.effect, *resolved.param);
    return true;
}

float AudioMixerIO::clampValue(const AudioMixerDef& def, const std::string& target, float value) {
    if (!std::isfinite(value)) value = 0.0f;
    AudioMixerTarget resolved;
    if (!resolveTarget(def, target, resolved)) return value;
    if (resolved.volume) return std::clamp(value, AudioMath::minDb, AudioMath::maxDb);
    return std::clamp(value, resolved.param->minValue, resolved.param->maxValue);
}

bool AudioMixerIO::setAuthoredValue(AudioMixerDef& def, const std::string& target, float value) {
    AudioMixerTarget resolved;
    if (!resolveTarget(def, target, resolved)) return false;
    value = clampValue(def, target, value);
    AudioMixerGroupDef* group = def.findGroup(resolved.group->id);
    if (resolved.volume) {
        group->volumeDb = value;
        return true;
    }
    AudioMixerEffectDef& effect = group->effects[resolved.effectIndex];
    if (std::string(resolved.param->name) == "wet") {
        effect.wet = value;
    } else {
        effect.params[resolved.param->name] = value;
    }
    return true;
}

AudioParamCurve AudioMixerIO::curveOf(const AudioMixerDef& def, const std::string& target) {
    AudioMixerTarget resolved;
    if (!resolveTarget(def, target, resolved)) return AudioParamCurve::Linear;
    return resolved.volume ? AudioParamCurve::Decibel : resolved.param->curve;
}

std::vector<std::string> AudioMixerIO::allTargets(const AudioMixerDef& def) {
    std::vector<std::string> out;
    for (const AudioMixerGroupDef* group : def.treeOrder()) {
        out.push_back(volumeTarget(group->id));
    }
    for (const AudioMixerGroupDef* group : def.treeOrder()) {
        for (const AudioMixerEffectDef& effect : group->effects) {
            const AudioEffectInfo* info = AudioEffects::find(effect.type);
            if (!info) continue;
            for (const AudioEffectParamInfo& param : info->params) {
                out.push_back(paramTarget(effect.id, param.name));
            }
            out.push_back(paramTarget(effect.id, "wet"));
        }
    }
    return out;
}

std::string AudioMixerIO::describeTarget(const AudioMixerDef& def, const std::string& target) {
    AudioMixerTarget resolved;
    if (!resolveTarget(def, target, resolved)) return target;
    if (resolved.volume) return resolved.group->name + " volume";
    const AudioEffectInfo* info = AudioEffects::find(resolved.effect->type);
    return resolved.group->name + " > " + (info ? info->label : "?") + " " + resolved.param->name;
}

// ---------------------------------------------------------------- ids

std::string AudioMixerIO::newId(const AudioMixerDef& def) {
    static std::mt19937 rng(std::random_device{}());
    std::set<std::string> used;
    for (const AudioMixerGroupDef& group : def.groups) {
        used.insert(group.id);
        for (const AudioMixerEffectDef& effect : group.effects) used.insert(effect.id);
    }
    static const char* hex = "0123456789abcdef";
    for (;;) {
        std::string id;
        for (int i = 0; i < 8; i++) id += hex[rng() % 16];
        if (!used.count(id)) return id;
    }
}

AudioMixerDef AudioMixerIO::createDefault() {
    AudioMixerDef def;
    AudioMixerGroupDef master;
    master.id = newId(def);
    master.name = "Master";
    def.groups.push_back(master);
    return def;
}

// ---------------------------------------------------------------- validation

namespace {

    std::string groupPath(size_t index) {
        return "groups[" + std::to_string(index) + "]";
    }

    bool validId(const std::string& id) {
        if (id.empty() || id.size() > 64) return false;
        for (char c : id) {
            if (c == '/' || c == '"' || (unsigned char)c < 0x20) return false;
        }
        return true;
    }

}

std::string AudioMixerIO::validate(const AudioMixerDef& def) {
    std::set<std::string> ids;
    std::set<std::string> names;
    int masters = 0;
    for (size_t i = 0; i < def.groups.size(); i++) {
        const AudioMixerGroupDef& group = def.groups[i];
        const std::string path = groupPath(i);
        if (!validId(group.id)) return path + ".id: missing or invalid (8 hex characters)";
        if (!ids.insert(group.id).second) return path + ".id: '" + group.id + "' is used twice";
        if (group.name.empty()) return path + ".name: a group needs a name";
        if (!names.insert(group.name).second) return path + ".name: two groups are named '" + group.name + "' (names are unique)";
        if (group.parentId.empty()) {
            masters++;
        }
        if (group.effects.size() > (size_t)AudioEffects::maxEffectsPerGroup) {
            return path + ".effects: " + std::to_string(group.effects.size()) + " effects, at most " +
                   std::to_string(AudioEffects::maxEffectsPerGroup) + " per group";
        }
        for (size_t e = 0; e < group.effects.size(); e++) {
            const AudioMixerEffectDef& effect = group.effects[e];
            const std::string effectPath = path + ".effects[" + std::to_string(e) + "]";
            if (!validId(effect.id)) return effectPath + ".id: missing or invalid (8 hex characters)";
            if (!ids.insert(effect.id).second) return effectPath + ".id: '" + effect.id + "' is used twice";
        }
    }
    if (masters == 0) return "groups: no Master (the one group with \"parent\": null)";
    if (masters > 1) return "groups: " + std::to_string(masters) + " groups have \"parent\": null; only Master has no parent";
    for (size_t i = 0; i < def.groups.size(); i++) {
        const AudioMixerGroupDef& group = def.groups[i];
        if (group.parentId.empty()) continue;
        if (!def.findGroupById(group.parentId)) return groupPath(i) + ".parent: '" + group.parentId + "' is not a group id";
        if (def.isUnder(group.parentId, group.id)) return groupPath(i) + ".parent: '" + group.name + "' would be its own ancestor";
    }
    // every group hangs under Master
    if (def.treeOrder().size() != def.groups.size()) return "groups: a group does not hang under Master";
    for (size_t i = 0; i < def.groups.size(); i++) {
        for (size_t e = 0; e < def.groups[i].effects.size(); e++) {
            const AudioMixerEffectDef& effect = def.groups[i].effects[e];
            if (effect.type == AudioEffectType::Duck && !effect.sidechainGroup.empty() && !def.findGroupByName(effect.sidechainGroup)) {
                return groupPath(i) + ".effects[" + std::to_string(e) + "].params.sidechainGroup: no group named '" + effect.sidechainGroup + "'";
            }
        }
    }
    std::set<std::string> exposedNames;
    for (size_t i = 0; i < def.exposed.size(); i++) {
        const AudioMixerExposedDef& exposed = def.exposed[i];
        const std::string path = "exposed[" + std::to_string(i) + "]";
        if (exposed.name.empty()) return path + ".name: an exposed parameter needs a name";
        if (!exposedNames.insert(exposed.name).second) return path + ".name: '" + exposed.name + "' is exposed twice (names are unique)";
        AudioMixerTarget resolved;
        if (!resolveTarget(def, exposed.target, resolved)) {
            return path + ".target: '" + exposed.target + "' does not resolve (\"<groupId>/volume\" or \"<effectId>/<param>\")";
        }
    }
    std::set<std::string> snapshotNames;
    for (size_t i = 0; i < def.snapshots.size(); i++) {
        const AudioMixerSnapshotDef& snapshot = def.snapshots[i];
        const std::string path = "snapshots[" + std::to_string(i) + "]";
        if (snapshot.name.empty()) return path + ".name: a snapshot needs a name";
        if (!snapshotNames.insert(snapshot.name).second) return path + ".name: two snapshots are named '" + snapshot.name + "'";
        for (const auto& [target, value] : snapshot.values) {
            AudioMixerTarget resolved;
            if (!resolveTarget(def, target, resolved)) return path + ".values: '" + target + "' does not resolve";
            if (!std::isfinite(value)) return path + ".values." + target + ": not a finite number";
        }
    }
    if (!def.startSnapshot.empty() && !def.findSnapshot(def.startSnapshot)) {
        return "startSnapshot: no snapshot named '" + def.startSnapshot + "'";
    }
    return "";
}

// ---------------------------------------------------------------- parse

namespace {

    bool readNumber(const Json& node, float& out) {
        if (!node.is_number()) return false;
        const double value = node.get<double>();
        if (!std::isfinite(value)) return false;
        out = (float)value;
        return true;
    }

    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

}

bool AudioMixerIO::parse(const std::string& text, AudioMixerDef& out, std::string* error, std::vector<std::string>* warnings) {
    Json root;
    try {
        root = Json::parse(text);
    } catch (const std::exception& e) {
        std::string message = e.what();
        const size_t at = message.find("syntax error");
        return fail(error, "not valid JSON: " + (at != std::string::npos ? message.substr(at) : message));
    }
    if (!root.is_object()) return fail(error, "not an audio mixer: the top level must be a JSON object");
    if (!root.contains("format") || root["format"] != FORMAT) {
        return fail(error, std::string("not an audio mixer: \"format\" must be \"") + FORMAT + "\"");
    }

    AudioMixerDef def;
    if (root.contains("version")) {
        if (!root["version"].is_number_integer() || root["version"].get<int>() < 1) return fail(error, "version: expected a whole number from 1");
        def.version = root["version"].get<int>();
        if (def.version > 2) return fail(error, "version: " + std::to_string(def.version) + " is newer than this engine reads (2)");
    }
    if (root.contains("updateMode")) {
        const Json& mode = root["updateMode"];
        if (mode == "unscaled") def.unscaled = true;
        else if (mode == "scaled") def.unscaled = false;
        else return fail(error, "updateMode: expected \"scaled\" or \"unscaled\"");
    }

    auto warn = [warnings](const std::string& message) {
        if (warnings) warnings->push_back(message);
    };

    if (!root.contains("groups") || !root["groups"].is_array()) return fail(error, "groups: expected an array with Master");
    const Json& groups = root["groups"];
    for (size_t i = 0; i < groups.size(); i++) {
        const Json& node = groups[i];
        const std::string path = groupPath(i);
        if (!node.is_object()) return fail(error, path + ": expected an object");
        AudioMixerGroupDef group;
        if (!node.contains("id") || !node["id"].is_string()) return fail(error, path + ".id: expected a string");
        group.id = node["id"].get<std::string>();
        if (!node.contains("name") || !node["name"].is_string()) return fail(error, path + ".name: expected a string");
        group.name = node["name"].get<std::string>();
        if (node.contains("parent") && !node["parent"].is_null()) {
            if (!node["parent"].is_string()) return fail(error, path + ".parent: expected a group id or null");
            group.parentId = node["parent"].get<std::string>();
            if (group.parentId.empty()) return fail(error, path + ".parent: expected a group id or null");
        }
        if (node.contains("volume")) {
            if (!readNumber(node["volume"], group.volumeDb)) return fail(error, path + ".volume: expected a number (dB)");
            const float clamped = std::clamp(group.volumeDb, AudioMath::minDb, AudioMath::maxDb);
            if (clamped != group.volumeDb) warn(path + ".volume: clamped to " + std::to_string((int)clamped) + " dB");
            group.volumeDb = clamped;
        }
        for (const char* key : {"mute", "solo", "bypassEffects"}) {
            if (!node.contains(key)) continue;
            if (!node[key].is_boolean()) return fail(error, path + "." + key + ": expected true or false");
        }
        group.mute = node.value("mute", false);
        group.solo = node.value("solo", false);
        group.bypassEffects = node.value("bypassEffects", false);
        if (node.contains("effects")) {
            if (!node["effects"].is_array()) return fail(error, path + ".effects: expected an array");
            for (size_t e = 0; e < node["effects"].size(); e++) {
                const Json& effectNode = node["effects"][e];
                const std::string effectPath = path + ".effects[" + std::to_string(e) + "]";
                if (!effectNode.is_object()) return fail(error, effectPath + ": expected an object");
                AudioMixerEffectDef effect;
                if (!effectNode.contains("id") || !effectNode["id"].is_string()) return fail(error, effectPath + ".id: expected a string");
                effect.id = effectNode["id"].get<std::string>();
                if (!effectNode.contains("type") || !effectNode["type"].is_string()) return fail(error, effectPath + ".type: expected a string");
                effect.typeName = effectNode["type"].get<std::string>();
                const AudioEffectInfo* info = AudioEffects::find(effect.typeName);
                if (!info || effect.typeName != info->name) {
                    // "Low Pass" reads as lowpass; anything else is kept as it is, inactive
                    if (info) {
                        effect.typeName = info->name;
                    } else {
                        effect.type = AudioEffectType::Unknown;
                        effect.unknownJson = effectNode.dump();
                        warn(effectPath + ".type: unknown effect '" + effect.typeName + "' (kept in the file, inactive)");
                        group.effects.push_back(effect);
                        continue;
                    }
                }
                effect.type = info->type;
                if (effectNode.contains("bypass")) {
                    if (!effectNode["bypass"].is_boolean()) return fail(error, effectPath + ".bypass: expected true or false");
                    effect.bypass = effectNode["bypass"].get<bool>();
                }
                if (effectNode.contains("wet")) {
                    if (!readNumber(effectNode["wet"], effect.wet)) return fail(error, effectPath + ".wet: expected a number 0..1");
                    effect.wet = std::clamp(effect.wet, 0.0f, 1.0f);
                }
                if (effectNode.contains("params")) {
                    const Json& params = effectNode["params"];
                    if (!params.is_object()) return fail(error, effectPath + ".params: expected an object");
                    for (auto it = params.begin(); it != params.end(); ++it) {
                        if (it.key() == "sidechainGroup" && info->type == AudioEffectType::Duck) {
                            if (!it.value().is_string()) return fail(error, effectPath + ".params.sidechainGroup: expected a group name");
                            effect.sidechainGroup = it.value().get<std::string>();
                            continue;
                        }
                        const AudioEffectParamInfo* param = AudioEffects::findParam(info->type, it.key());
                        if (!param) {
                            return fail(error, effectPath + ".params." + it.key() + ": " + info->name + " has no such parameter (" +
                                               AudioEffects::paramNames(info->type) + ")");
                        }
                        float value = 0.0f;
                        if (!readNumber(it.value(), value)) return fail(error, effectPath + ".params." + it.key() + ": expected a number");
                        const float clamped = std::clamp(value, param->minValue, param->maxValue);
                        if (clamped != value) warn(effectPath + ".params." + it.key() + ": clamped into " +
                                                   std::to_string(param->minValue) + ".." + std::to_string(param->maxValue));
                        if (std::string(param->name) == "wet") effect.wet = clamped;
                        else effect.params[param->name] = clamped;
                    }
                }
                group.effects.push_back(effect);
            }
        }
        def.groups.push_back(group);
    }

    if (root.contains("exposed")) {
        if (!root["exposed"].is_array()) return fail(error, "exposed: expected an array");
        for (size_t i = 0; i < root["exposed"].size(); i++) {
            const Json& node = root["exposed"][i];
            const std::string path = "exposed[" + std::to_string(i) + "]";
            if (!node.is_object()) return fail(error, path + ": expected an object");
            AudioMixerExposedDef exposed;
            if (!node.contains("name") || !node["name"].is_string()) return fail(error, path + ".name: expected a string");
            if (!node.contains("target") || !node["target"].is_string()) return fail(error, path + ".target: expected a string");
            exposed.name = node["name"].get<std::string>();
            exposed.target = node["target"].get<std::string>();
            if (node.contains("remember")) {
                if (!node["remember"].is_boolean()) return fail(error, path + ".remember: expected true or false");
                exposed.remember = node["remember"].get<bool>();
            }
            def.exposed.push_back(exposed);
        }
    }

    if (root.contains("snapshots")) {
        if (!root["snapshots"].is_array()) return fail(error, "snapshots: expected an array");
        for (size_t i = 0; i < root["snapshots"].size(); i++) {
            const Json& node = root["snapshots"][i];
            const std::string path = "snapshots[" + std::to_string(i) + "]";
            if (!node.is_object()) return fail(error, path + ": expected an object");
            AudioMixerSnapshotDef snapshot;
            if (!node.contains("name") || !node["name"].is_string()) return fail(error, path + ".name: expected a string");
            snapshot.name = node["name"].get<std::string>();
            if (node.contains("values")) {
                if (!node["values"].is_object()) return fail(error, path + ".values: expected an object of target: value");
                for (auto it = node["values"].begin(); it != node["values"].end(); ++it) {
                    float value = 0.0f;
                    if (!readNumber(it.value(), value)) return fail(error, path + ".values." + it.key() + ": expected a number");
                    snapshot.values[it.key()] = value;
                }
            }
            def.snapshots.push_back(snapshot);
        }
    }

    if (root.contains("startSnapshot") && !root["startSnapshot"].is_null()) {
        if (!root["startSnapshot"].is_string()) return fail(error, "startSnapshot: expected a snapshot name");
        def.startSnapshot = root["startSnapshot"].get<std::string>();
    }

    const std::string invalid = validate(def);
    if (!invalid.empty()) return fail(error, invalid);

    // snapshot values into their target's range
    for (AudioMixerSnapshotDef& snapshot : def.snapshots) {
        for (auto& [target, value] : snapshot.values) {
            value = clampValue(def, target, value);
        }
    }

    out = std::move(def);
    return true;
}

// ---------------------------------------------------------------- write

namespace {

    // whole numbers without a fraction, the rest as the shortest float text
    OJson number(float value) {
        if (std::fabs(value) < 1e9f && value == std::floor(value)) return (long long)value;
        // the float's shortest text (0.65, not 0.6499999761581421)
        char text[32];
        std::snprintf(text, sizeof(text), "%.7g", value);
        return std::strtod(text, nullptr);
    }

}

// Version 2 is a mixer an older engine must refuse by its version, not by a parameter it does not
// know: only a limiter with a delay or a true-peak detector needs it (S38 R3), so every other mixer
// keeps its bytes.
static bool usesLimiterDelay(const AudioMixerDef& def) {
    for (const AudioMixerGroupDef& group : def.groups) {
        for (const AudioMixerEffectDef& effect : group.effects) {
            if (effect.type != AudioEffectType::Limiter) continue;
            for (const char* name : {"lookahead", "truePeak"}) {
                auto it = effect.params.find(name);
                if (it != effect.params.end() && it->second != 0.0f) return true;
            }
        }
    }
    return false;
}

std::string AudioMixerIO::write(const AudioMixerDef& def) {
    OJson root;
    root["format"] = FORMAT;
    root["version"] = usesLimiterDelay(def) ? 2 : 1;
    root["updateMode"] = def.unscaled ? "unscaled" : "scaled";

    OJson groups = OJson::array();
    for (const AudioMixerGroupDef& group : def.groups) {
        OJson node;
        node["id"] = group.id;
        node["name"] = group.name;
        if (group.parentId.empty()) node["parent"] = nullptr;
        else node["parent"] = group.parentId;
        node["volume"] = number(group.volumeDb);
        if (group.mute) node["mute"] = true;
        if (group.solo) node["solo"] = true;
        if (group.bypassEffects) node["bypassEffects"] = true;
        if (!group.effects.empty()) {
            OJson effects = OJson::array();
            for (const AudioMixerEffectDef& effect : group.effects) {
                if (effect.type == AudioEffectType::Unknown && !effect.unknownJson.empty()) {
                    try {
                        effects.push_back(OJson::parse(effect.unknownJson));
                        continue;
                    } catch (const std::exception&) {
                    }
                }
                OJson effectNode;
                effectNode["id"] = effect.id;
                effectNode["type"] = effect.type == AudioEffectType::Unknown ? effect.typeName : AudioEffects::typeName(effect.type);
                if (effect.bypass) effectNode["bypass"] = true;
                if (effect.wet != 1.0f) effectNode["wet"] = number(effect.wet);
                OJson params = OJson::object();
                if (const AudioEffectInfo* info = AudioEffects::find(effect.type)) {
                    // in the table's order
                    for (const AudioEffectParamInfo& param : info->params) {
                        auto it = effect.params.find(param.name);
                        if (it != effect.params.end()) params[param.name] = number(it->second);
                    }
                }
                if (effect.type == AudioEffectType::Duck && !effect.sidechainGroup.empty()) params["sidechainGroup"] = effect.sidechainGroup;
                if (!params.empty()) effectNode["params"] = params;
                effects.push_back(effectNode);
            }
            node["effects"] = effects;
        }
        groups.push_back(node);
    }
    root["groups"] = groups;

    if (!def.exposed.empty()) {
        OJson exposed = OJson::array();
        for (const AudioMixerExposedDef& entry : def.exposed) {
            OJson node;
            node["name"] = entry.name;
            node["target"] = entry.target;
            if (entry.remember) node["remember"] = true;
            exposed.push_back(node);
        }
        root["exposed"] = exposed;
    }

    if (!def.snapshots.empty()) {
        OJson snapshots = OJson::array();
        for (const AudioMixerSnapshotDef& snapshot : def.snapshots) {
            OJson node;
            node["name"] = snapshot.name;
            OJson values = OJson::object();
            // in the mixer's target order, then any other
            std::set<std::string> written;
            for (const std::string& target : allTargets(def)) {
                auto it = snapshot.values.find(target);
                if (it == snapshot.values.end()) continue;
                values[target] = number(it->second);
                written.insert(target);
            }
            for (const auto& [target, value] : snapshot.values) {
                if (!written.count(target)) values[target] = number(value);
            }
            node["values"] = values;
            snapshots.push_back(node);
        }
        root["snapshots"] = snapshots;
    }

    if (!def.startSnapshot.empty()) root["startSnapshot"] = def.startSnapshot;

    return root.dump(2) + "\n";
}
