// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "SoundEventIO.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

using namespace jasperengine;

using OJson = nlohmann::ordered_json;

namespace {

    constexpr float VOLUME_MIN_DB = -60.0f, VOLUME_MAX_DB = 12.0f;
    constexpr float PITCH_LIMIT_SEMITONES = 24.0f;
    constexpr float DELAY_MAX_MS = 2000.0f;
    constexpr float WEIGHT_MAX = 1000.0f;
    constexpr size_t CLIPS_MAX = 64;
    constexpr int INSTANCES_MAX = 255;
    constexpr float COOLDOWN_MAX_MS = 60000.0f;
    constexpr int PRIORITY_MAX = 256;
    constexpr float ROLLOFF_MAX = 100.0f;
    constexpr float DOPPLER_MAX = 10.0f;

    // the sound library's vocabulary (S36's editor/audio/SoundTypes.h: the engine cannot see the editor's table)
    constexpr const char* SOUND_TYPES[] = {
        "click", "key", "tick", "tap", "pop", "thump", "hit", "boom", "paper", "swish", "whoosh", "transition", "riser",
        "reverse", "ambience", "voice", "glitch", "ui", "zap", "chime", "shimmer", "footstep", "impact", "explosion",
        "pickup", "jump", "hurt", "shoot", "door", "stinger", "music"
    };

    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    bool readFloat(const OJson& node, float& out) {
        if (!node.is_number()) return false;
        const double value = node.get<double>();
        if (!std::isfinite(value)) return false;
        out = (float)value;
        return true;
    }

    bool readFloatKey(const OJson& parent, const char* key, const std::string& path, float& out, std::string* error) {
        if (!parent.contains(key)) return true;
        if (readFloat(parent[key], out)) return true;
        return fail(error, path + key + ": expected a number");
    }

    bool readRange(const OJson& root, const char* key, float& min, float& max, std::string* error) {
        if (!root.contains(key)) return true;
        const OJson& node = root[key];
        if (!node.is_array() || node.size() != 2 || !readFloat(node[0], min) || !readFloat(node[1], max)) {
            return fail(error, std::string(key) + ": expected [min, max]");
        }
        return true;
    }

    template<size_t N>
    SoundEventUnknownKeys unknownKeysOf(const OJson& object, const char* const (&known)[N]) {
        SoundEventUnknownKeys keys;
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (std::find_if(std::begin(known), std::end(known), [&](const char* name) { return it.key() == name; }) == std::end(known)) {
                keys.push_back({it.key(), it.value().dump()});
            }
        }
        return keys;
    }

    void writeUnknownKeys(OJson& object, const SoundEventUnknownKeys& keys) {
        for (const auto& [key, text] : keys) {
            object[key] = OJson::parse(text, nullptr, false);
        }
    }

    // the shortest text a float reads back from: 0.1f is written 0.1, not 0.10000000149011612
    double shortFloat(float value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.7g", (double)value);
        return std::strtod(text, nullptr);
    }

    OJson number(float value) {
        const double shortest = shortFloat(value);
        if (shortest == std::floor(shortest) && std::fabs(shortest) < 1e9) return OJson((int64_t)shortest);
        return OJson(shortest);
    }

    OJson range(float min, float max) {
        OJson node = OJson::array();
        node.push_back(number(min));
        node.push_back(number(max));
        return node;
    }

    void checkRange(std::vector<std::string>& problems, const char* key, float min, float max, float low, float high, const char* unit) {
        char bounds[64];
        std::snprintf(bounds, sizeof(bounds), "%g..%g%s", (double)low, (double)high, unit);
        if (min > max) problems.push_back(std::string(key) + ": min must not be above max");
        if (min < low || min > high || max < low || max > high) problems.push_back(std::string(key) + ": must lie in " + bounds);
    }

    void checkValue(std::vector<std::string>& problems, const std::string& path, float value, float low, float high, const char* unit) {
        if (value >= low && value <= high) return;
        char bounds[64];
        std::snprintf(bounds, sizeof(bounds), "%g..%g%s", (double)low, (double)high, unit);
        problems.push_back(path + ": must lie in " + bounds);
    }

}

bool SoundEventIO::parse(const std::string& json, SoundEventDef& out, std::string* error) {
    OJson root;
    try {
        root = OJson::parse(json);
    } catch (const OJson::exception& e) {
        const std::string message = e.what();
        const size_t at = message.find("syntax error");
        return fail(error, "$: not valid JSON: " + (at != std::string::npos ? message.substr(at) : message));
    }
    if (!root.is_object()) return fail(error, "$: not a Sound Event: the top level must be a JSON object");
    if (!root.contains("format") || !root["format"].is_string() || root["format"].get<std::string>() != FORMAT) {
        return fail(error, std::string("format: not a Sound Event: expected \"") + FORMAT + "\"");
    }
    if (!root.contains("version") || !root["version"].is_number_integer() || root["version"].get<int64_t>() < 1) {
        return fail(error, "version: expected a whole number from 1");
    }
    if (root["version"].get<int64_t>() > VERSION) {
        return fail(error, "version: " + std::to_string(root["version"].get<int64_t>()) + " is newer than this engine reads (" +
                           std::to_string(VERSION) + ")");
    }

    SoundEventDef def;
    if (!root.contains("mode") || !root["mode"].is_string() || !SoundEventDef::modeFromName(root["mode"].get<std::string>(), def.mode)) {
        return fail(error, "mode: expected \"random\", \"shuffle\", \"sequence\" or \"layer\"");
    }
    if (!root.contains("clips") || !root["clips"].is_array()) return fail(error, "clips: expected an array");

    static constexpr const char* CLIP_KEYS[] = {"clip", "weight", "volume_db", "pitch_semitones", "delay_ms"};
    size_t index = 0;
    for (const OJson& node : root["clips"]) {
        const std::string path = "clips[" + std::to_string(index++) + "]";
        if (!node.is_object()) return fail(error, path + ": expected an object");
        if (!node.contains("clip") || !node["clip"].is_object()) return fail(error, path + ".clip: expected {guid, path}");
        const OJson& ref = node["clip"];
        SoundEventClip clip;
        if (ref.contains("path")) {
            if (!ref["path"].is_string()) return fail(error, path + ".clip.path: expected a string");
            clip.clip.path = ref["path"].get<std::string>();
        }
        if (ref.contains("guid")) {
            if (!ref["guid"].is_string() || (!ref["guid"].get<std::string>().empty() && !Guid::isValidText(ref["guid"].get<std::string>()))) {
                return fail(error, path + ".clip.guid: expected 32 hex digits");
            }
            clip.clip.guid = Guid::parse(ref["guid"].get<std::string>());
        }
        const std::string prefix = path + ".";
        if (!readFloatKey(node, "weight", prefix, clip.weight, error) ||
            !readFloatKey(node, "volume_db", prefix, clip.volumeDb, error) ||
            !readFloatKey(node, "pitch_semitones", prefix, clip.pitchSemitones, error) ||
            !readFloatKey(node, "delay_ms", prefix, clip.delayMs, error)) {
            return false;
        }
        clip.unknownKeys = unknownKeysOf(node, CLIP_KEYS);
        def.clips.push_back(std::move(clip));
    }

    if (root.contains("avoid_repeat")) {
        if (!root["avoid_repeat"].is_boolean()) return fail(error, "avoid_repeat: expected true or false");
        def.avoidRepeat = root["avoid_repeat"].get<bool>();
    }
    if (!readRange(root, "pitch_semitones", def.pitchMin, def.pitchMax, error) ||
        !readRange(root, "volume_db", def.volumeMin, def.volumeMax, error) ||
        !readRange(root, "delay_ms", def.delayMin, def.delayMax, error)) {
        return false;
    }
    if (root.contains("output")) {
        const OJson& output = root["output"];
        if (!output.is_object()) return fail(error, "output: expected {id, name}");
        if (output.contains("id")) {
            if (!output["id"].is_string()) return fail(error, "output.id: expected a string");
            def.outputGroupId = output["id"].get<std::string>();
        }
        if (output.contains("name")) {
            if (!output["name"].is_string()) return fail(error, "output.name: expected a string");
            def.outputGroupName = output["name"].get<std::string>();
        }
    }
    if (root.contains("max_instances")) {
        if (!root["max_instances"].is_number_integer()) return fail(error, "max_instances: expected a whole number");
        def.maxInstances = (int)std::clamp<int64_t>(root["max_instances"].get<int64_t>(), -1, INSTANCES_MAX + 1);
    }
    if (root.contains("steal")) {
        if (!root["steal"].is_string() || !SoundEventDef::stealFromName(root["steal"].get<std::string>(), def.steal)) {
            return fail(error, "steal: expected \"oldest\", \"quietest\", \"newest\" or \"none\"");
        }
    }
    if (!readFloatKey(root, "cooldown_ms", "", def.cooldownMs, error)) return false;
    if (root.contains("priority")) {
        if (!root["priority"].is_number_integer()) return fail(error, "priority: expected a whole number");
        def.priority = (int)std::clamp<int64_t>(root["priority"].get<int64_t>(), -1, PRIORITY_MAX + 1);
    }
    if (root.contains("loop")) {
        if (!root["loop"].is_boolean()) return fail(error, "loop: expected true or false");
        def.loop = root["loop"].get<bool>();
    }
    if (root.contains("spatial")) {
        const OJson& node = root["spatial"];
        if (!node.is_object()) return fail(error, "spatial: expected an object");
        SoundEventSpatial spatial;
        if (!readFloatKey(node, "min_distance", "spatial.", spatial.minDistance, error) ||
            !readFloatKey(node, "max_distance", "spatial.", spatial.maxDistance, error) ||
            !readFloatKey(node, "rolloff", "spatial.", spatial.rolloff, error) ||
            !readFloatKey(node, "doppler", "spatial.", spatial.doppler, error)) {
            return false;
        }
        if (node.contains("attenuation")) {
            if (!node["attenuation"].is_string() || !SoundEventDef::attenuationFromName(node["attenuation"].get<std::string>(), spatial.attenuation)) {
                return fail(error, "spatial.attenuation: expected \"no_attenuation\", \"inverse_distance\", \"linear_distance\" or \"exponential_distance\"");
            }
        }
        def.spatial = spatial;
    }
    if (root.contains("type")) {
        if (!root["type"].is_string()) return fail(error, "type: expected a string");
        def.type = root["type"].get<std::string>();
    }

    static constexpr const char* ROOT_KEYS[] = {"format", "version", "mode", "clips", "avoid_repeat", "pitch_semitones", "volume_db",
                                                "delay_ms", "output", "max_instances", "steal", "cooldown_ms", "priority", "loop",
                                                "spatial", "type"};
    def.unknownKeys = unknownKeysOf(root, ROOT_KEYS);
    out = std::move(def);
    return true;
}

std::string SoundEventIO::write(const SoundEventDef& def) {
    const SoundEventDef defaults;
    OJson root = OJson::object();
    root["format"] = FORMAT;
    root["version"] = VERSION;
    root["mode"] = SoundEventDef::modeName(def.mode);
    OJson clips = OJson::array();
    for (const SoundEventClip& clip : def.clips) {
        OJson node = OJson::object();
        OJson ref = OJson::object();
        if (!clip.clip.guid.isNull()) ref["guid"] = clip.clip.guid.str();
        ref["path"] = clip.clip.path;
        node["clip"] = std::move(ref);
        if (clip.weight != 1.0f) node["weight"] = number(clip.weight);
        if (clip.volumeDb != 0.0f) node["volume_db"] = number(clip.volumeDb);
        if (clip.pitchSemitones != 0.0f) node["pitch_semitones"] = number(clip.pitchSemitones);
        if (clip.delayMs != 0.0f) node["delay_ms"] = number(clip.delayMs);
        writeUnknownKeys(node, clip.unknownKeys);
        clips.push_back(std::move(node));
    }
    root["clips"] = std::move(clips);
    if (def.avoidRepeat != defaults.avoidRepeat) root["avoid_repeat"] = def.avoidRepeat;
    if (def.pitchMin != 0.0f || def.pitchMax != 0.0f) root["pitch_semitones"] = range(def.pitchMin, def.pitchMax);
    if (def.volumeMin != 0.0f || def.volumeMax != 0.0f) root["volume_db"] = range(def.volumeMin, def.volumeMax);
    if (def.delayMin != 0.0f || def.delayMax != 0.0f) root["delay_ms"] = range(def.delayMin, def.delayMax);
    if (!def.outputGroupId.empty() || !def.outputGroupName.empty()) {
        OJson output = OJson::object();
        if (!def.outputGroupId.empty()) output["id"] = def.outputGroupId;
        if (!def.outputGroupName.empty()) output["name"] = def.outputGroupName;
        root["output"] = std::move(output);
    }
    if (def.maxInstances != defaults.maxInstances) root["max_instances"] = def.maxInstances;
    if (def.steal != defaults.steal) root["steal"] = SoundEventDef::stealName(def.steal);
    if (def.cooldownMs != defaults.cooldownMs) root["cooldown_ms"] = number(def.cooldownMs);
    if (def.priority != defaults.priority) root["priority"] = def.priority;
    if (def.loop != defaults.loop) root["loop"] = def.loop;
    if (def.spatial) {
        OJson spatial = OJson::object();
        spatial["min_distance"] = number(def.spatial->minDistance);
        spatial["max_distance"] = number(def.spatial->maxDistance);
        spatial["attenuation"] = SoundEventDef::attenuationName(def.spatial->attenuation);
        spatial["rolloff"] = number(def.spatial->rolloff);
        spatial["doppler"] = number(def.spatial->doppler);
        root["spatial"] = std::move(spatial);
    }
    if (!def.type.empty()) root["type"] = def.type;
    writeUnknownKeys(root, def.unknownKeys);
    return root.dump(2) + "\n";
}

const std::vector<std::string>& SoundEventIO::soundTypes() {
    static const std::vector<std::string> types(std::begin(SOUND_TYPES), std::end(SOUND_TYPES));
    return types;
}

std::vector<std::string> SoundEventIO::validate(const SoundEventDef& def) {
    std::vector<std::string> problems;
    if (def.clips.empty() || def.clips.size() > CLIPS_MAX) problems.push_back("clips: expected 1 to 64 clips");
    bool anyWeight = false;
    for (size_t i = 0; i < def.clips.size(); i++) {
        const SoundEventClip& clip = def.clips[i];
        const std::string path = "clips[" + std::to_string(i) + "].";
        if (clip.clip.path.empty() && clip.clip.guid.isNull()) problems.push_back(path + "clip: expected a path or a GUID");
        checkValue(problems, path + "weight", clip.weight, 0.0f, WEIGHT_MAX, "");
        checkValue(problems, path + "volume_db", clip.volumeDb, VOLUME_MIN_DB, VOLUME_MAX_DB, " dB");
        checkValue(problems, path + "pitch_semitones", clip.pitchSemitones, -PITCH_LIMIT_SEMITONES, PITCH_LIMIT_SEMITONES, " semitones");
        checkValue(problems, path + "delay_ms", clip.delayMs, 0.0f, DELAY_MAX_MS, " ms");
        anyWeight = anyWeight || clip.weight > 0.0f;
    }
    if (def.mode == SoundEventMode::Random && !def.clips.empty() && !anyWeight) {
        problems.push_back("clips: random mode needs a clip with a weight above 0");
    }
    checkRange(problems, "pitch_semitones", def.pitchMin, def.pitchMax, -PITCH_LIMIT_SEMITONES, PITCH_LIMIT_SEMITONES, " semitones");
    checkRange(problems, "volume_db", def.volumeMin, def.volumeMax, VOLUME_MIN_DB, VOLUME_MAX_DB, " dB");
    checkRange(problems, "delay_ms", def.delayMin, def.delayMax, 0.0f, DELAY_MAX_MS, " ms");
    if (def.maxInstances < 0 || def.maxInstances > INSTANCES_MAX) problems.push_back("max_instances: must lie in 0..255 (0 = no limit)");
    checkValue(problems, "cooldown_ms", def.cooldownMs, 0.0f, COOLDOWN_MAX_MS, " ms");
    if (def.priority < 0 || def.priority > PRIORITY_MAX) problems.push_back("priority: must lie in 0..256");
    if (def.spatial) {
        if (!(def.spatial->minDistance > 0.0f)) problems.push_back("spatial.min_distance: must be above 0");
        if (!(def.spatial->maxDistance >= def.spatial->minDistance)) problems.push_back("spatial.max_distance: must not be below min_distance");
        checkValue(problems, "spatial.rolloff", def.spatial->rolloff, 0.0f, ROLLOFF_MAX, "");
        checkValue(problems, "spatial.doppler", def.spatial->doppler, 0.0f, DOPPLER_MAX, "");
    }
    if (!def.type.empty() && std::none_of(std::begin(SOUND_TYPES), std::end(SOUND_TYPES), [&](const char* name) { return def.type == name; })) {
        problems.push_back("type: '" + def.type + "' is not a sound type");
    }
    return problems;
}

#endif // JASPERENGINE_SOUND_LAYER
