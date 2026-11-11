// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundEventDocument.h"

#include "Out.h"
#include "Project.h"
#include "audio/AudioFileCheck.h"
#include "audio/AudioImportSettings.h"
#include "audio/AudioMixer.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool soundEventFail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    bool readText(const fs::path& file, std::string& text) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}

bool SoundEventDocument::read(const fs::path& file, SoundEventDef& def, std::string* error) {
    std::string text;
    if (!readText(file, text)) return soundEventFail(error, "cannot read " + FileUtils::pathToGenericUtf8(file));
    return SoundEventIO::parse(text, def, error);
}

bool SoundEventDocument::commit(Project* project, const fs::path& file, const SoundEventDef& def, const std::string& undoName,
                                const std::string& mergeKey, std::string* error) {
    if (!project) return soundEventFail(error, "no project is open");
    // a new event has no clip yet: it is saved, and until it has one it plays nothing
    for (const std::string& problem : SoundEventIO::validate(def)) {
        if (!def.clips.empty() || problem.rfind("clips:", 0) != 0) return soundEventFail(error, problem);
    }
    const std::string text = SoundEventIO::write(def);

    std::string current;
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && readText(file, current) && current == text) return true;

    auto* command = new WriteFilesCmd({{file, text}}, [project](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(project, changed);
    });
    command->setName(undoName);
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey("sound-event\n" + FileUtils::pathToGenericUtf8(file) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        return soundEventFail(error, "writing " + FileUtils::pathToGenericUtf8(file) + " failed" + (failure->empty() ? "" : ": " + *failure));
    }
    return true;
}

std::vector<fs::path> SoundEventDocument::relinkGuids(Project* project, const std::map<std::string, fs::path>& moved) {
    std::vector<fs::path> written;
    if (!project || project->getProjectPath().empty() || moved.empty()) return written;
    for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath().lexically_normal(), {".soundevent"})) {
        std::string text;
        if (!ProjectChecks::readFile(file, text)) continue;
        bool named = false;
        for (const auto& entry : moved) named = named || text.find(entry.first) != std::string::npos;
        if (!named) continue;
        SoundEventDef def;
        if (!SoundEventIO::parse(text, def)) continue;
        bool changed = false;
        for (SoundEventClip& clip : def.clips) {
            const auto found = moved.find(clip.clip.guid.str());
            if (found == moved.end()) continue;
            const std::string form = project->normalizeToAssetsRelative(found->second).generic_string();
            if (fs::path(clip.clip.path).lexically_normal().generic_string() == form) continue;
            clip.clip.path = form;
            changed = true;
        }
        if (!changed) continue;
        if (!FileUtils::writeFileReplacing(file, SoundEventIO::write(def))) {
            Out::warning("Could not update the clip paths of %s", file.string().c_str());
            continue;
        }
        written.push_back(file);
    }
    if (!written.empty()) ProjectFileWrites::filesWritten(project, written);
    return written;
}

AssetRef SoundEventDocument::clipRef(Project* project, const std::string& pathOrGuid, std::string* error) {
    AssetRef ref;
    if (Guid::isValidText(pathOrGuid)) {
        const Guid guid = Guid::parse(pathOrGuid);
        const fs::path found = project->getAssetDatabase().getPath(guid);
        if (found.empty()) {
            soundEventFail(error, "no asset has the GUID " + pathOrGuid);
            return ref;
        }
        ref.guid = guid;
        ref.path = project->normalizeToAssetsRelative(project->getProjectPath() / found).generic_string();
        return ref;
    }
    const fs::path resolved = project->resolveAssetPath(FileUtils::pathFromUtf8(pathOrGuid)).lexically_normal();
    ref.path = project->normalizeToAssetsRelative(resolved).generic_string();
    const fs::path relative = project->normalizeToProjectRelative(resolved);
    ref.guid = project->getAssetDatabase().getAssetGuid(relative);
    return ref;
}

namespace {

    using Json = nlohmann::json;

    bool numberValue(const Json& value, const std::string& key, float& out, std::string* error) {
        if (!value.is_number()) return soundEventFail(error, key + ": expected a number");
        out = value.get<float>();
        return true;
    }

    bool rangeValue(const Json& value, const std::string& key, float& min, float& max, std::string* error) {
        if (value.is_null()) {
            min = max = 0.0f;
            return true;
        }
        if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
            return soundEventFail(error, key + ": expected [min, max]");
        }
        min = value[0].get<float>();
        max = value[1].get<float>();
        return true;
    }

    bool clipValue(Project* project, const Json& item, const std::string& path, SoundEventClip& clip, std::string* error) {
        auto ref = [&](const Json& value) {
            std::string why;
            clip.clip = SoundEventDocument::clipRef(project, value.get<std::string>(), &why);
            if (!why.empty()) return soundEventFail(error, path + ": " + why);
            return true;
        };
        if (item.is_string()) return ref(item);
        if (!item.is_object() || !item.contains("clip") || !item["clip"].is_string()) {
            return soundEventFail(error, path + ": expected a path, a GUID or {clip, weight, volume_db, pitch_semitones, delay_ms, loops}");
        }
        if (!ref(item["clip"])) return false;
        for (auto it = item.begin(); it != item.end(); ++it) {
            const std::string key = path + "." + it.key();
            if (it.key() == "clip") continue;
            if (it.key() == "loops") {
                if (!it.value().is_number_integer()) return soundEventFail(error, key + ": expected a whole number (0 = until Audio.advanceMusic)");
                clip.loops = (int)std::clamp<int64_t>(it.value().get<int64_t>(), -1, 1001);
                continue;
            }
            float* field = it.key() == "weight" ? &clip.weight : it.key() == "volume_db" ? &clip.volumeDb
                         : it.key() == "pitch_semitones" ? &clip.pitchSemitones : it.key() == "delay_ms" ? &clip.delayMs : nullptr;
            if (!field) return soundEventFail(error, key + ": unknown (weight, volume_db, pitch_semitones, delay_ms, loops)");
            if (!numberValue(it.value(), key, *field, error)) return false;
        }
        return true;
    }

}

bool SoundEventDocument::applyValues(Project* project, SoundEventDef& def, const Json& values, std::string* error) {
    if (!values.is_object()) return soundEventFail(error, "values: expected an object");
    const SoundEventDef defaults;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::string& key = it.key();
        const Json& value = it.value();
        const bool reset = value.is_null();
        if (key == "mode") {
            if (reset) def.mode = defaults.mode;
            else if (!value.is_string() || !SoundEventDef::modeFromName(value.get<std::string>(), def.mode)) {
                return soundEventFail(error, "mode: expected random, shuffle, sequence or layer");
            }
        } else if (key == "clips") {
            if (!value.is_array()) return soundEventFail(error, "clips: expected a list of paths, GUIDs or {clip, weight, ...}");
            def.clips.clear();
            for (size_t i = 0; i < value.size(); i++) {
                SoundEventClip clip;
                if (!clipValue(project, value[i], "clips[" + std::to_string(i) + "]", clip, error)) return false;
                def.clips.push_back(std::move(clip));
            }
        } else if (key == "avoid_repeat") {
            if (reset) def.avoidRepeat = defaults.avoidRepeat;
            else if (value.is_boolean()) def.avoidRepeat = value.get<bool>();
            else return soundEventFail(error, "avoid_repeat: expected true or false");
        } else if (key == "pitch_semitones") {
            if (!rangeValue(value, key, def.pitchMin, def.pitchMax, error)) return false;
        } else if (key == "volume_db") {
            if (!rangeValue(value, key, def.volumeMin, def.volumeMax, error)) return false;
        } else if (key == "delay_ms") {
            if (!rangeValue(value, key, def.delayMin, def.delayMax, error)) return false;
        } else if (key == "output_group") {
            def.outputGroupId.clear();
            def.outputGroupName.clear();
            if (!reset) {
                if (!value.is_string()) return soundEventFail(error, "output_group: expected a group's name");
                def.outputGroupName = value.get<std::string>();
                def.outputGroupId = AudioMixer::findGroupId(def.outputGroupName);
            }
        } else if (key == "max_instances") {
            if (reset) def.maxInstances = defaults.maxInstances;
            else if (value.is_number_integer()) def.maxInstances = value.get<int>();
            else return soundEventFail(error, "max_instances: expected a whole number (0 = no limit)");
        } else if (key == "steal") {
            if (reset) def.steal = defaults.steal;
            else if (!value.is_string() || !SoundEventDef::stealFromName(value.get<std::string>(), def.steal)) {
                return soundEventFail(error, "steal: expected oldest, quietest, newest or none");
            }
        } else if (key == "cooldown_ms") {
            if (reset) def.cooldownMs = defaults.cooldownMs;
            else if (!numberValue(value, key, def.cooldownMs, error)) return false;
        } else if (key == "priority") {
            if (reset) def.priority = defaults.priority;
            else if (value.is_number_integer()) def.priority = value.get<int>();
            else return soundEventFail(error, "priority: expected a whole number");
        } else if (key == "loop") {
            if (reset) def.loop = defaults.loop;
            else if (value.is_boolean()) def.loop = value.get<bool>();
            else return soundEventFail(error, "loop: expected true or false");
        } else if (key == "spatial") {
            if (reset) {
                def.spatial.reset();
                continue;
            }
            if (!value.is_object()) return soundEventFail(error, "spatial: expected {min_distance, max_distance, attenuation, rolloff, doppler}");
            SoundEventSpatial spatial = def.spatial.value_or(SoundEventSpatial());
            for (auto field = value.begin(); field != value.end(); ++field) {
                const std::string path = "spatial." + field.key();
                if (field.key() == "attenuation") {
                    if (!field.value().is_string() || !SoundEventDef::attenuationFromName(field.value().get<std::string>(), spatial.attenuation)) {
                        return soundEventFail(error, path + ": expected no_attenuation, inverse_distance, linear_distance or exponential_distance");
                    }
                    continue;
                }
                float* number = field.key() == "min_distance" ? &spatial.minDistance : field.key() == "max_distance" ? &spatial.maxDistance
                              : field.key() == "rolloff" ? &spatial.rolloff : field.key() == "doppler" ? &spatial.doppler : nullptr;
                if (!number) return soundEventFail(error, path + ": unknown (min_distance, max_distance, attenuation, rolloff, doppler)");
                if (!numberValue(field.value(), path, *number, error)) return false;
            }
            def.spatial = spatial;
        } else if (key == "type") {
            if (reset) def.type.clear();
            else if (value.is_string()) def.type = value.get<std::string>();
            else return soundEventFail(error, "type: expected a sound type");
        } else {
            return soundEventFail(error, "values." + key + ": unknown. Keys: mode, clips, avoid_repeat, pitch_semitones, volume_db, delay_ms, "
                               "output_group, max_instances, steal, cooldown_ms, priority, loop, spatial, type");
        }
    }
    return true;
}

Json SoundEventDocument::describe(const SoundEventDef& def) {
    Json clips = Json::array();
    for (const SoundEventClip& clip : def.clips) {
        clips.push_back({{"clip", clip.clip.path}, {"guid", clip.clip.guid.str()}, {"weight", clip.weight}, {"volume_db", clip.volumeDb},
                         {"pitch_semitones", clip.pitchSemitones}, {"delay_ms", clip.delayMs}, {"loops", clip.loops}});
    }
    Json out = {{"mode", SoundEventDef::modeName(def.mode)},
                {"clips", clips},
                {"avoid_repeat", def.avoidRepeat},
                {"pitch_semitones", {def.pitchMin, def.pitchMax}},
                {"volume_db", {def.volumeMin, def.volumeMax}},
                {"delay_ms", {def.delayMin, def.delayMax}},
                {"output_group", def.outputGroupName},
                {"max_instances", def.maxInstances},
                {"steal", SoundEventDef::stealName(def.steal)},
                {"cooldown_ms", def.cooldownMs},
                {"priority", def.priority},
                {"loop", def.loop},
                {"type", def.type}};
    if (def.spatial) {
        out["spatial"] = {{"min_distance", def.spatial->minDistance}, {"max_distance", def.spatial->maxDistance},
                          {"attenuation", SoundEventDef::attenuationName(def.spatial->attenuation)},
                          {"rolloff", def.spatial->rolloff}, {"doppler", def.spatial->doppler}};
    }
    return out;
}

namespace {

    // An event played as music (S39 R3): its sections play one after the other in one voice, so they
    // share its rate and channels, and each is long enough to prepare the next one while it plays
    void musicProblems(Project* project, const SoundEventDef& def, std::vector<std::string>& out) {
        const AudioFileFormat* first = nullptr;
        std::string firstPath;
        std::vector<AudioFileFormat> formats(def.clips.size());
        for (size_t i = 0; i < def.clips.size(); i++) {
            const AssetRef& clip = def.clips[i].clip;
            fs::path file = clip.guid.isNull() ? fs::path() : project->getAssetDatabase().getPath(clip.guid);
            file = project->resolveAssetPath(file.empty() ? FileUtils::pathFromUtf8(clip.path) : file);
            std::ifstream in(file, std::ios::binary);
            if (!in) continue;
            const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string error;
            AudioFileFormat& format = formats[i];
            if (bytes.empty() || !AudioFileCheck::check(bytes.data(), bytes.size(), AudioDecode::Stream, format, error) || format.sampleRate == 0) continue;
            const std::string path = "clips[" + std::to_string(i) + "]: ";
            if (!first) {
                first = &format;
                firstPath = clip.path;
            } else if (format.sampleRate != first->sampleRate || format.channels != first->channels) {
                out.push_back(path + "music sections differ in sample rate or channels: " + clip.path + " is " + std::to_string(format.sampleRate) + " Hz, " +
                              std::to_string(format.channels) + " channels; " + firstPath + " is " + std::to_string(first->sampleRate) + " Hz, " +
                              std::to_string(first->channels) + " channels");
            }
            // the trims a number gives (an `auto` trim is resolved by the analysis, not here)
            AudioImportSettings settings;
            std::string why;
            uint64_t frames = format.frames;
            if (AudioImportSettings::load(file, settings, why)) {
                const double cutMs = (settings.trimHead.automatic ? 0.0 : settings.trimHead.ms) + (settings.trimTail.automatic ? 0.0 : settings.trimTail.ms);
                const uint64_t cut = (uint64_t)std::llround(cutMs * format.sampleRate / 1000.0);
                frames = frames > cut ? frames - cut : 0;
            }
            if (frames * 10 < format.sampleRate) out.push_back(path + "a music section is shorter than 100 ms: " + clip.path);
        }
    }

}

std::vector<std::string> SoundEventDocument::problems(Project* project, const SoundEventDef& def) {
    std::vector<std::string> out = SoundEventIO::validate(def);
    for (size_t i = 0; i < def.clips.size(); i++) {
        const AssetRef& clip = def.clips[i].clip;
        const fs::path byGuid = clip.guid.isNull() ? fs::path() : project->getAssetDatabase().getPath(clip.guid);
        std::error_code ec;
        const bool exists = !byGuid.empty() || fs::is_regular_file(project->resolveAssetPath(FileUtils::pathFromUtf8(clip.path)), ec);
        if (!exists) out.push_back("clips[" + std::to_string(i) + "]: missing clip " + clip.path);
    }
    if (def.type == "music") musicProblems(project, def, out);
    const bool named = !def.outputGroupName.empty() || !def.outputGroupId.empty();
    if (named && AudioMixer::findGroupId(def.outputGroupId).empty() && AudioMixer::findGroupId(def.outputGroupName).empty()) {
        out.push_back("output_group: '" + (def.outputGroupName.empty() ? def.outputGroupId : def.outputGroupName) +
                      "' is not in the mixer (it plays through Master)");
    }
    return out;
}
