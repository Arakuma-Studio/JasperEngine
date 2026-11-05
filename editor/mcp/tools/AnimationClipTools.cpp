// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The clip-file mode of edit_animation / get_animation / preview_animation (animation clips
// spec 5, 9.R4/R28/R29). A clip is an .anim asset edited through AnimationClipDocument (one
// project undo step per call, all or nothing; create {from: <entity Animation>} is one scene step
// with the Animation Player it adds), its tracks bound by path relative to the GameObject that
// plays it. The key grammar is edit_animation's (AnimationKeys.h); results report the bindings
// against a root (ClipValidation), the components sharing the file and the undo scope.
#include "AnimationClipTools.h"
#include "AnimatorTools.h"
#include "AnimationKeys.h"

#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipAudio.h"
#include "animation/ClipBinding.h"
#include "animation/ClipConverters.h"
#include "animation/ClipPropertyNames.h"
#include "animation/ClipEditorUtil.h"
#include "animation/AvatarMaskWeights.h"
#include "animation/HumanoidClip.h"
#include "pose/PoseRigProvider.h"
#include "pool/AnimationClipPool.h"
#include "pose/PoseDofs.h"
#include "pose/PoseMath.h"
#include "pose/PoseRig.h"
#include "animation/ClipValidation.h"
#include "animation/PropertyBindings.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "audio/AudioMixer.h"
#include "audio/TimelineAudio.h"
#include "component/AudioSourceComponent.h"
#include "util/AssetDatabase.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/ModelComponent.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/Transform.h"
#include "subsystem/ActionSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include "command/type/ClipSceneActions.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace jasperengine::editor::mcp::clip_tools {

namespace {

namespace fs = std::filesystem;
using anim_keys::Channel;
using anim_keys::Key;

constexpr float MERGE_TIME = 0.001f;
// the cage resolution limit (LatticeComponent clamps to 2..16 per axis): flat indices a clip can
// hold when no cage is given to check them against
constexpr int MAX_LATTICE_POINTS = 16 * 16 * 16;

bool has(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

double r6(double value) {
    if (!std::isfinite(value)) return 0.0;
    return std::round(value * 1e6) / 1e6;
}

std::string fmt(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3g", value);
    return buffer;
}

std::string plural(size_t count, const std::string& word) {
    return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
}

std::string projectRel(Project* project, const fs::path& absolute) {
    return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
}

// A stored (asset-relative) clip entry shown project-relative, as the tools take it
std::string entryClipJson(Project* project, const std::string& stored) {
    if (stored.empty()) return stored;
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(stored, reference)) {
        return projectRel(project, project->resolveAssetPath(FileUtils::pathFromUtf8(reference.model)).lexically_normal()) + "#" + reference.clip;
    }
    return projectRel(project, project->resolveAssetPath(FileUtils::pathFromUtf8(stored)).lexically_normal());
}

const std::vector<std::pair<const char*, ClipChannel>>& channelTable() {
    static const std::vector<std::pair<const char*, ClipChannel>> table = {
        {"position", ClipChannel::Position}, {"rotation", ClipChannel::Rotation}, {"scale", ClipChannel::Scale},
        {"morph", ClipChannel::Morph}, {"lattice", ClipChannel::Lattice},
        {"humanoid", ClipChannel::Humanoid}, {"humanoid_position", ClipChannel::HumanoidPosition}};
    return table;
}

// a humanoid slot's joint angles (degrees) of an E (x y z w): the pose system's DOFs by name
Json humanoidAnglesJson(int humanBone, const float* xyzw) {
    if (humanBone < 0 || humanBone >= pose::HUMAN_BONE_COUNT) return nullptr;
    float angles[pose::DOFS_PER_BONE];
    pose::jointAngles(pose::boneAt(humanBone), pose::Quatd(quaternionFromXYZW(xyzw)).normalized(), angles);
    Json out = Json::object();
    const pose::BoneDofInfo& info = pose::boneDofInfo(pose::boneAt(humanBone));
    for (int k = 0; k < pose::DOFS_PER_BONE; k++) out[info.dofs[k].name] = std::round(angles[k] * 1000.0) / 1000.0;
    return out;
}

Channel keyChannel(ClipChannel channel) {
    switch (channel) {
        case ClipChannel::Position: return Channel::Position;
        case ClipChannel::Rotation: return Channel::Rotation;
        case ClipChannel::Scale: return Channel::Scale;
        case ClipChannel::Morph: return Channel::Morph;
        default: return Channel::Lattice;
    }
}

std::string wrapNames() {
    return "once, loop, ping_pong or clamp_forever";
}

// ------------------------------------------------------------------ paths

// `value`: a project-relative .anim path, or (allowSubClip) "<model>#<clip>". `relative` gets
// the project-relative form (the document key), `absolute` the file (the model of a sub-clip).
// `write`: the tool writes the .anim (edit_animation): inside the project once links are resolved
bool resolveClipArg(Project* project, const Json& value, const std::string& what, bool allowSubClip,
                    std::string& relative, fs::path& absolute, bool& subClip, std::string& error, bool write = false) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        error = what + " must be a project-relative .anim path (e.g. assets/anim/bob.anim)" +
                std::string(allowSubClip ? " or a model clip \"assets/models/hero.gltf#Walk\"" : "") + ".";
        return false;
    }
    const std::string given = value.get<std::string>();
    anim::SubClipReference reference;
    subClip = anim::parseSubClipReference(given, reference);
    if (subClip && !allowSubClip) {
        error = what + ": \"" + given + "\" is a model clip (read-only); pass a new .anim path and create: {from: \"" + given + "\"} to extract it.";
        return false;
    }
    const std::string file = subClip ? reference.model : given;
    if (!subClip && lower(editor::FileUtils::pathFromUtf8(file).extension().string()) != ".anim") {
        error = what + ": \"" + given + "\" must end in .anim" +
                std::string(allowSubClip ? " (or name a model clip: \"assets/models/hero.gltf#Walk\")" : "") + ".";
        return false;
    }
    if ((write && !subClip) ? !McpRefs::resolveProjectWritePath(project, file, absolute, error, false)
                            : !McpRefs::resolveProjectPath(project, file, absolute, error, subClip)) {
        error = what + ": " + error;
        return false;
    }
    absolute = absolute.lexically_normal();
    relative = projectRel(project, absolute) + (subClip ? "#" + reference.clip : std::string());
    return true;
}

// The Animation Player entry form of a project path: asset-relative, '/' separated
std::string assetForm(Project* project, const std::string& projectRelative) {
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(projectRelative, reference)) {
        return FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(project->getProjectPath() / FileUtils::pathFromUtf8(reference.model))) +
               "#" + reference.clip;
    }
    return FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(project->getProjectPath() / FileUtils::pathFromUtf8(projectRelative)));
}

bool insideAssets(Project* project, const fs::path& absolute) {
    return project->isInsideAssetsPath(project->normalizeToAssetsRelative(absolute));
}

// ------------------------------------------------------------------ JSON of clip data

Json argJson(const AnimationEventArg& arg) {
    if (const double* number = std::get_if<double>(&arg)) return r6(*number);
    if (const bool* flag = std::get_if<bool>(&arg)) return *flag;
    return std::get<std::string>(arg);
}

Json floatsJson(const std::vector<float>& values, size_t from, size_t count) {
    Json out = Json::array();
    for (size_t i = from; i < from + count && i < values.size(); i++) out.push_back(r6(values[i]));
    return out;
}

Json quatJson(const float* xyzw) {
    Json q = McpValues::toJson(quaternionFromXYZW(xyzw));
    // the stored quaternion, not the engine's float noise
    q["quat"] = Json::array({r6(xyzw[0]), r6(xyzw[1]), r6(xyzw[2]), r6(xyzw[3])});
    return q;
}

// A key value (ClipKey::value layout) as the tools write it
Json valueJson(const ClipTrackData& track, const std::vector<float>& value) {
    switch (track.channel) {
        case ClipChannel::Position:
        case ClipChannel::Scale:
            return floatsJson(value, 0, 3);
        case ClipChannel::Rotation:
            if (value.size() >= 4) return quatJson(value.data());
            return Json::array();
        case ClipChannel::Humanoid: {
            // {quat, angles} (slots: the joint angles; extra bones: the quaternion only)
            if (value.size() < 4) return Json::array();
            Json q = {{"quat", Json::array({r6(value[0]), r6(value[1]), r6(value[2]), r6(value[3])})}};
            Json angles = humanoidAnglesJson(track.humanBone, value.data());
            if (!angles.is_null()) q["angles"] = angles;
            return q;
        }
        case ClipChannel::HumanoidPosition:
            return floatsJson(value, 0, 3);
        case ClipChannel::Morph:
            return floatsJson(value, 0, value.size());
        case ClipChannel::Lattice: {
            Json points = Json::object();
            for (size_t n = 0; n < track.points.size() && n * 3 + 2 < value.size(); n++) {
                points[std::to_string(track.points[n])] = floatsJson(value, n * 3, 3);
            }
            return points;
        }
        default:
            return nullptr;
    }
}

// a property value in key space (anim-properties spec 11): bools as JSON bools, ints and frames
// as integers, one float as a number, else an array (colours sRGB)
Json propertyValueJson(PropertyKind kind, const std::vector<float>& value) {
    if (value.empty()) return nullptr;
    if (kind == PropertyKind::Bool && value.size() == 1) return value[0] > 0.5f;
    if ((kind == PropertyKind::Int || kind == PropertyKind::Frame) && value.size() == 1) return (long long)std::llround(value[0]);
    if (value.size() == 1) return r6(value[0]);
    return floatsJson(value, 0, value.size());
}

const char* tangentModeName(ClipTangentMode mode) {
    switch (mode) {
        case ClipTangentMode::Auto: return "auto";
        case ClipTangentMode::Free: return "free";
        case ClipTangentMode::Flat: return "flat";
        case ClipTangentMode::Broken: return "broken";
        case ClipTangentMode::Linear: return "linear";
        default: return "";
    }
}

Json keyJson(const ClipTrackData& track, const ClipKey& key) {
    Json item = {{"time", r6(key.time)}};
    if (track.channel == ClipChannel::Lattice) item["points"] = valueJson(track, key.value);
    else if (track.channel == ClipChannel::Property) item["value"] = propertyValueJson(track.kind, key.value);
    else item["value"] = valueJson(track, key.value);
    if (key.ease != EaseType::LINEAR) item["ease"] = anim::easeName(key.ease);
    if (key.hasTangents() && track.channel != ClipChannel::Lattice) {
        item["in"] = floatsJson(key.inTangent, 0, key.inTangent.size());
        item["out"] = floatsJson(key.outTangent, 0, key.outTangent.size());
        if (key.tangentMode != ClipTangentMode::None) item["tangent_mode"] = tangentModeName(key.tangentMode);
    }
    return item;
}

Json trackJson(const ClipTrackData& track, size_t index, size_t maxKeys) {
    Json item = {{"index", index}, {"path", track.path}, {"channel", anim::channelName(track.channel)}};
    if (isHumanoidChannel(track.channel)) {
        item.erase("path");
        item["bone"] = anim::humanoidTrackBoneName(track.channel, track.humanBone);
    }
    if (track.channel == ClipChannel::Property) {
        item["component"] = track.component;
        item["property"] = track.property;
        item["sampled"] = track.known;
        if (!track.known) return item;
        item["kind"] = anim::kindName(track.kind);
        if (track.discrete) item["discrete"] = true;
    }
    Json keys = Json::array();
    for (size_t k = 0; k < track.keys.size() && k < maxKeys; k++) keys.push_back(keyJson(track, track.keys[k]));
    item["key_count"] = track.keys.size();
    item["keys"] = keys;
    item["loop"] = track.loop;
    if (!isHumanoidChannel(track.channel)) item["relative"] = track.relative;
    if (track.channel == ClipChannel::Lattice) item["points"] = track.points;
    return item;
}

Json eventsJson(const std::vector<ClipEventData>& events) {
    Json out = Json::array();
    for (const ClipEventData& event : events) {
        Json args = Json::array();
        for (const AnimationEventArg& arg : event.args) args.push_back(argJson(arg));
        out.push_back({{"time", r6(event.time)}, {"function", event.function}, {"args", args}});
    }
    return out;
}

// ------------------------------------------------------------------ audio cues (S40 R12)

std::string projectPathOf(Project* project, const std::string& assetRelative);

namespace {

bool isAudioClipExtension(const std::string& extension) {
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3" || extension == ".flac";
}

// a sound's played length at speed 1 (seconds), 0 when it does not resolve
double soundSeconds(const AssetRef& sound) {
#if JASPERENGINE_SOUND_LAYER
    return sound.path.empty() ? 0.0 : TimelineAudio::soundLength(sound.path);
#else
    (void)sound;
    return 0.0;
#endif
}

} // namespace

bool resolveSoundRef(Project* project, const Json& value, const std::string& where, AssetRef& out, std::string& error) {
    std::string given;
    std::string guid;
    if (value.is_string()) {
        given = value.get<std::string>();
        if (AssetDatabase::isGuid(given)) std::swap(given, guid);
    } else if (value.is_object()) {
        if (value.contains("guid") && value["guid"].is_string()) guid = value["guid"].get<std::string>();
        if (value.contains("path") && value["path"].is_string()) given = value["path"].get<std::string>();
    }
    if (given.empty() && guid.empty()) {
        error = where + " must be the project path of an audio clip or .soundevent (e.g. \"assets/sounds/step.soundevent\"), or its GUID.";
        return false;
    }
    if (!guid.empty()) {
        const fs::path known = project->getAssetDatabase().getPath(guid);
        if (known.empty() && given.empty()) {
            error = where + ": GUID " + guid + " is no asset of this project: pass the sound's project path (e.g. \"assets/sounds/step.wav\").";
            return false;
        }
        if (!known.empty()) given = FileUtils::pathToGenericUtf8(known);
    }
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(project, given, absolute, error, true)) {
        error = where + ": " + error;
        return false;
    }
    const std::string relative = projectRel(project, absolute.lexically_normal());
    const std::string extension = lower(FileUtils::pathFromUtf8(relative).extension().string());
    if (extension != ".soundevent" && !isAudioClipExtension(extension)) {
        error = where + ": \"" + relative + "\" is not a sound: give an audio clip (.wav, .ogg, .mp3, .flac) or a .soundevent.";
        return false;
    }
    out.path = assetForm(project, relative);
    out.guid = project->getAssetDatabase().getAssetGuid(FileUtils::pathFromUtf8(relative));
    return true;
}

bool parseAudioCue(Project* project, const Json& item, const std::string& where, bool entityMode, ClipAudioCue& cue, std::string& error) {
    if (!item.is_object()) {
        error = where + " must be {time, and one of sound, source, snapshot}.";
        return false;
    }
    static const char* const clipKeys[] = {"time", "sound", "source", "snapshot", "path", "spatial", "volume_db", "pitch_semitones",
                                           "group", "stop", "follow_speed", "name", "seconds", "unscaled"};
    for (auto it = item.begin(); it != item.end(); ++it) {
        const std::string& key = it.key();
        bool known = entityMode && (key == "duration" || key == "lane");
        for (const char* k : clipKeys) known = known || key == k;
        if (!known) {
            error = where + ": unknown key \"" + key + "\" (a cue is {time, sound | source | snapshot, path, spatial, volume_db, "
                    "pitch_semitones, group, stop, follow_speed, name, seconds, unscaled}" + std::string(entityMode ? ", plus duration and lane" : "") + ").";
            return false;
        }
    }
    const int kinds = (int)item.contains("sound") + (int)item.contains("source") + (int)item.contains("snapshot");
    if (kinds != 1) {
        error = where + (kinds == 0 ? ": a cue needs what it plays" : ": a cue plays one thing") +
                ": give exactly one of sound (a clip or .soundevent), source (the path of a GameObject with an AudioSource) or "
                "snapshot (a mixer snapshot name)" + std::string(kinds > 1 ? "; split them into cues at the same time." : ".");
        return false;
    }
    if (!item.contains("time") || !item["time"].is_number() || !std::isfinite(item["time"].get<double>()) || item["time"].get<double>() < 0.0) {
        error = where + ".time must be a number of seconds >= 0" + std::string(entityMode ? " (the frame's start)." : ".");
        return false;
    }
    cue.time = item["time"].get<float>();
    auto text = [&](const char* key, std::string& out) {
        if (!item.contains(key)) return true;
        if (!item[key].is_string()) {
            error = where + "." + key + " must be a string.";
            return false;
        }
        out = item[key].get<std::string>();
        return true;
    };
    auto flag = [&](const char* key, bool& out) {
        if (!item.contains(key)) return true;
        if (!item[key].is_boolean()) {
            error = where + "." + key + " must be true or false.";
            return false;
        }
        out = item[key].get<bool>();
        return true;
    };
    auto ranged = [&](const char* key, float low, float high, float& out) {
        if (!item.contains(key)) return true;
        if (!item[key].is_number() || item[key].get<double>() < low || item[key].get<double>() > high) {
            error = where + "." + key + " must be a number from " + fmt(low) + " to " + fmt(high) + ".";
            return false;
        }
        out = item[key].get<float>();
        return true;
    };
    if (item.contains("source") && entityMode) {
        error = where + ".source: an entity Animation plays sounds as audio actions: give sound (a clip or .soundevent) and path (its target).";
        return false;
    }
    if (entityMode && (item.contains("stop") || item.contains("follow_speed"))) {
        error = where + ": an audio action lives in its frame (stop and follow_speed are clip cue keys): set duration to cut it.";
        return false;
    }
    if (item.contains("sound")) {
        cue.kind = ClipCueKind::Sound;
        if (!resolveSoundRef(project, item["sound"], where + ".sound", cue.sound, error)) return false;
    } else if (item.contains("source")) {
        cue.kind = ClipCueKind::Source;
        if (!text("source", cue.path)) return false;
    } else {
        cue.kind = ClipCueKind::Snapshot;
        if (!text("snapshot", cue.snapshot)) return false;
        const std::vector<std::string> names = AudioMixer::getSnapshotNames();
        if (std::find(names.begin(), names.end(), cue.snapshot) == names.end()) {
            std::string list;
            for (const std::string& name : names) list += (list.empty() ? "" : ", ") + name;
            error = where + ".snapshot \"" + cue.snapshot + "\" is not in the project's mixer (snapshots: " + (list.empty() ? "none" : list) +
                    "): use one of them or add it to the mixer.";
            return false;
        }
    }
    if (cue.kind != ClipCueKind::Source && !text("path", cue.path)) return false;
    if (!text("group", cue.group) || !text("name", cue.name) || !flag("spatial", cue.spatial) || !flag("follow_speed", cue.followSpeed)) return false;
    if (!ranged("volume_db", anim::CUE_MIN_VOLUME_DB, anim::CUE_MAX_VOLUME_DB, cue.volumeDb) ||
        !ranged("pitch_semitones", -anim::CUE_MAX_PITCH_SEMITONES, anim::CUE_MAX_PITCH_SEMITONES, cue.pitchSemitones) ||
        !ranged("seconds", 0.0f, 600.0f, cue.seconds)) return false;
    if (item.contains("unscaled")) {
        bool unscaled = false;
        if (!flag("unscaled", unscaled)) return false;
        cue.unscaled = unscaled;
    }
    std::string stop;
    if (!text("stop", stop)) return false;
    if (!stop.empty() && !anim::cueStopFromName(stop, cue.stop)) {
        error = where + ".stop must be none (plays to its end) or state_exit (stops when its state ends).";
        return false;
    }
    if (cue.kind == ClipCueKind::Sound && (item.contains("seconds") || item.contains("unscaled"))) {
        error = where + ": seconds and unscaled belong to snapshot cues.";
        return false;
    }
    if (cue.kind != ClipCueKind::Sound && (item.contains("volume_db") || item.contains("pitch_semitones") || item.contains("group") ||
                                           item.contains("spatial"))) {
        error = where + ": volume_db, pitch_semitones, group and spatial belong to sound cues" +
                std::string(cue.kind == ClipCueKind::Source ? " (a source cue plays with its AudioSource's own settings)." : ".");
        return false;
    }
    if (!entityMode) {
        if (cue.kind == ClipCueKind::Sound && !cue.spatial && item.contains("path")) {
            error = where + ".path: a 2D sound cue follows nothing: add spatial: true to play it at that GameObject.";
            return false;
        }
        const std::string problem = anim::validateCue(cue);
        if (!problem.empty()) {
            error = where + ": " + problem + " (paths are relative to the root, e.g. \"Body/LeftFoot\"; \"\" is the root).";
            return false;
        }
    }
    return true;
}

Json cueJson(Project* project, const ClipAudioCue& cue, size_t index) {
    Json item = {{"index", index}, {"time", r6(cue.time)}, {"kind", anim::cueKindName(cue.kind)}};
    switch (cue.kind) {
        case ClipCueKind::Sound: {
            Json sound = {{"path", projectPathOf(project, cue.sound.path)}};
            if (!cue.sound.guid.isNull()) sound["guid"] = cue.sound.guid.str();
            item["sound"] = sound;
            item["spatial"] = cue.spatial;
            if (cue.spatial) item["path"] = cue.path;
            item["volume_db"] = r6(cue.volumeDb);
            item["pitch_semitones"] = r6(cue.pitchSemitones);
            if (!cue.group.empty()) item["group"] = cue.group;
            const double length = soundSeconds(cue.sound);
            item["length"] = length > 0.0 ? Json(r6(length)) : Json(nullptr);
            break;
        }
        case ClipCueKind::Source:
            item["source"] = cue.path;
            item["length"] = nullptr;
            break;
        case ClipCueKind::Snapshot:
            item["snapshot"] = cue.snapshot;
            item["seconds"] = r6(cue.seconds);
            if (cue.unscaled) item["unscaled"] = *cue.unscaled;
            item["length"] = r6(cue.seconds);
            break;
    }
    if (cue.kind != ClipCueKind::Snapshot) {
        item["stop"] = anim::cueStopName(cue.stop);
        if (cue.followSpeed) item["follow_speed"] = true;
    }
    if (!cue.name.empty()) item["name"] = cue.name;
    return item;
}

Json cuesJson(Project* project, const std::vector<ClipAudioCue>& cues) {
    Json out = Json::array();
    for (size_t i = 0; i < cues.size(); i++) out.push_back(cueJson(project, cues[i], i));
    return out;
}

Json warningsJson(const std::vector<ClipWarning>& warnings) {
    Json out = Json::array();
    for (const ClipWarning& warning : warnings) out.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    return out;
}

std::string projectPathOf(Project* project, const std::string& assetRelative) {
    if (assetRelative.empty()) return assetRelative;
    return projectRel(project, project->resolveAssetPath(FileUtils::pathFromUtf8(assetRelative)).lexically_normal());
}

Json clipJson(Project* project, const std::string& clip, const ClipData& data, bool readOnly, size_t maxKeys) {
    Json tracks = Json::array();
    for (size_t i = 0; i < data.tracks.size(); i++) tracks.push_back(trackJson(data.tracks[i], i, maxKeys));
    Json out = {{"clip", clip}, {"read_only", readOnly}, {"length", r6(data.effectiveLength())},
                {"explicit_length", data.hasExplicitLength()}, {"wrap", anim::wrapName(data.wrap)},
                {"frame_rate", r6(data.frameRate)}, {"tracks", tracks}, {"events", eventsJson(data.events)}};
    if (!data.audio.empty()) out["audio"] = cuesJson(project, data.audio);
    if (!data.previewModel.empty()) out["preview_model"] = projectPathOf(project, data.previewModel);
    if (!data.source.empty()) out["source"] = {{"kind", data.source.kind}, {"path", data.source.path}, {"name", data.source.name}};
    return out;
}

// ------------------------------------------------------------------ users and bindings

// the first 50 users (results add users_total when there are more)
Json usersJson(Project* project, const std::vector<ClipUser>& users) {
    Json out = Json::array();
    for (const ClipUser& user : users) {
        Json item = {{"entry", user.entry}};
        SceneProject* scene = user.sceneId ? project->getScene(user.sceneId) : nullptr;
        if (user.prefabAsset) item["prefab"] = user.prefabPath;
        if (scene && scene->scene && scene->scene->isEntityCreated(user.entity)) {
            item["scene"] = scene->id;
            item["entity"] = McpRefs::entityJson(scene, user.entity);
            if (const AnimationPlayerComponent* player = scene->scene->findComponent<AnimationPlayerComponent>(user.entity)) {
                const std::vector<std::string> names = anim::playerStateNames(player->clips);
                if (user.entry < names.size()) item["state"] = names[user.entry];
            }
        }
        out.push_back(item);
        if (out.size() >= 50) break;
    }
    return out;
}

const char* statusName(TrackBindingStatus status) {
    switch (status) {
        case TrackBindingStatus::Ok: return "ok";
        case TrackBindingStatus::MissingPath: return "missing_path";
        case TrackBindingStatus::DuplicateSiblingName: return "duplicate_sibling_name";
        case TrackBindingStatus::MissingComponent: return "missing_component";
        case TrackBindingStatus::LatticeIndexOutOfRange: return "lattice_index_out_of_range";
        case TrackBindingStatus::MorphIndexOutOfRange: return "morph_index_out_of_range";
        case TrackBindingStatus::LegacyModelLayout: return "legacy_model_layout";
        case TrackBindingStatus::UnaddressableName: return "unaddressable_name";
        case TrackBindingStatus::NotSampled: return "not_sampled";
        case TrackBindingStatus::UnknownProperty: return "unknown_property";
        case TrackBindingStatus::HumanoidNoRig: return "humanoid_no_rig";
        case TrackBindingStatus::Unchecked: return "unchecked";
    }
    return "unchecked";
}

// The write_file edit that renames a track's path in a canonical .anim (the editor writes one
// track per `{"path": ..., "channel": ...` line): every key and tangent kept
Json renameTrackEdit(const std::string& path, ClipChannel channel, const std::string& to) {
    const std::string channelText = std::string(", \"channel\": \"") + anim::channelName(channel) + "\"";
    return {{"old_text", "{\"path\": " + Json(path).dump() + channelText}, {"new_text", "{\"path\": " + Json(to).dump() + channelText}};
}

// The .anim file a binding report may offer rename fixes for: its data and its file
struct FixableClip {
    const ClipData* data = nullptr;
    fs::path file;
};

// The angle (degrees) between the clip's first rotation key of `path` and the rest rotation of the
// GameObject `candidate` names under `root`; negative when the clip has no rotation track there or
// the candidate is not found. Rotation keys are absolute local rotations: a bone of another rig
// (another rest orientation) far from them is mangled by the renamed track
float restAngle(EntityRegistry* registry, Entity root, const ClipData& data, const std::string& path, const std::string& candidate) {
    if (!registry || root == NULL_ENTITY) return -1.0f;
    const int track = data.findTrack(path, ClipChannel::Rotation);
    if (track < 0 || data.tracks[(size_t)track].keys.empty() || data.tracks[(size_t)track].keys[0].value.size() < 4) return -1.0f;
    const Entity entity = anim::resolvePath(registry, root, candidate);
    const Transform* transform = entity != NULL_ENTITY ? registry->findComponent<Transform>(entity) : nullptr;
    if (!transform) return -1.0f;
    const std::vector<float>& q = data.tracks[(size_t)track].keys[0].value;
    const Quaternion& rest = transform->rotation;
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 0.0f)) return -1.0f;
    const float dot = std::fabs((q[0] * rest.x + q[1] * rest.y + q[2] * rest.z + q[3] * rest.w) / length);
    return 2.0f * std::acos(std::min(1.0f, dot)) * 57.2957795f;
}

// a renamed path whose rest is further than this from the clip's rotation is not offered as a fix
constexpr float REST_MISMATCH_DEGREES = 90.0f;

// {root, resolved, total, missing: [{path, channel, status, nearest, fix?, fix_path?, fix_conflict?,
// rest_mismatch?, message}], ...}. With `clip` (an .anim file), a missing path gets `fix`, the
// write_file edit that renames it to `fix_path`: the first nearest path whose (path, channel) is
// neither a track of the clip nor another fix's target, so every fix can be applied together without
// two tracks merging; none free: `fix_conflict`. A nearest path is no fix either when its bone name
// says the other side (Left/Right, .L/.R), or when the clip's first rotation key of the path is more
// than 90 degrees from that GameObject's rest rotation (`rest_mismatch`: another rig, whose clip is
// baked on it, not renamed; no less near path is tried then; agents review I1). No fix when the file is not in the canonical layout
// (the edit's old_text is not in it).
Json bindingsJson(SceneProject* scene, const ClipValidationReport& report, const FixableClip* clip = nullptr) {
    Json missing = Json::array();
    Json ambiguous = Json::array();
    std::string fileText;
    bool fileRead = false;
    std::set<std::pair<std::string, ClipChannel>> taken;
    if (clip && clip->data) {
        std::ifstream in(clip->file, std::ios::binary);
        fileRead = in.good();
        if (fileRead) fileText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        fileRead = fileRead && !in.bad() && !fileText.empty();
        for (const ClipTrackData& track : clip->data->tracks) taken.insert({track.path, track.channel});
    }
    bool notCanonical = false;
    bool restMismatch = false;
    EntityRegistry* registry = scene ? scene->scene : nullptr;
    for (const TrackReport& track : report.tracks) {
        if (track.status == TrackBindingStatus::Ok || track.status == TrackBindingStatus::NotSampled ||
            track.status == TrackBindingStatus::Unchecked) continue;
        if (track.status == TrackBindingStatus::DuplicateSiblingName) {
            // bound (to the first match), but the path should say which sibling: "Name#N"
            Json item = {{"path", track.path}, {"channel", anim::channelName(track.channel)}};
            if (!track.message.empty()) item["message"] = track.message;
            ambiguous.push_back(item);
            continue;
        }
        Json item = {{"path", track.path}, {"channel", anim::channelName(track.channel)}, {"status", statusName(track.status)},
                     {"nearest", track.nearest}};
        if (track.channel == ClipChannel::Property) item["property"] = track.property;
        if (isHumanoidChannel(track.channel)) {
            item.erase("path");
            item["bone"] = anim::humanoidTrackBoneName(track.channel, track.humanBone);
        }
        if (!track.badPoints.empty()) item["points"] = track.badPoints;
        if (!track.message.empty()) item["message"] = track.message;
        // (property tracks share their path line prefix: no write_file rename for them)
        if (clip && clip->data && track.status == TrackBindingStatus::MissingPath && !track.nearest.empty() &&
            track.channel != ClipChannel::Property) {
            const Json probe = renameTrackEdit(track.path, track.channel, track.path);
            if (fileText.find(probe["old_text"].get<std::string>()) == std::string::npos) {
                notCanonical = true;
            } else {
                const char side = ClipValidation::boneSide(track.path);
                bool otherSide = false;
                std::string restReason;
                // a rest far from the clip's rotation means another rig: no further (less near) path
                // is tried, it would be a wrong bone that happens to rest like this one
                auto free = std::find_if(track.nearest.begin(), track.nearest.end(), [&](const std::string& candidate) {
                    if (!restReason.empty() || taken.count({candidate, track.channel})) return false;
                    const char candidateSide = ClipValidation::boneSide(candidate);
                    if (side && candidateSide && side != candidateSide) {
                        otherSide = true;
                        return false;
                    }
                    const float angle = restAngle(registry, report.root, *clip->data, track.path, candidate);
                    if (angle > REST_MISMATCH_DEGREES) {
                        if (restReason.empty()) {
                            restReason = "the clip's first rotation of " + track.path + " is " + std::to_string((int)std::lround(angle)) +
                                         " degrees from the rest of " + candidate + ": the rest pose differs a lot (likely "
                                         "another rig, which a renamed path would turn wrong). Play it retargeted (the entry's "
                                         "retarget: true, or a Convert to a humanoid clip), or rename the path by hand if it is "
                                         "the bone you mean";
                        }
                        return false;
                    }
                    return true;
                });
                if (free != track.nearest.end()) {
                    taken.insert({*free, track.channel});
                    item["fix"] = renameTrackEdit(track.path, track.channel, *free);
                    item["fix_path"] = *free;
                } else if (!restReason.empty()) {
                    item["rest_mismatch"] = restReason;
                    restMismatch = true;
                } else if (otherSide) {
                    item["fix_conflict"] = "the nearest paths free for a fix name the other side (Left/Right, .L/.R): rename by hand";
                } else {
                    item["fix_conflict"] = "every nearest path already has a " + std::string(anim::channelName(track.channel)) +
                                           " track in the clip or is another fix's target: rename by hand";
                }
            }
        }
        missing.push_back(item);
    }
    Json handlers = Json::array();
    for (const EventReport& event : report.events) {
        if (event.status == EventHandlerStatus::NotFound) handlers.push_back(event.function);
    }
    const bool unchecked = std::all_of(report.tracks.begin(), report.tracks.end(), [](const TrackReport& t) {
        return t.status == TrackBindingStatus::Unchecked;
    }) && !report.tracks.empty();
    Json out = {{"root", scene ? McpRefs::entityJson(scene, report.root) : Json(nullptr)}, {"resolved", report.resolved},
                {"total", report.total}, {"missing", missing}, {"missing_handlers", handlers}};
    if (!ambiguous.empty()) out["ambiguous"] = ambiguous;
    if (report.legacyModelLayout || !report.legacyModels.empty()) {
        // models loaded without their node hierarchy: adding (or keeping) an Animation Player on
        // the root reloads them in it (ClipSceneActions::reloadModelsForPlayer, the Inspector's fix)
        Json models = Json::array();
        for (Entity model : report.legacyModels) models.push_back(scene ? McpRefs::entityJson(scene, model) : Json(model));
        out["legacy_models"] = models;
        out["legacy_fix"] = "add_component {entity: <root>, component: animationplayer} (no player: adds one; with one: reloads its "
                            "models in their node hierarchy, settings kept), one scene step; then the paths bind";
    }
    if (unchecked) out["unchecked"] = true;
    if (!report.warnings.empty()) out["warnings"] = warningsJson(report.warnings);
    if (restMismatch) {
        // agents review I1: renaming fits renamed or reparented objects of the same rig
        out["rig_mismatch"] = "rest_mismatch paths: the clip's rotations are far from these bones' rest (rotation keys are "
                              "absolute local rotations). Likely a clip made for another rig, whose renamed paths would turn "
                              "this character wrong: retarget it (set_properties animationplayer.clips[i].retarget: true, or "
                              "convert it: edit_animation {clip: <new .anim>, create: {from: <clip>, humanoid: {}}}), or use a "
                              "clip made on this rig (bake the .poseanim on this model: edit_pose_animation {path, bake: "
                              "{model, clip: true}}, or key a new clip on it). A path of the same rig (a bone or prop posed far from its rest) can still "
                              "be renamed by hand";
    }
    if (std::any_of(missing.begin(), missing.end(), [](const Json& m) { return m.contains("fix"); })) {
        out["fix_with"] = "write_file {path: <clip>, edits: [missing[i].fix, ...]} renames those paths to their fix_path, keys and tangents "
                          "kept; the fix targets differ from each other and from the clip's tracks, so they apply together (fixes are for "
                          "renamed or reparented objects of the same rig: check each fix_path is the bone you mean; every user of the "
                          "clip changes: copy it first with move_asset {copy: true})";
    } else if (notCanonical && !fileRead) {
        out["fix_with"] = "no fix: could not read the clip file; rename the paths with edit_animation (the old track's keys under the new "
                          "path, keys: [] under the old one)";
    } else if (notCanonical) {
        out["fix_with"] = "no fix: the file is not in the editor's canonical layout (written by hand); rename the paths with edit_animation "
                          "(the old track's keys under the new path, keys: [] under the old one), which also rewrites the file canonically";
    }
    return out;
}

std::string bindingsText(const Json& bindings) {
    std::string text = "bindings on \"" + (bindings["root"].is_object() ? bindings["root"].value("name", std::string()) : std::string()) +
                       "\": " + std::to_string(bindings["resolved"].get<size_t>()) + "/" + std::to_string(bindings["total"].get<size_t>()) +
                       " track(s) bound";
    if (bindings.contains("unchecked")) return text + " (not checked)";
    for (const Json& m : bindings["missing"]) {
        text += "\n  " + (m.contains("bone") ? "humanoid " + m["bone"].get<std::string>() : m["path"].get<std::string>()) + " : " +
                m["channel"].get<std::string>() + " " + m["status"].get<std::string>();
        if (!m["nearest"].empty()) text += " (did you mean \"" + m["nearest"][0].get<std::string>() + "\"?" +
                                          (m.contains("fix") ? " fix: write_file edit missing[].fix -> \"" + m["fix_path"].get<std::string>() + "\""
                                           : m.contains("rest_mismatch") ? std::string(" no fix: rest pose differs a lot")
                                           : m.contains("fix_conflict") ? std::string(m["fix_conflict"].get<std::string>().find("other side") != std::string::npos
                                                                                  ? " no fix: the other side" : " no fix: its targets are taken")
                                           : std::string()) + ")";
    }
    for (const Json& f : bindings["missing_handlers"]) text += "\n  event " + f.get<std::string>() + ": no script method or onEvent";
    if (bindings.contains("ambiguous")) {
        for (const Json& a : bindings["ambiguous"]) {
            text += "\n  " + a["path"].get<std::string>() + " : " + a["channel"].get<std::string>() +
                    " bound to the first of several same-named siblings (write Name#N)";
        }
    }
    if (bindings.contains("legacy_models")) {
        text += "\n  " + std::to_string(bindings["legacy_models"].size()) + " model(s) without their node hierarchy: " +
                bindings["legacy_fix"].get<std::string>();
    }
    if (bindings.contains("rig_mismatch")) text += "\n  " + bindings["rig_mismatch"].get<std::string>();
    return text;
}

// The binding report against `root`, else against the first user of the clip in an open scene
Json bindingsFor(Project* project, SceneProject* rootScene, Entity root, const ClipData& data, const std::vector<ClipUser>& users,
                 const fs::path& file) {
    SceneProject* scene = rootScene;
    Entity target = root;
    if (target == NULL_ENTITY) {
        for (const ClipUser& user : users) {
            SceneProject* candidate = user.sceneId ? project->getScene(user.sceneId) : nullptr;
            if (candidate && candidate->scene && candidate->scene->isEntityCreated(user.entity)) {
                scene = candidate;
                target = user.entity;
                break;
            }
        }
    }
    if (!scene || !scene->scene || target == NULL_ENTITY) return nullptr;
    FixableClip clip{&data, file};
    return bindingsJson(scene, ClipValidation::validateInScene(project, scene, data, target), &clip);
}

// ------------------------------------------------------------------ rejections (9.R29)

struct Rejection {
    const char* arg;
    const char* message;
};

const std::vector<Rejection>& fileModeRejections() {
    static const std::vector<Rejection> table = {
        {"loop", "loop: a clip's playback is wrap (once, loop, ping_pong, clamp_forever)"},
        {"duration", "duration: a clip's playback length is length (seconds; -1 = the last key or event)"},
        {"tweens", "tweens are scene actions: use edit_animation without clip (an entity Animation), or key the values as tracks"},
        {"add_actions", "add_actions nests scene actions (sprite animations, particles, animations): use edit_animation without clip"},
        {"set_frames", "set_frames edits an entity Animation's timeline: a clip's tracks have no frames (shift the keys' times)"},
        {"remove_actions", "remove_actions edits an entity Animation: in a clip, remove a track with tracks [{path, channel, keys: []}]"},
        {"delete_removed", "delete_removed belongs to remove_actions (entity Animations)"},
        {"target", "target: a clip binds by path relative to the GameObject that plays it: give tracks[].path (and root to check them)"},
        {"animation", "animation names an entity Animation: pass clip or animation, not both"},
        {"autoplay", "autoplay: an Animation Player's playAutomatically (set_properties) starts a clip"},
        {"speed", "speed: an Animation Player's speed (set_properties) or the state's speed in Lua"},
        {"name", "name: a clip's state name is set on the Animation Player entry (clips[i].name)"}};
    return table;
}

const std::vector<Rejection>& sceneModeRejections() {
    static const std::vector<Rejection> table = {
        {"wrap", "wrap"}, {"length", "length"}, {"frame_rate", "frame_rate"}, {"events", "events"},
        {"events_mode", "events_mode"}, {"root", "root"}, {"create", "create"}};
    return table;
}

// ------------------------------------------------------------------ editing

struct EditContext {
    Project* project = nullptr;
    SceneProject* scene = nullptr;     // the root's
    Entity root = NULL_ENTITY;
    std::vector<std::string> changes;
};

bool readNumber(const Json& value, const std::string& where, float& out, std::string& error) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        error = where + " must be a number.";
        return false;
    }
    out = value.get<float>();
    return true;
}

bool applySettings(EditContext& ctx, const Json& args, ClipData& data, std::string& error) {
    if (has(args, "wrap")) {
        ClipWrapMode wrap = ClipWrapMode::Once;
        if (!args["wrap"].is_string() || !anim::wrapFromName(lower(args["wrap"].get<std::string>()), wrap) || wrap == ClipWrapMode::Default) {
            error = "wrap " + args["wrap"].dump() + " is not valid: use " + wrapNames() + ".";
            return false;
        }
        ctx.changes.push_back(wrap != data.wrap ? std::string("wrap ") + anim::wrapName(data.wrap) + " -> " + anim::wrapName(wrap)
                                                : std::string("wrap ") + anim::wrapName(wrap) + " (unchanged)");
        data.wrap = wrap;
    }
    if (has(args, "frame_rate")) {
        float rate = 0.0f;
        if (!readNumber(args["frame_rate"], "frame_rate", rate, error)) return false;
        if (rate < anim::ANIM_MIN_FRAME_RATE || rate > anim::ANIM_MAX_FRAME_RATE) {
            error = "frame_rate must be 1 to 240 samples per second (keys stay in seconds; it is the snapping grid).";
            return false;
        }
        ctx.changes.push_back(rate != data.frameRate ? "frame_rate " + fmt(data.frameRate) + " -> " + fmt(rate)
                                                     : "frame_rate " + fmt(rate) + " (unchanged)");
        data.frameRate = rate;
    }
    if (has(args, "length")) {
        float length = 0.0f;
        if (!readNumber(args["length"], "length", length, error)) return false;
        if (length == 0.0f) {
            error = "length must be > 0 seconds, or -1 for automatic (the last key or event).";
            return false;
        }
        if (length < 0.0f) length = -1.0f;
        const std::string what = length < 0.0f ? std::string("length automatic (last key or event)") : "length " + fmt(length) + " s";
        ctx.changes.push_back(length != data.length ? what : what + " (unchanged)");
        data.length = length;
    }
    return true;
}

// A value of `key` in ClipKey::value layout
std::vector<float> keyValue(ClipChannel channel, const Key& key) {
    switch (channel) {
        case ClipChannel::Position:
        case ClipChannel::Scale:
            return {key.vec.x, key.vec.y, key.vec.z};
        case ClipChannel::Rotation: {
            float xyzw[4];
            quaternionToXYZW(key.quat, xyzw);
            return {xyzw[0], xyzw[1], xyzw[2], xyzw[3]};
        }
        case ClipChannel::Morph:
            return key.weights;
        default:
            return {};
    }
}

// Lattice keys without a cage to read (no root): flat indices and "rest" only
bool checkCagelessLatticeKeys(const Json& keys, const std::string& where, std::string& error) {
    for (size_t i = 0; i < keys.size(); i++) {
        const Json& item = keys[i];
        if (!item.is_object()) continue;
        const char* field = has(item, "points") ? "points" : "value";
        if (!has(item, field)) continue;
        const Json& v = item[field];
        const std::string at = where + ".keys[" + std::to_string(i) + "]." + field;
        if (v.is_string() && lower(v.get<std::string>()) != "rest") {
            error = at + ": " + v.dump() + " reads the cage (\"current\" or a pose): pass root (the GameObject that plays the clip), or give offsets {\"<index>\": [x,y,z]}.";
            return false;
        }
        if (v.is_array()) {
            for (const Json& pair : v) {
                if (pair.is_array() && !pair.empty() && !pair[0].is_number_integer()) {
                    error = at + ": grid coordinates [i,j,k] need the cage's resolution: pass root, or use flat indices (i + rx*(j + ry*k)).";
                    return false;
                }
            }
        }
    }
    return true;
}

LatticeComponent cagelessLattice() {
    LatticeComponent lattice;
    lattice.resolutionX = 16;
    lattice.resolutionY = 16;
    lattice.resolutionZ = 16;
    return lattice;
}

// The track's existing keys as edit keys (merge)
std::vector<Key> latticeKeysOf(const ClipTrackData& track) {
    std::vector<Key> keys;
    for (const ClipKey& clipKey : track.keys) {
        Key key;
        key.time = clipKey.time;
        key.ease = clipKey.ease;
        for (size_t n = 0; n < track.points.size() && n * 3 + 2 < clipKey.value.size(); n++) {
            key.points.push_back(track.points[n]);
            key.offsets.push_back(Vector3(clipKey.value[n * 3], clipKey.value[n * 3 + 1], clipKey.value[n * 3 + 2]));
        }
        keys.push_back(key);
    }
    return keys;
}

// The Hermite tangents keys carry ({in, out}, as results list them for imported model clips), by
// key time; `quat` the stored quaternion a rotation key read from a result names (the tangents'
// hemisphere). False with `error` for a malformed pair or a lattice key with tangents.
struct GivenTangents {
    float time = 0.0f;
    std::vector<float> in, out;
    std::vector<float> quat;
    bool used = false;
};

bool readTangentArray(const Json& value, size_t width, std::vector<float>& out) {
    if (!value.is_array() || value.size() != width) return false;
    out.clear();
    for (const Json& number : value) {
        if (!number.is_number() || !std::isfinite(number.get<double>())) return false;
        out.push_back(number.get<float>());
    }
    return true;
}

bool parseKeyTangents(ClipChannel channel, const Json& keys, const std::string& where, std::vector<GivenTangents>& out,
                      std::string& error) {
    if (!keys.is_array()) return true;
    for (size_t i = 0; i < keys.size(); i++) {
        const Json& item = keys[i];
        if (!item.is_object() || (!has(item, "in") && !has(item, "out"))) continue;
        const std::string at = where + ".keys[" + std::to_string(i) + "]";
        if (channel == ClipChannel::Lattice) {
            error = at + ": lattice keys have no in/out tangents.";
            return false;
        }
        if (!has(item, "in") || !has(item, "out")) {
            error = at + ": in and out go together (Hermite tangents in units per second, as results list them).";
            return false;
        }
        size_t width = channel == ClipChannel::Rotation ? 4 : 3;
        if (channel == ClipChannel::Morph) width = has(item, "value") && item["value"].is_array() ? item["value"].size() : 0;
        GivenTangents given;
        given.time = has(item, "time") && item["time"].is_number() ? item["time"].get<float>() : 0.0f;
        if (width == 0 || !readTangentArray(item["in"], width, given.in) || !readTangentArray(item["out"], width, given.out)) {
            error = at + ": in and out must be arrays of " + std::to_string(width) + " numbers (" +
                    (channel == ClipChannel::Rotation ? "quaternion x, y, z, w" : channel == ClipChannel::Morph ? "one per weight" : "x, y, z") +
                    " per second).";
            return false;
        }
        if (channel == ClipChannel::Rotation && has(item, "value") && item["value"].is_object() && item["value"].contains("quat")) {
            readTangentArray(item["value"]["quat"], 4, given.quat);
        }
        out.push_back(given);
    }
    return true;
}

// a key value equal to another one (a key sent back unchanged: the r6 of results, euler round trips)
bool sameKeyValue(ClipChannel channel, const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    const float tolerance = channel == ClipChannel::Rotation ? 1e-4f : 1e-5f;
    auto close = [&](float sign) {
        for (size_t i = 0; i < a.size(); i++) {
            if (std::abs(a[i] - sign * b[i]) > tolerance * std::max(1.0f, std::abs(b[i]))) return false;
        }
        return true;
    };
    return close(1.0f) || (channel == ClipChannel::Rotation && close(-1.0f));
}

// A property track (anim-properties spec 11): {path, channel: "property", component, property,
// keys, mode, loop}. component: the Catalog key or the Inspector's name ("Image" -> ui); keys
// [{time, value, ease, in, out, tangent_mode}] (or [time, value, ease]) in key space, read by the
// .anim parser itself (bools, ints, "#RRGGBB[AA]" colours, the step rule), its warnings in changes
bool applyPropertyTrack(EditContext& ctx, const Json& item, const std::string& where, const std::string& path, ClipData& data,
                        std::string& error) {
    if (!has(item, "component") || !item["component"].is_string() || !has(item, "property") || !item["property"].is_string()) {
        error = where + ": a property track needs component (\"ui\", \"light\", \"gameobject\", \"script:<Class>\"...) and property "
                "(\"color\", \"color.a\", \"intensity\", \"active\"...); get_animation {root, list_properties: \"<path>\"} lists them.";
        return false;
    }
    if (has(item, "relative") && item["relative"].is_boolean() && item["relative"].get<bool>()) {
        error = where + ".relative: only position, rotation and scale tracks can be relative.";
        return false;
    }
    std::string component, property, nameError;
    if (!ClipPropertyNames::resolve(item["component"].get<std::string>(), item["property"].get<std::string>(), component, property, nameError)) {
        error = where + ": " + nameError + ".";
        return false;
    }
    const std::string mode = item.contains("mode") && item["mode"].is_string() ? lower(item["mode"].get<std::string>()) : "replace";
    if (mode != "replace" && mode != "merge") {
        error = where + ".mode must be replace or merge.";
        return false;
    }
    const bool merge = mode == "merge";
    const std::string label = path + " : " + component + "." + property;
    if (component != item["component"].get<std::string>() || property != item["property"].get<std::string>()) {
        ctx.changes.push_back(where + ": " + item["component"].get<std::string>() + "." + item["property"].get<std::string>() + " is " +
                              component + "." + property);
    }
    int index = data.findPropertyTrack(path, component, property);
    const Json keysJson = has(item, "keys") ? item["keys"] : Json::array();
    if (!keysJson.is_array()) {
        error = where + ".keys must be an array of {time, value} (value in the Inspector's units: sRGB colours, cone degrees, true/false).";
        return false;
    }
    if (keysJson.empty()) {
        if (merge) {
            error = where + ".keys is empty: merge adds keys; to remove the track use mode replace with keys [].";
            return false;
        }
        if (index < 0) {
            error = where + ": the clip has no " + label + " track to remove (keys: [] removes one).";
            return false;
        }
        data.tracks.erase(data.tracks.begin() + index);
        ctx.changes.push_back("removed track " + label);
        return true;
    }
    // the keys through the clip parser (one track document): the file's own rules
    Json document = {{"format", anim::ANIM_FORMAT}, {"version", anim::ANIM_VERSION}, {"tracks", Json::array()}};
    Json trackDoc = {{"path", path}, {"channel", "property"}, {"component", component}, {"property", property}, {"keys", keysJson}};
    if (has(item, "loop")) trackDoc["loop"] = item["loop"];
    document["tracks"].push_back(trackDoc);
    anim::ClipParseResult parsed = anim::parseClip(document.dump());
    if (!parsed.ok() || parsed.data->tracks.empty()) {
        error = where + ": " + (parsed.ok() ? std::string("no keys could be read") : parsed.error.toString()) + ".";
        return false;
    }
    ClipTrackData track = parsed.data->tracks.front();
    for (const ClipWarning& warning : parsed.warnings) {
        std::string at = warning.path;
        if (at.compare(0, 9, "tracks[0]") == 0) at = where + at.substr(9);
        ctx.changes.push_back("warning: " + (at.empty() ? where : at) + ": " + warning.message);
    }
    if (track.keys.empty()) {
        error = where + ".keys: no key could be read (values in the Inspector's units: true/false, numbers, [r, g, b, a] sRGB or \"#RRGGBB\").";
        return false;
    }
    // a script field's declared type (Int, Color) when the root binds it
    if (ctx.root != NULL_ENTITY && ctx.scene && track.swizzle < 0) {
        const Entity entity = anim::resolvePath(ctx.scene->scene, ctx.root, path);
        anim::PropertyRef ref;
        if (entity != NULL_ENTITY && anim::parsePropertyRef(component, property, ref) && ref.script) {
            const anim::PropertyBinding binding = anim::bindProperty(ctx.scene->scene, entity, ref, track.width);
            if (binding.status == anim::PropertyBindStatus::Bound && binding.width == track.width) track.kind = binding.kind;
        }
    }
    ClipTrackData* existing = index >= 0 ? &data.tracks[(size_t)index] : nullptr;
    if (existing && !has(item, "loop")) track.loop = existing->loop;
    if (merge && existing) {
        std::vector<ClipKey> keys = existing->keys;
        for (const ClipKey& key : track.keys) {
            auto same = std::find_if(keys.begin(), keys.end(), [&](const ClipKey& k) { return std::abs(k.time - key.time) < MERGE_TIME; });
            if (same != keys.end()) *same = key;
            else keys.push_back(key);
        }
        std::stable_sort(keys.begin(), keys.end(), [](const ClipKey& a, const ClipKey& b) { return a.time < b.time; });
        track.keys = keys;
    }
    if (existing) {
        ctx.changes.push_back(std::string(merge ? "merged " : "replaced ") + std::to_string(keysJson.size()) + " key(s) into " + label +
                              " (" + std::to_string(track.keys.size()) + " now)");
        *existing = track;
    } else {
        ctx.changes.push_back("added track " + label + " (" + std::to_string(track.keys.size()) + " key(s), " + anim::kindName(track.kind) +
                              (track.discrete ? ", stepped" : "") + ")");
        data.tracks.push_back(track);
    }
    return true;
}

// the humanoid track bone names nearest `name` (did-you-mean)
std::string nearestHumanoidBone(ClipChannel channel, const std::string& name) {
    auto distance = [](const std::string& a, const std::string& b) {
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) row[j] = j;
        for (size_t i = 1; i <= a.size(); i++) {
            size_t previous = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++) {
                const size_t current = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, previous + (a[i - 1] == b[j - 1] ? 0 : 1)});
                previous = current;
            }
        }
        return row[b.size()];
    };
    std::string best;
    size_t bestDistance = std::max<size_t>(3, name.size() / 2) + 1;
    const int count = channel == ClipChannel::Humanoid ? anim::HUMANOID_TRACK_BONE_COUNT : HUMANOID_POSITION_COUNT;
    for (int b = 0; b < count; b++) {
        const std::string candidate = anim::humanoidTrackBoneName(channel, b);
        const size_t d = distance(lower(name), candidate);
        if (d < bestDistance) {
            bestDistance = d;
            best = candidate;
        }
    }
    return best;
}

// A humanoid key value as the .anim parser takes it: [x, y, z, w], {quat}, {angles: [a, b, c]},
// {angles: {<dof>: degrees}} (the slot's DOF names, as get_animation lists them) or the output
// form {quat, angles} sent back (the two must agree). Errors name the slot's DOF order.
bool humanoidKeyValue(ClipChannel channel, int humanBone, const Json& given, const std::string& at, Json& out, std::string& error) {
    out = given;
    if (channel != ClipChannel::Humanoid || !given.is_object()) return true;
    const bool slot = humanBone >= 0 && humanBone < pose::HUMAN_BONE_COUNT;
    std::string order;
    if (slot) {
        const pose::BoneDofInfo& dofs = pose::boneDofInfo(pose::boneAt(humanBone));
        for (int k = 0; k < pose::DOFS_PER_BONE; k++) order += std::string(k ? ", " : "") + dofs.dofs[k].name;
    }
    const std::string bone = anim::humanoidTrackBoneName(channel, humanBone);
    for (auto it = given.begin(); it != given.end(); ++it) {
        if (it.key() != "quat" && it.key() != "angles") {
            error = at + ": unknown key \"" + it.key() + "\" (a humanoid value is [x, y, z, w], {quat} or {angles: [" + order + "]} degrees).";
            return false;
        }
    }
    Json angles;
    if (given.contains("angles")) {
        if (!slot) {
            error = at + ".angles: " + bone + " is a finger, eye or jaw bone: give {quat: [x, y, z, w]} (angles are for the 22 body slots).";
            return false;
        }
        const pose::BoneDofInfo& dofs = pose::boneDofInfo(pose::boneAt(humanBone));
        const Json& a = given["angles"];
        std::array<double, pose::DOFS_PER_BONE> values{};
        if (a.is_array() && a.size() == (size_t)pose::DOFS_PER_BONE && std::all_of(a.begin(), a.end(), [](const Json& v) { return v.is_number(); })) {
            for (int k = 0; k < pose::DOFS_PER_BONE; k++) values[(size_t)k] = a[(size_t)k].get<double>();
        } else if (a.is_object()) {
            for (auto it = a.begin(); it != a.end(); ++it) {
                int k = 0;
                while (k < pose::DOFS_PER_BONE && it.key() != dofs.dofs[k].name) k++;
                if (k == pose::DOFS_PER_BONE || !it.value().is_number()) {
                    error = at + ".angles." + it.key() + ": " + bone + "'s angles are {" + order + "} (degrees, any of them; the others 0).";
                    return false;
                }
                values[(size_t)k] = it.value().get<double>();
            }
        } else {
            error = at + ".angles: " + bone + "'s angles are [" + order + "] or {" + order + "} (degrees).";
            return false;
        }
        angles = Json::array({values[0], values[1], values[2]});
    }
    if (!angles.is_null() && given.contains("quat")) {
        // the output form sent back: the quaternion stands when both say the same rotation
        const Json& q = given["quat"];
        if (!q.is_array() || q.size() != 4 || !std::all_of(q.begin(), q.end(), [](const Json& v) { return v.is_number(); })) {
            error = at + ".quat must be [x, y, z, w].";
            return false;
        }
        float a[pose::DOFS_PER_BONE] = {angles[0].get<float>(), angles[1].get<float>(), angles[2].get<float>()};
        const pose::Quatd fromAngles = pose::jointRotation(pose::boneAt(humanBone), a).normalized();
        const pose::Quatd fromQuat = pose::Quatd(q[3].get<double>(), q[0].get<double>(), q[1].get<double>(), q[2].get<double>()).normalized();
        if (pose::radToDeg(pose::angleBetween(fromAngles, fromQuat)) > 0.05) {
            error = at + ": quat and angles say different rotations: give one of them (angles [" + order + "] degrees, or quat).";
            return false;
        }
        out = q;
        return true;
    }
    if (!angles.is_null()) {
        out = {{"angles", angles}};
        return true;
    }
    if (given.contains("quat")) out = given["quat"];
    return true;
}

// A humanoid track (anim-retarget 10): {channel: "humanoid", bone, keys} with key values
// {quat} / [x, y, z, w] / {angles: [dof1, dof2, twist]} (slots), or {channel: "humanoid_position",
// bone: hips | left_foot | right_foot, keys} with [left, up, forward]. The keys go through the
// .anim parser (its value rules and messages).
bool applyHumanoidTrack(EditContext& ctx, const Json& item, const std::string& where, ClipChannel channel, ClipData& data, std::string& error) {
    for (auto it = item.begin(); it != item.end(); ++it) {
        if (it.key() == "path" || it.key() == "target") {
            error = where + "." + it.key() + ": humanoid tracks have no path: they name their bone (\"bone\": \"left_upper_arm\").";
            return false;
        }
        static const std::set<std::string> keys = {"bone", "channel", "keys", "mode", "loop"};
        if (!keys.count(it.key())) {
            error = where + ": unknown key \"" + it.key() + "\" (a humanoid track is {channel, bone, keys, mode, loop}).";
            return false;
        }
    }
    if (!has(item, "bone") || !item["bone"].is_string()) {
        error = where + ".bone is required: a humanoid slot (left_upper_arm...), finger, eye or jaw" +
                std::string(channel == ClipChannel::HumanoidPosition ? " (humanoid_position: hips, left_foot or right_foot)." : ".");
        return false;
    }
    const std::string bone = item["bone"].get<std::string>();
    int humanBone = -1;
    if (!anim::humanoidTrackBoneFromName(channel, bone, humanBone)) {
        const std::string suggestion = nearestHumanoidBone(channel, bone);
        error = where + ".bone '" + bone + "' is not a " + (channel == ClipChannel::Humanoid ? "humanoid bone" : "humanoid_position bone (hips, left_foot, right_foot)") +
                (suggestion.empty() ? std::string(".") : " (did you mean '" + suggestion + "'?).");
        return false;
    }
    const std::string mode = item.contains("mode") && item["mode"].is_string() ? lower(item["mode"].get<std::string>()) : "replace";
    if (mode != "replace" && mode != "merge") {
        error = where + ".mode must be replace or merge.";
        return false;
    }
    const bool merge = mode == "merge";
    const std::string label = clipui::humanoidTrackLabel(channel, humanBone);
    const int index = data.findHumanoidTrack(channel, humanBone);
    if (has(item, "keys") && item["keys"].is_array() && item["keys"].empty()) {
        if (merge || index < 0) {
            error = where + (merge ? ".keys is empty: merge adds keys." : ": the clip has no " + label + " track to remove (keys: [] removes one).");
            return false;
        }
        data.tracks.erase(data.tracks.begin() + index);
        ctx.changes.push_back("removed track " + label);
        return true;
    }
    if (!has(item, "keys") || !item["keys"].is_array()) {
        error = where + ".keys must be an array of {time, value, ease?} (value " +
                (channel == ClipChannel::Humanoid ? std::string("{quat: [x, y, z, w]} or {angles: [dof1, dof2, twist]} degrees") : std::string("[left, up, forward]")) + ").";
        return false;
    }
    // the keys as one .anim track, read by the clip parser
    Json keys = Json::array();
    for (size_t k = 0; k < item["keys"].size(); k++) {
        const Json& key = item["keys"][k];
        const std::string at = where + ".keys[" + std::to_string(k) + "]";
        if (!key.is_object() || !key.contains("time") || !key.contains("value")) {
            error = at + " must be {time, value, ease?}.";
            return false;
        }
        Json value;
        if (!humanoidKeyValue(channel, humanBone, key["value"], at + ".value", value, error)) return false;
        Json entry = {{"time", key["time"]}, {"value", value}};
        if (key.contains("ease")) entry["ease"] = key["ease"];
        // the Hermite tangents get_animation lists (units per second, as the file stores them)
        if (key.contains("in") != key.contains("out")) {
            error = at + ": in and out go together (Hermite tangents, as get_animation lists them).";
            return false;
        }
        if (key.contains("in")) {
            entry["in"] = key["in"];
            entry["out"] = key["out"];
        }
        for (auto it = key.begin(); it != key.end(); ++it) {
            if (it.key() != "time" && it.key() != "value" && it.key() != "ease" && it.key() != "in" && it.key() != "out") {
                error = at + ": unknown key \"" + it.key() + "\" (a humanoid key is {time, value, ease, in?, out?}).";
                return false;
            }
        }
        keys.push_back(entry);
    }
    Json trackJson = {{"channel", anim::channelName(channel)}, {"bone", bone}, {"keys", keys}};
    if (item.contains("loop") && item["loop"].is_boolean()) trackJson["loop"] = item["loop"];
    const Json document = {{"format", anim::ANIM_FORMAT}, {"version", anim::ANIM_VERSION}, {"tracks", Json::array({trackJson})}};
    anim::ClipParseResult parsed = anim::parseClip(document.dump());
    if (!parsed.ok() || parsed.data->tracks.size() != 1) {
        error = where + ": " + (parsed.ok() ? std::string("no valid key") : parsed.error.toString());
        return false;
    }
    for (const ClipWarning& warning : parsed.warnings) {
        // a quaternion off unit length (rounded output sent back) is normalized, not refused
        if (warning.message.find("not a unit quaternion") != std::string::npos) continue;
        if (warning.path.rfind("tracks[0].keys", 0) == 0 || warning.path.rfind("tracks[0]", 0) == 0) {
            error = where + warning.path.substr(std::string("tracks[0]").size()) + ": " + warning.message;
            return false;
        }
    }
    ClipTrackData track = parsed.data->tracks[0];
    if (merge && index >= 0) {
        ClipTrackData merged = data.tracks[(size_t)index];
        for (const ClipKey& key : track.keys) {
            auto same = std::find_if(merged.keys.begin(), merged.keys.end(), [&](const ClipKey& k) { return std::abs(k.time - key.time) < MERGE_TIME; });
            if (same != merged.keys.end()) *same = key;
            else merged.keys.push_back(key);
        }
        std::stable_sort(merged.keys.begin(), merged.keys.end(), [](const ClipKey& a, const ClipKey& b) { return a.time < b.time; });
        if (item.contains("loop") && item["loop"].is_boolean()) merged.loop = track.loop;
        track = merged;
    }
    if (index >= 0) {
        ctx.changes.push_back(std::string(merge ? "merged " : "replaced ") + std::to_string(keys.size()) + " key(s) into " + label +
                              " (" + std::to_string(track.keys.size()) + " now)");
        data.tracks[(size_t)index] = track;
    } else {
        ctx.changes.push_back("added track " + label + " (" + std::to_string(track.keys.size()) + " key(s))");
        data.tracks.push_back(track);
    }
    return true;
}

bool applyTrack(EditContext& ctx, const Json& item, size_t t, ClipData& data, std::string& error) {
    const std::string where = "tracks[" + std::to_string(t) + "]";
    if (!item.is_object()) {
        error = where + " must be an object {path, channel, keys, mode, loop, relative}.";
        return false;
    }
    {
        const std::string channel = item.contains("channel") && item["channel"].is_string() ? lower(item["channel"].get<std::string>()) : "";
        if (channel == "humanoid" || channel == "humanoid_position") {
            return applyHumanoidTrack(ctx, item, where, channel == "humanoid" ? ClipChannel::Humanoid : ClipChannel::HumanoidPosition, data, error);
        }
        if (has(item, "bone")) {
            error = where + ".bone is for humanoid tracks: give channel humanoid or humanoid_position (path tracks have a path).";
            return false;
        }
    }
    if (has(item, "start_time")) {
        error = where + ".start_time: clip tracks start at their first key: shift the keys' times.";
        return false;
    }
    if (has(item, "name")) {
        error = where + ".name: a clip track is named by its path and channel.";
        return false;
    }
    for (auto it = item.begin(); it != item.end(); ++it) {
        static const std::set<std::string> keys = {"path", "target", "channel", "keys", "mode", "loop", "relative", "component", "property"};
        if (!keys.count(it.key())) {
            error = where + ": unknown key \"" + it.key() + "\" (a clip track is {path, target, channel, keys, mode, loop, relative}; "
                    "a property track adds component and property).";
            return false;
        }
    }
    // the binding path
    std::string path;
    if (has(item, "path") && has(item, "target")) {
        error = where + ": give path or target (with root), not both.";
        return false;
    }
    if (has(item, "path")) {
        if (!item["path"].is_string()) {
            error = where + ".path must be a string: names from the root joined by '/' (\"\" = the root itself), e.g. \"Arm/Hand\", or \"Hero/mixamorig:Hips\" on a "
                    "model (its glTF root node first).";
            return false;
        }
        path = item["path"].get<std::string>();
    } else if (has(item, "target")) {
        if (ctx.root == NULL_ENTITY) {
            error = where + ".target: a clip binds by path; give path, or root (the GameObject that plays the clip) to turn target into a path.";
            return false;
        }
        std::string refError;
        const Entity target = McpRefs::resolveEntity(ctx.scene, item["target"], refError);
        if (target == NULL_ENTITY) {
            error = where + ".target: " + refError;
            return false;
        }
        if (!anim::relativePath(ctx.scene->scene, ctx.root, target, path)) {
            error = where + ".target: \"" + ctx.scene->scene->getEntityName(target) + "\" is not root \"" +
                    ctx.scene->scene->getEntityName(ctx.root) + "\" or under it (or a name on the way has '/' or ends in #<digits>).";
            return false;
        }
    } else {
        error = where + ".path is required: names from the root joined by '/' (\"\" = the root itself), e.g. \"Arm/Hand\", or \"Hero/mixamorig:Hips\" on a "
                    "model (its glTF root node first).";
        return false;
    }
    std::vector<anim::PathSegment> segments;
    if (!anim::parsePath(path, segments)) {
        error = where + ".path \"" + path + "\" is malformed: names joined by '/', no empty names, \"Name#N\" (N > 0) for the N-th later sibling of that name.";
        return false;
    }
    path = anim::formatPath(segments);

    // the channel
    ClipChannel channel = ClipChannel::Position;
    const std::string channelName = item.contains("channel") && item["channel"].is_string() ? lower(item["channel"].get<std::string>()) : "position";
    if (channelName == "property") return applyPropertyTrack(ctx, item, where, path, data, error);
    if (has(item, "component") || has(item, "property")) {
        error = where + ": component and property name a property track: give channel \"property\".";
        return false;
    }
    bool known = false;
    for (const auto& entry : channelTable()) {
        if (channelName == entry.first) {
            channel = entry.second;
            known = true;
        }
    }
    if (!known) {
        error = where + ".channel \"" + channelName + "\" is not valid. Use one of: position, rotation, scale, morph, lattice, property, humanoid, humanoid_position.";
        return false;
    }
    if ((channel == ClipChannel::Morph || channel == ClipChannel::Lattice) && item.contains("relative") && item["relative"].is_boolean() &&
        item["relative"].get<bool>()) {
        error = where + ".relative: only position, rotation and scale tracks can be relative.";
        return false;
    }
    const std::string mode = item.contains("mode") && item["mode"].is_string() ? lower(item["mode"].get<std::string>()) : "replace";
    if (mode != "replace" && mode != "merge") {
        error = where + ".mode must be replace or merge.";
        return false;
    }
    const bool merge = mode == "merge";
    const std::string label = path + " : " + anim::channelName(channel);
    int index = data.findTrack(path, channel);

    // keys: [] removes the track
    if (item.contains("keys") && item["keys"].is_array() && item["keys"].empty()) {
        if (merge) {
            error = where + ".keys is empty: merge adds keys; to remove the track use mode replace with keys [].";
            return false;
        }
        if (index < 0) {
            error = where + ": the clip has no " + label + " track to remove (keys: [] removes one).";
            return false;
        }
        data.tracks.erase(data.tracks.begin() + index);
        ctx.changes.push_back("removed track " + label);
        return true;
    }

    // a cage to read lattice keys against: the bound one when root is given
    const LatticeComponent* cage = nullptr;
    Entity cageEntity = NULL_ENTITY;
    LatticeComponent cageless = cagelessLattice();
    if (channel == ClipChannel::Lattice) {
        if (ctx.root != NULL_ENTITY) {
            cageEntity = anim::resolvePath(ctx.scene->scene, ctx.root, path);
            cage = cageEntity != NULL_ENTITY ? ctx.scene->scene->findComponent<LatticeComponent>(cageEntity) : nullptr;
        }
        if (!cage) {
            if (ctx.root != NULL_ENTITY && cageEntity != NULL_ENTITY) {
                error = where + ": \"" + path + "\" on root \"" + ctx.scene->scene->getEntityName(ctx.root) +
                        "\" has no Lattice: a lattice track's path is the cage (the lattice GameObject), not the mesh it deforms.";
                return false;
            }
            if (has(item, "keys") && item["keys"].is_array() && !checkCagelessLatticeKeys(item["keys"], where, error)) return false;
        }
    }
    std::vector<Key> keys;
    if (!anim_keys::parseKeys(keyChannel(channel), has(item, "keys") ? item["keys"] : Json(), keys, error,
                              channel == ClipChannel::Lattice ? (cage ? cage : &cageless) : nullptr)) {
        error = where + "." + error;
        return false;
    }
    std::vector<GivenTangents> tangents;
    if (!parseKeyTangents(channel, has(item, "keys") ? item["keys"] : Json(), where, tangents, error)) return false;

    ClipTrackData* existing = index >= 0 ? &data.tracks[(size_t)index] : nullptr;
    ClipTrackData track;
    if (existing) {
        track.loop = existing->loop;
        track.relative = existing->relative;
    }
    track.path = path;
    track.channel = channel;
    if (item.contains("loop") && item["loop"].is_boolean()) track.loop = item["loop"].get<bool>();
    if (item.contains("relative") && item["relative"].is_boolean()) track.relative = item["relative"].get<bool>();

    if (channel == ClipChannel::Lattice) {
        // "current" / pose / "rest" keys hold the points the other keys (and the track) animate
        LatticeTracksComponent known;
        if (existing && merge) known.points = existing->points;
        if (cage) {
            anim_keys::resolveCurrentLatticeKeys(ctx.scene->id, cageEntity, *cage, merge && existing ? &known : nullptr, keys);
            anim_keys::resolveShapeLatticeKeys(*cage, merge && existing ? &known : nullptr, keys);
        } else {
            std::set<int> points(known.points.begin(), known.points.end());
            for (const Key& key : keys) {
                if (!key.rest) points.insert(key.points.begin(), key.points.end());
            }
            for (size_t k = 0; k < keys.size(); k++) {
                if (!keys[k].rest) continue;
                if (points.empty()) {
                    error = where + ".keys[" + std::to_string(k) + "]: \"rest\" holds the points the other keys animate and this track has none: "
                            "give offsets, or root (the cage) to rest every point.";
                    return false;
                }
                keys[k].points.assign(points.begin(), points.end());
                keys[k].offsets.assign(points.size(), Vector3(0, 0, 0));
            }
        }
        for (const Key& key : keys) {
            for (int point : key.points) {
                if (point < 0 || point >= MAX_LATTICE_POINTS) {
                    error = where + ": lattice point " + std::to_string(point) + " is out of range (a cage has at most 16x16x16 points).";
                    return false;
                }
            }
        }
        std::vector<Key> all = keys;
        if (merge && existing) {
            all = latticeKeysOf(*existing);
            for (const Key& key : keys) {
                auto same = std::find_if(all.begin(), all.end(), [&](const Key& k) { return std::abs(k.time - key.time) < MERGE_TIME; });
                if (same != all.end()) *same = key;
                else all.push_back(key);
            }
            std::stable_sort(all.begin(), all.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
        }
        const LatticeTrackData columns = anim_keys::latticeTrackData(nullptr, NULL_ENTITY, all, cage ? *cage : cageless, false);
        track.points = columns.points;
        for (size_t k = 0; k < columns.times.size(); k++) {
            ClipKey key;
            key.time = columns.times[k];
            key.ease = k < columns.easings.size() ? columns.easings[k] : EaseType::LINEAR;
            for (size_t n = 0; n < columns.points.size() && k < columns.values.size(); n++) {
                const Vector3& v = columns.values[k][n];
                key.value.insert(key.value.end(), {v.x, v.y, v.z});
            }
            track.keys.push_back(key);
        }
    } else {
        // tangents (I4): a key's own in/out are kept; a key without them that equals the existing
        // key at its time (sent back unchanged, linear) keeps that key and its tangents
        std::vector<ClipKey> newKeys;
        std::vector<bool> ownSign;      // per new key: its sign is the caller's (given tangents, or the stored key kept)
        size_t tangentKeys = 0;
        for (const Key& key : keys) {
            ClipKey clipKey;
            clipKey.time = key.time;
            clipKey.ease = key.ease;
            clipKey.value = keyValue(channel, key);
            auto given = std::find_if(tangents.begin(), tangents.end(), [&](const GivenTangents& g) { return !g.used && g.time == key.time; });
            if (given != tangents.end() && (channel != ClipChannel::Morph || given->in.size() == clipKey.value.size())) {
                given->used = true;
                ownSign.push_back(true);
                clipKey.inTangent = given->in;
                clipKey.outTangent = given->out;
                // the tangents belong to the stored quaternion: the value goes to its hemisphere
                if (channel == ClipChannel::Rotation && given->quat.size() == 4 && clipKey.value.size() == 4) {
                    float dot = 0.0f;
                    for (size_t c = 0; c < 4; c++) dot += clipKey.value[c] * given->quat[c];
                    if (dot < 0.0f) {
                        for (float& c : clipKey.value) c = -c;
                    }
                }
            } else if (existing && clipKey.ease == EaseType::LINEAR) {
                auto same = std::find_if(existing->keys.begin(), existing->keys.end(), [&](const ClipKey& k) {
                    return std::abs(k.time - clipKey.time) < MERGE_TIME && k.hasTangents() && sameKeyValue(channel, clipKey.value, k.value);
                });
                ownSign.push_back(same != existing->keys.end());
                if (same != existing->keys.end()) clipKey = *same;
            } else {
                ownSign.push_back(false);
            }
            if (clipKey.hasTangents()) tangentKeys++;
            newKeys.push_back(clipKey);
        }
        // a rotation key without its own tangents written into a track with tangents keeps the sign
        // of the key it replaces; an inserted one takes its previous key's (anim::alignQuaternionKey)
        const bool tangentTrack = channel == ClipChannel::Rotation &&
            (tangentKeys > 0 || (existing && std::any_of(existing->keys.begin(), existing->keys.end(), [](const ClipKey& k) { return k.hasTangents(); })));
        std::vector<float> insertedTimes;
        if (tangentTrack) {
            for (size_t k = 0; k < newKeys.size(); k++) {
                if (ownSign[k]) continue;
                const ClipKey* replaced = nullptr;
                if (existing) {
                    for (const ClipKey& old : existing->keys) {
                        if (std::abs(old.time - newKeys[k].time) < MERGE_TIME) replaced = &old;
                    }
                }
                if (replaced) anim::alignQuaternionKey(newKeys[k].value, replaced->value);
                else insertedTimes.push_back(newKeys[k].time);
            }
        }
        if (tangentKeys > 0) ctx.changes.push_back(label + ": " + plural(tangentKeys, "key") + " with in/out tangents");
        if (merge && existing) {
            track.keys = existing->keys;
            for (const ClipKey& key : newKeys) {
                auto same = std::find_if(track.keys.begin(), track.keys.end(), [&](const ClipKey& k) { return std::abs(k.time - key.time) < MERGE_TIME; });
                if (same != track.keys.end()) *same = key;
                else track.keys.push_back(key);
            }
            std::stable_sort(track.keys.begin(), track.keys.end(), [](const ClipKey& a, const ClipKey& b) { return a.time < b.time; });
        } else {
            track.keys = newKeys;
        }
        for (size_t k = 0; k < track.keys.size(); k++) {
            if (std::find(insertedTimes.begin(), insertedTimes.end(), track.keys[k].time) == insertedTimes.end()) continue;
            if (k > 0) anim::alignQuaternionKey(track.keys[k].value, track.keys[k - 1].value);
            else if (k + 1 < track.keys.size()) anim::alignQuaternionKey(track.keys[k].value, track.keys[k + 1].value);
        }
    }

    if (existing) {
        ctx.changes.push_back(std::string(merge ? "merged " : "replaced ") + std::to_string(keys.size()) + " key(s) into " + label +
                              " (" + std::to_string(track.keys.size()) + " now)");
        *existing = track;
    } else {
        ctx.changes.push_back("added track " + label + " (" + std::to_string(track.keys.size()) + " key(s))");
        data.tracks.push_back(track);
    }
    return true;
}

bool applyEvents(EditContext& ctx, const Json& args, ClipData& data, std::string& error) {
    if (!has(args, "events")) {
        if (has(args, "events_mode") && !has(args, "audio")) {
            error = "events_mode needs events: [{time, function, args}] or audio: [{time, sound | source | snapshot}].";
            return false;
        }
        return true;
    }
    const std::string mode = has(args, "events_mode") && args["events_mode"].is_string() ? lower(args["events_mode"].get<std::string>()) : "replace";
    if (mode != "replace" && mode != "merge") {
        error = "events_mode must be replace (the whole list) or merge (same time and function replaces, others are added).";
        return false;
    }
    const Json& list = args["events"];
    if (!list.is_array()) {
        error = "events must be an array of {time, function, args} ([] clears them with events_mode replace).";
        return false;
    }
    std::vector<ClipEventData> parsed;
    for (size_t i = 0; i < list.size(); i++) {
        const std::string where = "events[" + std::to_string(i) + "]";
        const Json& item = list[i];
        if (!item.is_object()) {
            error = where + " must be {time, function, args}.";
            return false;
        }
        for (auto it = item.begin(); it != item.end(); ++it) {
            if (it.key() != "time" && it.key() != "function" && it.key() != "args") {
                error = where + ": unknown key \"" + it.key() + "\" (an event is {time, function, args}).";
                return false;
            }
        }
        ClipEventData event;
        if (!item.contains("time") || !item["time"].is_number() || item["time"].get<double>() < 0.0) {
            error = where + ".time must be a number of seconds >= 0.";
            return false;
        }
        event.time = item["time"].get<float>();
        if (!item.contains("function") || !item["function"].is_string() || !anim::isLuaIdentifier(item["function"].get<std::string>())) {
            const std::string given = item.contains("function") && item["function"].is_string() ? item["function"].get<std::string>() : "";
            const bool keyword = !given.empty() && given.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos;
            error = where + ".function must be a Lua identifier" + std::string(keyword ? ", not a keyword" : "") +
                    ": the script method it calls (e.g. \"onFootstep\").";
            return false;
        }
        event.function = item["function"].get<std::string>();
        if (item.contains("args")) {
            const Json& a = item["args"];
            if (!a.is_array() || a.size() > MAX_ANIMATION_EVENT_ARGS) {
                error = where + ".args must be an array of at most " + std::to_string(MAX_ANIMATION_EVENT_ARGS) + " numbers, strings or booleans.";
                return false;
            }
            for (size_t n = 0; n < a.size(); n++) {
                if (a[n].is_boolean()) event.args.push_back(AnimationEvent::boolArg(a[n].get<bool>()));
                else if (a[n].is_number()) event.args.push_back(AnimationEvent::numberArg(a[n].get<double>()));
                else if (a[n].is_string()) event.args.push_back(AnimationEvent::stringArg(a[n].get<std::string>()));
                else {
                    error = where + ".args[" + std::to_string(n) + "] must be a number, string or boolean.";
                    return false;
                }
            }
        }
        parsed.push_back(event);
    }
    if (mode == "replace") {
        ctx.changes.push_back("events: " + std::to_string(parsed.size()) + " (replaced " + std::to_string(data.events.size()) + ")");
        data.events = parsed;
    } else {
        size_t added = 0, replaced = 0;
        for (const ClipEventData& event : parsed) {
            auto same = std::find_if(data.events.begin(), data.events.end(), [&](const ClipEventData& e) {
                return e.function == event.function && std::abs(e.time - event.time) < MERGE_TIME;
            });
            if (same != data.events.end()) {
                *same = event;
                replaced++;
            } else {
                data.events.push_back(event);
                added++;
            }
        }
        ctx.changes.push_back("events: " + std::to_string(added) + " added, " + std::to_string(replaced) + " replaced");
    }
    return true;
}

// audio cues (S40 R12): events_mode's replace (the whole list) or merge (same time and kind replaces)
bool applyAudio(EditContext& ctx, const Json& args, ClipData& data, std::string& error) {
    if (!has(args, "audio")) return true;
    const std::string mode = has(args, "events_mode") && args["events_mode"].is_string() ? lower(args["events_mode"].get<std::string>()) : "replace";
    if (mode != "replace" && mode != "merge") {
        error = "events_mode must be replace (the whole list) or merge (a cue at the same time and of the same kind replaces, others are added).";
        return false;
    }
    const Json& list = args["audio"];
    if (!list.is_array()) {
        error = "audio must be an array of cues {time, sound | source | snapshot, ...} ([] clears them with events_mode replace).";
        return false;
    }
    std::vector<ClipAudioCue> parsed;
    for (size_t i = 0; i < list.size(); i++) {
        ClipAudioCue cue;
        if (!parseAudioCue(ctx.project, list[i], "audio[" + std::to_string(i) + "]", false, cue, error)) return false;
        parsed.push_back(cue);
    }
    if (mode == "replace") {
        ctx.changes.push_back("audio: " + std::to_string(parsed.size()) + " cue(s) (replaced " + std::to_string(data.audio.size()) + ")");
        data.audio = parsed;
    } else {
        size_t added = 0, replaced = 0;
        for (const ClipAudioCue& cue : parsed) {
            auto same = std::find_if(data.audio.begin(), data.audio.end(), [&](const ClipAudioCue& c) {
                return c.kind == cue.kind && std::abs(c.time - cue.time) < MERGE_TIME;
            });
            if (same != data.audio.end()) {
                *same = cue;
                replaced++;
            } else {
                data.audio.push_back(cue);
                added++;
            }
        }
        ctx.changes.push_back("audio: " + std::to_string(added) + " cue(s) added, " + std::to_string(replaced) + " replaced");
    }
    return true;
}

bool applyEdits(EditContext& ctx, const Json& args, ClipData& data, std::string& error) {
    const float lengthBefore = data.effectiveLength();
    if (!applySettings(ctx, args, data, error)) return false;
    if (has(args, "tracks")) {
        if (!args["tracks"].is_array()) {
            error = "tracks must be an array of {path, channel, keys, mode, loop, relative}.";
            return false;
        }
        for (size_t t = 0; t < args["tracks"].size(); t++) {
            if (!applyTrack(ctx, args["tracks"][t], t, data, error)) return false;
        }
    }
    if (!applyEvents(ctx, args, data, error)) return false;
    if (!applyAudio(ctx, args, data, error)) return false;
    if (!data.hasExplicitLength() && data.effectiveLength() != lengthBefore && !data.events.empty() &&
        data.events.back().time >= data.effectiveLength() - anim::ANIM_TIME_EPSILON) {
        ctx.changes.push_back("length now " + fmt(data.effectiveLength()) + " s (the event at " + fmt(data.events.back().time) +
                              " s; set length to keep it shorter)");
    }
    return true;
}

// The root GameObject: `root` (or `entity` for get_animation) in `scene`
bool resolveRoot(Project* project, const Json& args, const char* key, SceneProject*& scene, Entity& root, std::string& error) {
    scene = nullptr;
    root = NULL_ENTITY;
    if (!has(args, key)) return true;
    scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) {
        error = "scene: " + error;
        return false;
    }
    if (!scene->scene) {
        error = McpRefs::notOpenError(project, *scene);
        return false;
    }
    root = McpRefs::resolveEntity(scene, args[key], error);
    if (root == NULL_ENTITY) {
        error = std::string(key) + ": " + error;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ create {from}

// create {from: <entity Animation>}: the converted clip with this call's edits, written by one
// scene step that also gives the root an Animation Player listing it (9.R4)
bool createFromEntity(const CallPtr& call, EditContext& ctx, const Json& from, const std::string& relative, const fs::path& absolute,
                      std::string& text, Json& out) {
    Project* project = ctx.project;
    const Json& args = call->args();
    std::string error;
    SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene || !scene->scene) {
        call->fail("create.from: " + (scene ? McpRefs::notOpenError(project, *scene) : error));
        return false;
    }
    if (scene->playState != ScenePlayState::STOPPED) {
        call->fail("create.from: the scene is playing; stop play mode first.");
        return false;
    }
    const Entity animation = McpRefs::resolveEntity(scene, from, error);
    if (animation == NULL_ENTITY) {
        call->fail("create.from: " + error);
        return false;
    }
    if (!scene->scene->findComponent<AnimationComponent>(animation)) {
        call->fail("create.from: \"" + scene->scene->getEntityName(animation) + "\" is not an entity Animation; from takes an Animation "
                   "GameObject (Convert to Clip) or a model clip \"assets/models/hero.gltf#Walk\" (Extract).");
        return false;
    }
    if (ctx.root != NULL_ENTITY && ctx.scene != scene) {
        call->fail("root and create.from must be in the same scene.");
        return false;
    }
    Entity root = ctx.root;
    if (root == NULL_ENTITY) {
        const ActionComponent* action = scene->scene->findComponent<ActionComponent>(animation);
        root = action && action->target != NULL_ENTITY && scene->scene->isEntityCreated(action->target) ? action->target : NULL_ENTITY;
    }
    if (root == NULL_ENTITY) {
        call->fail("create.from: the Animation has no target; pass root (the GameObject that will play the clip).");
        return false;
    }
    ClipData data;
    ClipConvertReport report;
    if (!ClipConverters::convertEntityAnimation(scene->scene, animation, root, data, report, &error)) {
        call->fail("create.from: " + error);
        return false;
    }
    ctx.scene = scene;
    ctx.root = root;
    ctx.changes.push_back("converted " + std::to_string(report.convertedTracks) + " track(s) of the Animation \"" +
                          scene->scene->getEntityName(animation) + "\"");
    for (const std::string& skipped : report.notConverted) ctx.changes.push_back("not converted (kept in the Animation): " + skipped);
    if (!applyEdits(ctx, args, data, error)) {
        call->fail(error + " (nothing was changed)");
        return false;
    }
    const Json& create = args["create"];
    const bool removeTracks = create.is_object() && create.contains("remove_tracks") && create["remove_tracks"].is_boolean() &&
                              create["remove_tracks"].get<bool>();
    // where it came from (Convert's `source`: the scene file)
    if (data.source.empty()) {
        data.source.kind = "entity";
        data.source.name = scene->scene->getEntityName(animation);
    }
    if (!scene->filepath.empty()) {
        data.source.path = projectRel(project, scene->filepath.is_absolute() ? scene->filepath : project->getProjectPath() / scene->filepath);
    }
    std::vector<ClipWarning> warnings = report.warnings;
    ClipData normalized = data;
    anim::normalize(normalized, &warnings);
    Json warningList = warningsJson(warnings);
    if (!insideAssets(project, absolute)) {
        warningList.push_back(relative + " is outside the assets folder: an Animation Player in a game cannot load it");
    }

    std::vector<Command*> commands;
    ClipActionResult result;
    const std::string content = ClipSceneActions::newClipText(project, data);
    AnimationPlayerClip entry = ClipConverters::playerEntry(project, relative);
    entry.guid = anim::readClipGuid(content);
    ClipSceneActions::appendPlayerCommands(project, scene->id, root, {entry}, commands, result);
    size_t removed = 0;
    if (removeTracks && !report.convertedTrackEntities.empty()) {
        // as Convert to Clip's "Remove the converted tracks": the frames, then their entities
        std::vector<ActionFrame> kept;
        for (const ActionFrame& frame : scene->scene->getComponent<AnimationComponent>(animation).actions) {
            const auto& converted = report.convertedTrackEntities;
            if (std::find(converted.begin(), converted.end(), frame.action) == converted.end()) kept.push_back(frame);
        }
        commands.push_back(new PropertyCmd<std::vector<ActionFrame>>(project, scene->id, animation, ComponentType::AnimationComponent,
                                                                     "actions", kept));
        commands.push_back(new DeleteEntityCmd(project, scene->id, report.convertedTrackEntities, true));
        removed = report.convertedTrackEntities.size();
        ctx.changes.push_back("removed the " + std::to_string(removed) + " converted track(s) from the Animation");
    } else if (!report.convertedTrackEntities.empty()) {
        // two writers on the same channels when the Animation plays (it autoplays, or a model's
        // animation Model:playAnimation finds)
        const ActionComponent* action = scene->scene->findComponent<ActionComponent>(animation);
        bool modelAnimation = false;
        if (const ModelComponent* model = scene->scene->findComponent<ModelComponent>(root)) {
            modelAnimation = std::find(model->animations.begin(), model->animations.end(), animation) != model->animations.end();
        }
        const std::string name = scene->scene->getEntityName(animation);
        warningList.push_back("the entity Animation \"" + name + "\" keeps its " + std::to_string(report.convertedTrackEntities.size()) +
                              " converted track(s), which also animate these channels when it plays" +
                              (action && action->state == ActionState::Running ? std::string(" (it autoplays)") :
                               modelAnimation ? std::string(" (Model:playAnimation can play it)") : std::string("")) +
                              ": pass create.remove_tracks, or delete it (delete_entities)");
    }
    if (!ClipSceneActions::runStep(project, scene->id, {{relative, content}}, commands, root, result, error)) {
        call->fail("create.from: " + error);
        return false;
    }
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, relative, &error);
    if (!document) {
        call->fail("create.from: the clip was written but can't be read back: " + error);
        return false;
    }
    const size_t maxKeys = static_cast<size_t>(std::max(1, args.contains("max_keys") && args["max_keys"].is_number() ? args["max_keys"].get<int>() : 20));
    out = clipJson(project, document->path(), document->data(), false, maxKeys);
    out["warnings"] = warningList;
    out["player"] = McpRefs::entityJson(scene, root);
    out["added_player"] = result.addedPlayer;
    out["removed_tracks"] = removed;
    text = " (one scene undo step: the file and the Animation Player of \"" + scene->scene->getEntityName(root) + "\"" +
           (removed ? std::string(", the converted tracks removed") : std::string()) + "; undo scope scene)";
    return true;
}

} // namespace

bool readAudioCue(Project* project, const Json& item, const std::string& where, bool entityMode, ClipAudioCue& cue, std::string& error) {
    return parseAudioCue(project, item, where, entityMode, cue, error);
}

std::string toolPath(Project* project, const std::string& assetRelative) {
    return projectPathOf(project, assetRelative);
}

// ================================================================== public

std::string rejectedArgument(const Json& args, bool fileMode) {
    if (fileMode) {
        for (const Rejection& rejection : fileModeRejections()) {
            if (has(args, rejection.arg)) return std::string(rejection.message) + " (clip mode edits an .anim file).";
        }
        return "";
    }
    std::vector<std::string> given;
    for (const Rejection& rejection : sceneModeRejections()) {
        if (has(args, rejection.arg)) given.push_back(rejection.arg);
    }
    if (!given.empty()) {
        std::string names;
        for (size_t i = 0; i < given.size(); i++) names += (i ? ", " : "") + given[i];
        return names + (given.size() == 1 ? " is a clip-file setting" : " are clip-file settings") +
               ": pass clip (an .anim path) to edit a clip; an entity Animation uses loop, duration, target and tracks[].target.";
    }
    if (has(args, "tracks") && args["tracks"].is_array()) {
        for (size_t t = 0; t < args["tracks"].size(); t++) {
            if (has(args["tracks"][t], "path")) {
                return "tracks[" + std::to_string(t) + "].path binds a clip track: pass clip (an .anim path); an entity Animation's tracks use target.";
            }
        }
    }
    return "";
}

void editClip(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error = rejectedArgument(args, true);
    if (!error.empty()) {
        call->fail(error);
        return;
    }
    std::string relative;
    fs::path absolute;
    bool subClip = false;
    if (!resolveClipArg(project, args["clip"], "clip", true, relative, absolute, subClip, error, true)) {
        call->fail(error);
        return;
    }
    if (subClip) {
        call->fail("clip: \"" + relative + "\" is a model clip (read-only): extract it with edit_animation {clip: \"assets/anim/<name>.anim\", "
                   "create: {from: \"" + relative + "\"}} and edit the file.");
        return;
    }
    EditContext ctx;
    ctx.project = project;
    if (!resolveRoot(project, args, "root", ctx.scene, ctx.root, error)) {
        call->fail(error);
        return;
    }
    // create: false reads as no create
    const bool creating = has(args, "create") && !(args["create"].is_boolean() && !args["create"].get<bool>());
    std::error_code ec;
    const bool exists = fs::exists(absolute, ec);
    Json from;
    bool makeFolder = false;
    bool humanoidCreate = false;    // create.humanoid: Convert to Humanoid Clip (anim-retarget 10)
    std::string humanoidModel;
    if (creating) {
        const Json& create = args["create"];
        if (create.is_boolean() && create.get<bool>()) {
            // create: true reads as create: {}
        } else if (!create.is_object()) {
            call->fail("create must be an object {from?, remove_tracks?}: from an entity Animation GameObject (Convert) or a model clip \"assets/models/hero.gltf#Walk\" (Extract); {} makes an empty clip.");
            return;
        } else {
            for (auto it = create.begin(); it != create.end(); ++it) {
                if (it.key() != "from" && it.key() != "remove_tracks" && it.key() != "humanoid") {
                    call->fail("create: unknown key \"" + it.key() + "\" (create is {from?, remove_tracks?, humanoid?}).");
                    return;
                }
            }
            if (create.contains("from")) from = create["from"];
            if (create.contains("humanoid")) {
                const Json& humanoid = create["humanoid"];
                if (!humanoid.is_object() || (humanoid.size() > 1 || (humanoid.size() == 1 && !humanoid.contains("model"))) ||
                    (humanoid.contains("model") && !humanoid["model"].is_string())) {
                    call->fail("create.humanoid must be {model?: \"assets/models/<source>.gltf\"} (the rig the clip was made on; default: its "
                               "model, source or preview model).");
                    return;
                }
                if (!from.is_string()) {
                    call->fail("create.humanoid converts create.from: give from (an .anim or a model clip \"<model>#<animation>\").");
                    return;
                }
                humanoidCreate = true;
                if (humanoid.contains("model")) humanoidModel = humanoid["model"].get<std::string>();
            }
            if (create.contains("remove_tracks")) {
                anim::SubClipReference reference;
                if (!create["remove_tracks"].is_boolean()) {
                    call->fail("create.remove_tracks must be true or false.");
                    return;
                }
                if (from.is_null() || (from.is_string() && anim::parseSubClipReference(from.get<std::string>(), reference))) {
                    call->fail("create.remove_tracks removes the converted tracks of create.from's entity Animation: it needs from: <an Animation GameObject>.");
                    return;
                }
            }
            // a path given as from: a clip file (copy it) or a model without "#<animation>"
            if (from.is_string() && !humanoidCreate) {
                const std::string given = from.get<std::string>();
                anim::SubClipReference reference;
                const std::string extension = lower(editor::FileUtils::pathFromUtf8(given).extension().string());
                if (!anim::parseSubClipReference(given, reference) && (extension == ".anim" || anim::isModelFilePath(given))) {
                    call->fail("create.from takes an entity Animation GameObject or a model clip \"<model>#<animation>\" (e.g. \"" +
                               (anim::isModelFilePath(given) ? given : std::string("assets/models/hero.gltf")) + "#Walk\"); copy a clip with "
                               "move_asset {path, to, copy: true}.");
                    return;
                }
            }
        }
        if (exists) {
            call->fail("create: " + relative + " already exists; leave out create to edit it.");
            return;
        }
        // a missing folder is made when the clip is written, as the pose tools do (agents review M1)
        makeFolder = !fs::is_directory(absolute.parent_path(), ec);
        if (makeFolder && fs::exists(absolute.parent_path(), ec)) {
            call->fail("create: " + projectRel(project, absolute.parent_path()) + " is a file, not a folder.");
            return;
        }
    }
    const size_t maxKeys = static_cast<size_t>(std::max(1, args.contains("max_keys") && args["max_keys"].is_number() ? args["max_keys"].get<int>() : 20));

    std::shared_ptr<AnimationClipDocument> document;
    ClipData data;
    std::vector<ClipWarning> sourceWarnings;
    std::string extractedFrom;      // Extract: the model clip (project-relative)
    Json humanoidReport;
    if (creating && humanoidCreate) {
        // the source clip (a file or a model clip) converted on its source rig
        std::string fromRelative;
        fs::path fromAbsolute;
        bool fromSub = false;
        if (!resolveClipArg(project, from, "create.from", true, fromRelative, fromAbsolute, fromSub, error)) {
            call->fail(error);
            return;
        }
        const std::string stored = fromSub ? assetForm(project, fromRelative) : std::string();
        ClipData sourceData;
        if (fromSub) {
            if (!anim::buildModelSubClip(stored, sourceData, sourceWarnings, &error)) {
                call->fail("create.from: " + error);
                return;
            }
        } else {
            std::shared_ptr<AnimationClipDocument> source = AnimationClipDocument::open(project, fromRelative, &error);
            if (!source) {
                call->fail("create.from: " + error);
                return;
            }
            sourceData = source->data();
        }
        std::string model = humanoidModel.empty() ? ClipConverters::humanoidSourceModel(stored, sourceData) : std::string();
        if (!humanoidModel.empty()) {
            fs::path modelAbsolute;
            if (!anim::isModelFilePath(humanoidModel) || !McpRefs::resolveProjectPath(project, humanoidModel, modelAbsolute, error, true)) {
                call->fail("create.humanoid.model: " + (error.empty() ? "\"" + humanoidModel + "\" is not a .gltf, .glb or .fbx model" : error) + ".");
                return;
            }
            model = assetForm(project, projectRel(project, modelAbsolute.lexically_normal()));
        }
        if (model.empty()) {
            call->fail("create.humanoid: " + fromRelative + " names no source model: pass humanoid: {model: \"assets/models/<the rig it was made "
                       "on>.gltf\"} (or set the clip's preview model).");
            return;
        }
        anim::HumanoidConversionReport conversion;
        if (!ClipConverters::convertToHumanoid(sourceData, model, data, sourceWarnings, &error, &conversion)) {
            call->fail("create.humanoid: " + error);
            return;
        }
        humanoidReport = {{"source_model", projectPathOf(project, model)}, {"human_scale", r6(data.humanoid.humanScale)},
                          {"mapped_slots", conversion.mappedSlots.size()}, {"unmapped_slots", conversion.unmappedSlots},
                          {"extra_bones", conversion.mappedExtra}, {"generic_tracks", conversion.genericTracks.size()},
                          {"dropped", conversion.droppedBones}};
        ctx.changes.push_back("converted " + fromRelative + " to a humanoid clip on " + projectPathOf(project, model) + " (" +
                              std::to_string(conversion.mappedSlots.size()) + " slots, " + std::to_string(conversion.mappedExtra.size()) +
                              " finger/eye/jaw bones, " + std::to_string(conversion.genericTracks.size()) + " generic tracks kept)");
    } else if (creating && !from.is_null()) {
        anim::SubClipReference reference;
        if (from.is_string() && anim::parseSubClipReference(from.get<std::string>(), reference)) {
            // Extract: the model's clip as an editable file (a project step; the model is untouched)
            std::string modelRelative;
            fs::path model;
            bool isSub = false;
            if (!resolveClipArg(project, from, "create.from", true, modelRelative, model, isSub, error)) {
                call->fail(error);
                return;
            }
            const std::string stored = assetForm(project, modelRelative);
            if (!ClipConverters::extractModelSubClip(stored, data, sourceWarnings, &error)) {
                call->fail("create.from: " + error);
                return;
            }
            anim::SubClipReference parsed;
            anim::parseSubClipReference(stored, parsed);
            if (data.previewModel.empty()) data.previewModel = parsed.model;
            extractedFrom = modelRelative;
            ctx.changes.push_back("extracted " + modelRelative + " (" + std::to_string(data.tracks.size()) + " track(s))");
        } else {
            std::string text;
            Json out;
            if (!createFromEntity(call, ctx, from, relative, absolute, text, out)) return;
            out["created"] = true;
            out["changes"] = ctx.changes;
            const std::vector<ClipUser> users = findClipUsers(project, relative);
            out["users"] = usersJson(project, users);
            const Json bindings = bindingsFor(project, ctx.scene, ctx.root, AnimationClipDocument::open(project, relative, nullptr)->data(), users,
                                              absolute);
            if (!bindings.is_null()) out["bindings"] = bindings;
            std::string summary = relative + ": created, " + plural(out["tracks"].size(), "track") + ", " + fmt(out["length"].get<double>()) + " s, " +
                                  out["wrap"].get<std::string>() + text;
            for (const Json& line : out["changes"]) summary += "\n" + line.get<std::string>();
            for (const Json& warning : out["warnings"]) summary += "\nwarning: " + warning.get<std::string>();
            if (!bindings.is_null()) summary += "\n" + bindingsText(bindings);
            call->ok(summary, out);
            return;
        }
    } else if (creating) {
        ctx.changes.push_back("created " + relative);
    } else {
        document = AnimationClipDocument::open(project, relative, &error);
        if (!document) {
            call->fail("clip: " + error + ". Pass create: {} to make a new clip (or create: {from}).");
            return;
        }
        data = document->data();
    }

    if (!applyEdits(ctx, args, data, error)) {
        call->fail(error + " (nothing was changed: a call is all or nothing)");
        return;
    }
    if (!creating && ctx.changes.empty()) {
        call->fail("Nothing to do: give tracks, events, wrap, frame_rate or length (or create).");
        return;
    }

    std::vector<ClipWarning> warnings = sourceWarnings;
    {
        ClipData normalized = data;
        anim::normalize(normalized, &warnings);
    }
    const uint64_t before = document ? document->revision() : 0;
    // the folders made for the clip go again when it can't be written (nothing left behind by a
    // failed call); kept once the file is there
    // (before the reply too: the client may look at the disk as soon as it has the answer)
    struct MadeFolders {
        std::vector<fs::path> folders;      // deepest first
        bool keep = false;
        // before the call fails (its reply may reach the client before this function returns)
        void removeNow() {
            if (keep) return;
            for (const fs::path& folder : folders) {
                std::error_code ignored;
                if (fs::is_directory(folder, ignored) && fs::is_empty(folder, ignored)) FileUtils::removePath(folder, ignored);
            }
            folders.clear();
        }
        ~MadeFolders() { removeNow(); }
    } madeFolders;
    if (makeFolder) {
        std::error_code folderError;
        for (fs::path folder = absolute.parent_path(); !folder.empty() && !fs::exists(folder, folderError); folder = folder.parent_path()) {
            madeFolders.folders.push_back(folder);
            if (folder == folder.parent_path()) break;
        }
        FileUtils::createDirectories(absolute.parent_path(), folderError);
        if (!fs::is_directory(absolute.parent_path(), folderError)) {
            madeFolders.removeNow();
            call->fail("create: cannot make the folder " + projectRel(project, absolute.parent_path()) +
                       (folderError ? ": " + folderError.message() : std::string()));
            return;
        }
        ctx.changes.push_back("made the folder " + projectRel(project, absolute.parent_path()));
    }
    // Extract with root: the root's Animation Player entries of the model clip switch to the new
    // file in the same call, one scene step (the UI's Extract to .anim); else the file alone
    std::string switched;
    std::vector<ClipUser> modelClipUsers;
    if (!extractedFrom.empty()) {
        modelClipUsers = findClipUsers(project, extractedFrom);
        const bool rootPlaysIt = ctx.root != NULL_ENTITY && std::any_of(modelClipUsers.begin(), modelClipUsers.end(), [&](const ClipUser& user) {
            return !user.prefabAsset && user.sceneId == ctx.scene->id && user.entity == ctx.root;
        });
        if (rootPlaysIt) {
            ClipActionResult result;
            if (!ClipSceneActions::extractSubClip(project, ctx.scene->id, ctx.root, extractedFrom, relative, true, result, error, &data)) {
                madeFolders.removeNow();
                call->fail("create.from: " + error);
                return;
            }
            document = AnimationClipDocument::open(project, relative, &error);
            if (!document) {
                madeFolders.removeNow();
                call->fail("create.from: the clip was written but can't be read back: " + error);
                return;
            }
            std::string entries;
            for (size_t i = 0; i < result.entries.size(); i++) entries += (i ? ", " : "") + std::to_string(result.entries[i]);
            switched = "; the Animation Player of \"" + ctx.scene->scene->getEntityName(ctx.root) + "\" plays the file now (clips[" + entries +
                       "] were " + extractedFrom + ")";
            ctx.changes.push_back("switched clips[" + entries + "] of \"" + ctx.scene->scene->getEntityName(ctx.root) + "\" to " + relative);
            modelClipUsers = findClipUsers(project, extractedFrom);
        }
    }
    if (document && !switched.empty()) {
        // written by the scene step above
    } else if (creating) {
        document = AnimationClipDocument::create(project, relative, data, humanoidCreate ? "Convert to Humanoid Clip" :
                                                 from.is_null() ? "Create Animation Clip" : "Extract to .anim", &error);
        if (!document) {
            madeFolders.removeNow();
            call->fail("create: " + error);
            return;
        }
    } else if (!document->commit(data, "Edit Animation Clip", "", &error)) {
        madeFolders.removeNow();
        call->fail(error);
        return;
    }
    madeFolders.keep = true;
    const bool changed = creating || document->revision() != before;

    const std::vector<ClipUser> users = findClipUsers(project, relative);
    Json out = clipJson(project, document->path(), document->data(), false, maxKeys);
    out["created"] = creating;
    out["changes"] = ctx.changes;
    if (!humanoidReport.is_null()) out["humanoid"] = humanoidReport;
    Json warningList = warningsJson(warnings);
    if (!insideAssets(project, absolute)) {
        warningList.push_back(relative + " is outside the assets folder: an Animation Player in a game cannot load it");
    }
    out["warnings"] = warningList;
    out["users"] = usersJson(project, users);
    if (users.size() > 50) out["users_total"] = users.size();
    if (!extractedFrom.empty() && !modelClipUsers.empty()) {
        // players that still play the read-only model clip: edits of the file don't show there
        out["model_clip_users"] = usersJson(project, modelClipUsers);
    }
    const Json bindings = bindingsFor(project, ctx.scene, ctx.root, document->data(), users, document->absolutePath());
    if (!bindings.is_null()) out["bindings"] = bindings;

    std::string text = document->path() + ": " + (creating ? "created, " : "") + plural(document->data().tracks.size(), "track") + ", " +
                       fmt(document->data().effectiveLength()) + " s, " + anim::wrapName(document->data().wrap);
    text += users.empty() ? std::string("; no Animation Player uses it yet (set_properties clips)") :
                            "; shared by " + std::to_string(users.size()) + (users.size() == 1 ? " Animation Player entry" : " Animation Player entries") + " in open scenes and prefabs (the edit changes every user)";
    if (!switched.empty()) {
        text += switched + " (one scene undo step: the file and the player; undo scope scene)";
    } else {
        text += changed ? " (one project undo step: undo scope project)" : " (no change: nothing written)";
    }
    if (!extractedFrom.empty() && !modelClipUsers.empty()) {
        text += "\n" + std::to_string(modelClipUsers.size()) + (modelClipUsers.size() == 1 ? " Animation Player entry still plays " :
                " Animation Player entries still play ") + extractedFrom +
                " (model_clip_users), not the file, so its edits won't show there: pass root (the player's GameObject) when extracting, or "
                "set_properties {animationplayer: {\"clips[<entry>].clip\": \"" + relative + "\"}}";
    }
    for (const std::string& line : ctx.changes) text += "\n" + line;
    for (const Json& warning : warningList) text += "\nwarning: " + warning.get<std::string>();
    if (!bindings.is_null()) text += "\n" + bindingsText(bindings);
    call->ok(text, out);
}

void listProperties(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = nullptr;
    Entity root = NULL_ENTITY;
    std::string error;
    if (!has(args, "root") && !has(args, "entity")) {
        call->fail("list_properties needs root (the GameObject whose Animation Player plays the clip): "
                   "get_animation {root, list_properties: \"Panel/Stats\"} (\"\" = the root itself).");
        return;
    }
    if (!resolveRoot(project, args, has(args, "root") ? "root" : "entity", scene, root, error)) {
        call->fail(error);
        return;
    }
    if (!args["list_properties"].is_string()) {
        call->fail("list_properties is a path under root (names joined by '/', \"\" = the root itself).");
        return;
    }
    const std::string path = args["list_properties"].get<std::string>();
    Scene* engineScene = scene->scene;
    const Entity entity = anim::resolvePath(engineScene, root, path);
    if (entity == NULL_ENTITY) {
        const std::vector<std::string> nearest = ClipValidation::nearestPaths(engineScene, root, path, 3);
        call->fail("list_properties: no GameObject at \"" + path + "\" under root \"" + engineScene->getEntityName(root) + "\"" +
                   (nearest.empty() ? std::string(".") : ": did you mean \"" + nearest[0] + "\"?"));
        return;
    }
    Json properties = Json::array();
    std::string text = "\"" + (path.empty() ? engineScene->getEntityName(root) : path) + "\": animatable properties (track component, property, kind):";
    for (const anim::AnimatablePropertyInfo& info : anim::listAnimatableProperties(engineScene, entity)) {
        anim::PropertyRef ref;
        anim::parsePropertyRef(info.component, info.property, ref);
        const anim::PropertyBinding binding = anim::bindProperty(engineScene, entity, ref, 0);
        float value[4] = {0, 0, 0, 0};
        Json item = {{"component", info.component}, {"property", info.property}, {"kind", anim::kindName(info.kind)},
                     {"label", info.label}, {"group", ClipPropertyNames::groupLabel(engineScene, entity, info.component)}};
        if (info.discrete) item["discrete"] = true;
        if (binding.status == anim::PropertyBindStatus::Bound && anim::readPropertyKeySpace(engineScene, entity, binding, value)) {
            item["value"] = propertyValueJson(binding.kind, std::vector<float>(value, value + binding.width));
        }
        properties.push_back(item);
        text += "\n" + info.component + " " + info.property + " (" + anim::kindName(info.kind) + (info.discrete ? ", stepped" : "") + ")";
    }
    Json out = {{"root", McpRefs::entityJson(scene, root)}, {"path", path}, {"entity", McpRefs::entityJson(scene, entity)},
                {"properties", properties}};
    call->ok(text, out);
}

void getClip(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string relative, error;
    fs::path absolute;
    bool subClip = false;
    if (!resolveClipArg(project, args["clip"], "clip", true, relative, absolute, subClip, error)) {
        call->fail(error);
        return;
    }
    SceneProject* scene = nullptr;
    Entity root = NULL_ENTITY;
    // `entity` names the root too (get_animation's own argument)
    if (!resolveRoot(project, args, has(args, "root") ? "root" : "entity", scene, root, error)) {
        call->fail(error);
        return;
    }
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, relative, &error);
    if (!document) {
        call->fail("clip: " + error + (subClip ? "." : ". Create one with edit_animation {clip, create: {}}."));
        return;
    }
    const size_t maxKeys = static_cast<size_t>(std::max(1, args.contains("max_keys") && args["max_keys"].is_number() ? args["max_keys"].get<int>() : 50));
    const ClipData& data = document->data();
    Json out = clipJson(project, document->path(), data, document->isReadOnly(), maxKeys);
    out["warnings"] = warningsJson(document->loadWarnings());
    const std::vector<ClipUser> users = findClipUsers(project, relative);
    out["users"] = usersJson(project, users);
    if (users.size() > 50) out["users_total"] = users.size();
    if (!document->isReadOnly()) out["guid"] = data.guid;

    // sampled values at the given seconds, through the runtime sampler (headless)
    std::string sampled;
    std::vector<float> humanoidTimes;
    ClipWrapMode humanoidWrap = ClipWrapMode::Once;
    // a clip the root's entry retargets: its bindings are the conversion's (what plays)
    std::shared_ptr<const AnimationClip> retargeted;
    if (has(args, "times")) {
        const Json& times = args["times"];
        if (!times.is_array() || times.empty() || times.size() > 64) {
            call->fail("times must be an array of 1 to 64 clip times in seconds.");
            return;
        }
        std::vector<float> at;
        for (const Json& t : times) {
            if (!t.is_number() || t.get<double>() < 0.0) {
                call->fail("times must hold numbers of seconds >= 0.");
                return;
            }
            at.push_back(t.get<float>());
        }
        std::shared_ptr<const AnimationClip> clip = document->clip();
        Json tracks = Json::array();
        const std::vector<AnimationClip::Track>& runtimeTracks = clip->getTracks();
        std::vector<std::vector<anim::TrackSample>> perTime;
        // the root's Animation Player entry of this clip may override its wrap (clips[i].wrap)
        ClipWrapMode wrap = clip->getWrap();
        if (root != NULL_ENTITY) {
            if (const AnimationPlayerComponent* player = scene->scene->findComponent<AnimationPlayerComponent>(root)) {
                for (const AnimationPlayerClip& entry : player->clips) {
                    if (entry.wrap == ClipWrapMode::Default || entry.clip.empty()) continue;
                    if (AnimationClipDocument::open(project, entryClipJson(project, entry.clip), nullptr) == document) {
                        wrap = entry.wrap;
                        break;
                    }
                }
            }
        }
        for (float t : at) {
            std::vector<anim::TrackSample> samples;
            ActionSystem::sampleClip(*clip, anim::wrapClipTime(t, clip->getLength(), wrap), samples);
            perTime.push_back(samples);
        }
        for (size_t i = 0; i < runtimeTracks.size(); i++) {
            const AnimationClip::Track& track = runtimeTracks[i];
            if (!track.isSampled()) continue;
            Json values = Json::array();
            for (size_t n = 0; n < perTime.size(); n++) {
                const anim::TrackSample* sample = i < perTime[n].size() ? &perTime[n][i] : nullptr;
                if (!sample || !sample->valid) {
                    values.push_back(nullptr);
                    continue;
                }
                switch (track.channel) {
                    case ClipChannel::Position:
                    case ClipChannel::Scale:
                        values.push_back(Json::array({r6(sample->vector.x), r6(sample->vector.y), r6(sample->vector.z)}));
                        break;
                    case ClipChannel::Rotation: {
                        float xyzw[4];
                        quaternionToXYZW(sample->rotation, xyzw);
                        values.push_back(quatJson(xyzw));
                        break;
                    }
                    case ClipChannel::Humanoid: {
                        float xyzw[4];
                        quaternionToXYZW(sample->rotation, xyzw);
                        Json value = {{"quat", Json::array({r6(xyzw[0]), r6(xyzw[1]), r6(xyzw[2]), r6(xyzw[3])})}};
                        Json angles = humanoidAnglesJson(track.humanBone, xyzw);
                        if (!angles.is_null()) value["angles"] = angles;
                        values.push_back(value);
                        break;
                    }
                    case ClipChannel::HumanoidPosition:
                        values.push_back(Json::array({r6(sample->vector.x), r6(sample->vector.y), r6(sample->vector.z)}));
                        break;
                    case ClipChannel::Morph:
                        values.push_back(floatsJson(sample->weights, 0, sample->weights.size()));
                        break;
                    case ClipChannel::Lattice: {
                        Json points = Json::object();
                        for (size_t p = 0; p < sample->offsets.size() && p < track.width && track.points; p++) {
                            const Vector3& o = sample->offsets[p];
                            points[std::to_string(track.points[p])] = Json::array({r6(o.x), r6(o.y), r6(o.z)});
                        }
                        values.push_back(points);
                        break;
                    }
                    case ClipChannel::Property:
                        // key space: sRGB colours, bools as true / false
                        values.push_back(propertyValueJson(track.swizzle >= 0 ? PropertyKind::Float : track.kind, sample->weights));
                        break;
                }
            }
            Json item = {{"index", i}, {"path", track.path}, {"channel", anim::channelName(track.channel)}, {"relative", track.relative},
                         {"values", values}};
            if (track.channel == ClipChannel::Property) {
                item["component"] = track.component;
                item["property"] = track.property;
            }
            if (isHumanoidChannel(track.channel)) {
                item.erase("path");
                item.erase("relative");
                item["bone"] = anim::humanoidTrackBoneName(track.channel, track.humanBone);
            }
            tracks.push_back(item);
        }
        Json timesOut = Json::array();
        for (float t : at) timesOut.push_back(r6(t));
        // relative tracks give their own offsets / factors (not the posed values)
        out["samples"] = {{"times", timesOut}, {"wrap_used", anim::wrapName(wrap)}, {"tracks", tracks}};
        sampled = "; sampled at " + std::to_string(at.size()) + " time(s) (samples)";
        humanoidTimes = at;
        humanoidWrap = wrap;
    }

    // a humanoid clip, a clip of an Animation Type: Humanoid model (the pool serves it humanoid),
    // or one the root's player entry retargets (anim-retarget 10): its realisation on the root's
    // rig at the times, with the entry's Mirror and Foot IK, and the retarget report
    bool entryMirror = false;
    bool entryFootIK = false;
    if (root != NULL_ENTITY) {
        std::shared_ptr<const AnimationClip> humanoidClip = document->clip()->isHumanoid() ? document->clip() : nullptr;
        const AnimationPlayerComponent* player = scene->scene->findComponent<AnimationPlayerComponent>(root);
        // the root's first entry with this clip gives Mirror and Foot IK (reported as `entry`)
        const AnimationPlayerClip* playing = nullptr;
        size_t playingIndex = 0;
        if (player) {
            for (size_t e = 0; e < player->clips.size(); e++) {
                const AnimationPlayerClip& entry = player->clips[e];
                if (entry.clip.empty() || AnimationClipDocument::open(project, entryClipJson(project, entry.clip), nullptr) != document) continue;
                playing = &entry;
                playingIndex = e;
                break;
            }
        }
        if (playing) {
            entryMirror = playing->mirror;
            entryFootIK = playing->footIK;
        }
        anim::SubClipReference sub;
        if (!humanoidClip && subClip && anim::parseSubClipReference(relative, sub)) {
            const std::string model = FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(project->getProjectPath() / FileUtils::pathFromUtf8(sub.model)));
            if (ClipConverters::isHumanoidModel(model)) {
                std::string loadError;
                std::shared_ptr<const AnimationClip> pooled = AnimationClipPool::getOrLoad(model + "#" + sub.clip, &loadError);
                if (pooled && pooled->isHumanoid()) {
                    humanoidClip = pooled;
                    retargeted = pooled;
                }
            }
        }
        // what plays: the humanoid clip itself, or a Retarget entry's generic clip read through its
        // source model (the one retarget path); the bindings list the clip as Convert writes it
        std::shared_ptr<const AnimationClip> playClip = humanoidClip;
        std::shared_ptr<const anim::RetargetSource> playSource;
        if (!humanoidClip && playing && playing->retarget) {
            std::string convertError;
            playSource = anim::clipRetargetSource(*document->clip(), playing->clip, &convertError);
            ClipData converted;
            std::vector<ClipWarning> convertWarnings;
            if (playSource && anim::convertToHumanoid(document->clip()->toData(), *playSource, converted, convertWarnings, &convertError)) {
                humanoidClip = AnimationClip::build(converted, convertWarnings, nullptr);
                playClip = document->clip();
            } else {
                out["retarget_report"] = {{"error", convertError}};
                playSource.reset();
            }
            retargeted = humanoidClip;
        }
        if (humanoidClip) {
            const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(scene->scene, root);
            Json report;
            if (!slots.target) {
                report["error"] = "\"" + scene->scene->getEntityName(root) + "\" has no humanoid rig (" + slots.error +
                                  "): map its model's bones (edit_pose_animation op set_humanoid); the clip's other tracks still play";
            } else {
                anim::HumanoidBinding binding;
                anim::bindHumanoidClip(*playClip, slots.target, binding, playSource);
                binding.mirror = entryMirror;
                binding.footIK = entryFootIK;
                const ClipHumanoidInfo& info = humanoidClip->getHumanoidInfo();
                const float targetScale = slots.target->rig->humanScale();
                report["target"] = {{"model", projectPathOf(project, slots.target->modelPath)}, {"human_scale", r6(targetScale)}};
                report["source"] = {{"model", projectPathOf(project, info.sourceModel)}, {"human_scale", r6(info.humanScale)}};
                if (info.humanScale > 0.0f) report["scale_ratio"] = r6(targetScale / info.humanScale);
                report["mirror"] = entryMirror;
                report["foot_ik"] = entryFootIK;
                if (playing) report["entry"] = playingIndex;
                report["folded"] = binding.folded;
                report["missing_on_target"] = binding.missingOnTarget;
                // generic tracks that bind neither by path nor by unique bone name on this rig (the
                // player's rule, anim::uniqueNamesUnder)
                Json unmapped = Json::array();
                std::unordered_map<std::string, Entity> names;
                anim::uniqueNamesUnder(scene->scene, root, names);
                for (const AnimationClip::Track& track : humanoidClip->getTracks()) {
                    if (!track.isSampled() || isHumanoidChannel(track.channel)) continue;
                    if (anim::resolvePath(scene->scene, root, track.path) != NULL_ENTITY) continue;
                    std::vector<anim::PathSegment> segments;
                    if (anim::parsePath(track.path, segments) && !segments.empty() && segments.back().index == 0) {
                        auto found = names.find(segments.back().name);
                        if (found != names.end() && found->second != NULL_ENTITY) continue;
                    }
                    if (std::find(unmapped.begin(), unmapped.end(), Json(track.path)) == unmapped.end()) unmapped.push_back(track.path);
                }
                report["unmapped_source_bones"] = unmapped;
                if (!humanoidTimes.empty()) {
                    Json realised = Json::array();
                    for (float t : humanoidTimes) {
                        anim::sampleHumanoidBinding(*playClip, binding, anim::wrapClipTime(t, playClip->getLength(), humanoidWrap), nullptr);
                        Json bones = Json::array();
                        Json hips = nullptr;
                        for (const anim::HumanoidOutput& output : binding.outputs) {
                            if (!anim::isLive(binding, output)) continue;
                            std::string path;
                            anim::relativePath(scene->scene, root, output.entity, path);
                            if (output.kind == anim::HumanoidOutput::Kind::Hips) {
                                hips = Json::array({r6(output.position.x), r6(output.position.y), r6(output.position.z)});
                                continue;
                            }
                            float xyzw[4];
                            quaternionToXYZW(output.rotation, xyzw);
                            const bool slot = output.kind == anim::HumanoidOutput::Kind::Slot;
                            Json item = {{"bone", slot ? pose::humanBoneName(pose::boneAt(output.index)) : pose::humanExtraBoneName(pose::extraAt(output.index))},
                                         {"entity", McpRefs::entityJson(scene, output.entity)}, {"path", path}, {"local_rotation", quatJson(xyzw)}};
                            if (slot) {
                                float local[4];
                                quaternionToXYZW(binding.pose.rotations[output.index], local);
                                float angles[pose::DOFS_PER_BONE];
                                slots.target->rig->anglesFromLocal(pose::boneAt(output.index), binding.pose.rotations[output.index], angles);
                                Json named = Json::object();
                                const pose::BoneDofInfo& dofs = pose::boneDofInfo(pose::boneAt(output.index));
                                for (int k = 0; k < pose::DOFS_PER_BONE; k++) named[dofs.dofs[k].name] = std::round(angles[k] * 1000.0) / 1000.0;
                                item["angles"] = named;
                            }
                            bones.push_back(item);
                        }
                        realised.push_back({{"time", r6(t)}, {"bones", bones}, {"hips_local_position", hips}});
                    }
                    out["realised"] = realised;
                    sampled += ", realised on \"" + scene->scene->getEntityName(root) + "\"'s humanoid rig (realised" +
                               std::string(entryMirror ? ", mirrored" : "") + (entryFootIK ? ", Foot IK" : "") + ")";
                }
            }
            out["retarget_report"] = report;
        }
    }

    FixableClip fixable{document->isReadOnly() || retargeted ? nullptr : &data, document->absolutePath()};
    const Json bindings = root != NULL_ENTITY ? bindingsJson(scene, ClipValidation::validateInScene(project, scene,
                                                             retargeted ? retargeted->toData() : data, root), &fixable)
                                              : Json(nullptr);
    if (!bindings.is_null()) out["bindings"] = bindings;

    std::string text = document->path() + (document->isReadOnly() ? " (read-only model clip)" : "") + ": " + plural(data.tracks.size(), "track") +
                       ", " + plural(data.events.size(), "event") + ", " + fmt(data.effectiveLength()) + " s, " + anim::wrapName(data.wrap) + ", " +
                       fmt(data.frameRate) + " fps; " + plural(users.size(), "user") + sampled;
    for (const ClipTrackData& track : data.tracks) {
        text += "\n" + ClipValidation::trackLabel(track) + " (" + plural(track.keys.size(), "key") + (track.loop ? ", loop" : "") +
                (track.relative ? ", relative" : "") + ")";
    }
    for (const Json& warning : out["warnings"]) text += "\nwarning: " + warning.get<std::string>();
    if (!bindings.is_null()) text += "\n" + bindingsText(bindings);
    if (out.contains("retarget_report")) {
        const Json& report = out["retarget_report"];
        if (report.contains("error")) {
            text += "\nretarget: " + report["error"].get<std::string>();
        } else {
            text += "\nretarget: on " + report["target"]["model"].get<std::string>() + " (human scale " + fmt(report["target"]["human_scale"].get<double>()) +
                    (report.contains("scale_ratio") ? ", " + fmt(report["scale_ratio"].get<double>()) + "x the source's" : std::string()) + ")";
            for (const Json& line : report["folded"]) text += "\n  " + line.get<std::string>();
            if (!report["missing_on_target"].empty()) text += "\n  the rig lacks " + report["missing_on_target"].dump();
        }
    }
    call->ok(text, out);
}

Json playersJson(Project* project, SceneProject* scene, Entity entity) {
    Json out = Json::array();
    if (!scene || !scene->scene) return out;
    const Entity root = AnimationClipController::findPlayerRoot(scene->scene, entity);
    if (root == NULL_ENTITY) return out;
    const AnimationPlayerComponent& player = scene->scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<AnimationPlayerClip> entries = player.clips;
    const std::vector<std::string> names = anim::playerStateNames(entries);
    Json states = Json::array();
    Json bindings = Json::array();
    for (size_t i = 0; i < entries.size(); i++) {
        const std::string clip = entryClipJson(project, entries[i].clip);
        Json state = {{"name", i < names.size() ? names[i] : std::string()}, {"clip", clip}, {"wrap", anim::wrapName(entries[i].wrap)}};
        std::string error;
        std::shared_ptr<AnimationClipDocument> document = entries[i].clip.empty() ? nullptr : AnimationClipDocument::open(project, clip, &error);
        if (!document) {
            state["missing"] = true;
            state["error"] = error.empty() ? std::string("no clip file") : error;
        } else {
            state["length"] = r6(document->data().effectiveLength());
            state["clip_wrap"] = anim::wrapName(document->data().wrap);
            FixableClip fixable{document->isReadOnly() ? nullptr : &document->data(), document->absolutePath()};
            Json b = bindingsJson(scene, ClipValidation::validateInScene(project, scene, document->data(), root), &fixable);
            b["state"] = state["name"];
            b.erase("root");
            bindings.push_back(b);
        }
        states.push_back(state);
    }
    Json item = {{"entity", McpRefs::entityJson(scene, root)}, {"states", states}, {"default", player.defaultClip},
                 {"play_automatically", player.playAutomatically}, {"speed", player.speed}, {"enabled", player.enabled},
                 {"bindings", bindings}};
    if (root != entity) item["plays"] = McpRefs::entityJson(scene, entity);
    // layers (live weights and queues in play), the masks' coverage, the Animator it hands over to
    animator_tools::addPlayerLayers(project, scene, root, item);
    out.push_back(item);
    return out;
}

bool previewPlayer(const CallPtr& call, SceneProject* scene, Entity entity) {
    if (!scene || !scene->scene) return false;
    Scene* engineScene = scene->scene;
    const Entity root = AnimationClipController::findPlayerRoot(engineScene, entity);
    // an entity Animation keeps its own preview
    if (root == NULL_ENTITY || (engineScene->findComponent<AnimationComponent>(entity) && root != entity)) return false;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    if (scene->playState != ScenePlayState::STOPPED) {
        call->fail("preview_animation previews in edit mode only; the scene is playing (play mode runs the Animation Player itself).");
        return true;
    }
    const std::string action = args.contains("action") && args["action"].is_string() ? lower(args["action"].get<std::string>()) : "play";
    if (action != "play" && action != "pause" && action != "stop" && action != "seek") {
        call->fail("action must be play, pause, stop or seek.");
        return true;
    }
    if (action == "seek" && !(args.contains("time") && args["time"].is_number())) {
        call->fail("seek needs time (seconds).");
        return true;
    }
    AnimationClipController& controller = AnimationClipController::shared(project);
    std::string error;
    // the preview runs in the selected scene, on the selected GameObject's player (the window's rule)
    if (project->getSelectedSceneId() != scene->id) project->setSelectedSceneId(scene->id);
    project->setSelectedEntity(scene->id, root);
    if (!controller.bindRoot(scene->id, root, &error)) {
        call->fail("preview_animation: " + error);
        return true;
    }
    const std::vector<std::string> names = controller.getStateNames();
    if (names.empty()) {
        call->fail("\"" + engineScene->getEntityName(root) + "\"'s Animation Player has no clips: add one with set_properties "
                   "{animationplayer: {clips: [{clip: \"assets/anim/<name>.anim\"}]}}.");
        return true;
    }
    if (args.contains("state") && args["state"].is_string()) {
        const std::string state = args["state"].get<std::string>();
        if (controller.getStateName() != state || !controller.getDocument()) {
            if (!controller.openState(state, &error)) {
                std::string list;
                for (const std::string& name : names) list += (list.empty() ? "" : ", ") + ("\"" + name + "\"");
                call->fail("state: " + error + " (states: " + list + ").");
                return true;
            }
        }
    } else if (!controller.getDocument() || controller.getStateName().empty()) {
        // bound before this GameObject had its clips (the Animation window binds on selection):
        // open the default clip now
        const AnimationPlayerComponent& player = engineScene->getComponent<AnimationPlayerComponent>(root);
        const std::string state = std::find(names.begin(), names.end(), player.defaultClip) != names.end() ? player.defaultClip : names.front();
        controller.openState(state, nullptr);
    }
    if (!controller.getDocument()) {
        call->fail("\"" + engineScene->getEntityName(root) + "\"'s clip \"" + controller.getStateName() +
                   "\" can't be opened (missing file?): pass state, or fix clips with set_properties.");
        return true;
    }
    // over: a base layer state composed under the state of a higher layer (anim layers spec 7.5)
    if (args.contains("over") && action != "stop") {
        const std::string over = args["over"].is_string() ? args["over"].get<std::string>() : std::string();
        if (!over.empty()) {
            const std::vector<std::string> choices = controller.previewOverChoices();
            if (controller.getStateLayer() == 0) {
                call->fail("over composes a state of a layer above the base under a base state; \"" + controller.getStateName() +
                           "\" plays on the base layer.");
                return true;
            }
            if (std::find(choices.begin(), choices.end(), over) == choices.end()) {
                std::string list;
                for (const std::string& name : choices) list += (list.empty() ? "" : ", ") + ("\"" + name + "\"");
                call->fail("over: no base layer state \"" + over + "\" (" + (list.empty() ? std::string("the base layer has none") : "base states: " + list) + ").");
                return true;
            }
        }
        controller.setPreviewOver(over);
    } else if (action != "stop") {
        controller.setPreviewOver("");
    }
    if (action == "stop") {
        controller.stopPreview();
    } else if (action == "pause") {
        controller.pause();
    } else {
        if (!controller.isPreviewing() && !controller.startPreview(&error)) {
            call->fail("preview_animation: " + error);
            return true;
        }
        if (action == "seek") {
            controller.pause();
            controller.seek(std::max(0.0f, args["time"].get<float>()));
        } else {
            controller.play();
        }
    }
    const uint32_t sceneId = scene->id;
    auto report = [call, project, sceneId, root, action]() {
        AnimationClipController& c = AnimationClipController::shared(project);
        SceneProject* sp = project->getScene(sceneId);
        Json data = {{"animation", sp ? McpRefs::entityJson(sp, root) : Json(nullptr)}, {"action", action}, {"state", c.getStateName()},
                     {"clip", c.getClipPath()}, {"previewing", c.isPreviewing()}, {"playing", c.isPlaying()},
                     {"time", r6(c.getTime())}, {"length", r6(c.getLength())}};
        if (c.getStateLayer() > 0) {
            data["layer"] = c.getLayerName(c.getStateLayer());
            data["over"] = c.getPreviewOver();
        }
        Json bound = Json::array();
        const std::vector<AnimationClipController::BoundTransform> all = c.sampledBoundTransforms(std::numeric_limits<size_t>::max());
        for (size_t i = 0; i < all.size() && i < 8; i++) {
            const AnimationClipController::BoundTransform& t = all[i];
            Json item = {{"path", t.path}, {"position", McpValues::toJson(t.position)}, {"rotation", McpValues::toJson(t.rotation)},
                         {"scale", McpValues::toJson(t.scale)}};
            if (sp && t.entity != NULL_ENTITY) item["entity"] = McpRefs::entityJson(sp, t.entity);
            bound.push_back(item);
        }
        data["bound"] = bound;
        data["bound_total"] = all.size();
        // the clip's properties as the preview shows them (key space: sRGB colours, bools)
        Json properties = Json::array();
        const std::vector<AnimationClipController::BoundProperty> allProperties = c.sampledBoundProperties(std::numeric_limits<size_t>::max());
        for (size_t i = 0; i < allProperties.size() && i < 16; i++) {
            const AnimationClipController::BoundProperty& p = allProperties[i];
            Json item = {{"path", p.path}, {"component", p.component}, {"property", p.property},
                         {"value", propertyValueJson(p.kind, p.value)}};
            if (sp && p.entity != NULL_ENTITY) item["entity"] = McpRefs::entityJson(sp, p.entity);
            properties.push_back(item);
        }
        if (!properties.empty()) {
            data["properties"] = properties;
            data["properties_total"] = allProperties.size();
        }
        data["unresolved"] = c.getUnresolvedPaths();
        // the preview loops the clip whatever its wrap (the window's playback); Play mode follows wrap
        std::string text = "Clip preview: " + action + " \"" + c.getStateName() + "\" (" + c.getClipPath() + ") at " + fmt(c.getTime()) + " s" +
                           (c.isPreviewing() ? "" : ", not previewing (the scene shows its saved pose)") +
                           (c.isPlaying() ? ", playing (looped, whatever the wrap)" : "") + "; " +
                           (all.size() > bound.size() ? std::to_string(bound.size()) + " of " + std::to_string(all.size()) + " bound transforms"
                                                      : plural(bound.size(), "bound transform")) + " sampled";
        if (!c.getUnresolvedPaths().empty()) text += "; unresolved paths: " + std::to_string(c.getUnresolvedPaths().size());
        call->ok(text, data);
    };
    const int waitMs = std::clamp(args.contains("wait_ms") && args["wait_ms"].is_number() ? args["wait_ms"].get<int>() : 0, 0, 30000);
    if (waitMs > 0) {
        // the App's per-frame hook advances a playing preview (AnimationClipAppHooks::frame)
        auto start = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        call->waitUntil([start, waitMs]() {
            return std::chrono::steady_clock::now() - *start >= std::chrono::milliseconds(waitMs);
        }, waitMs + 5000, [report](bool) { report(); });
        return true;
    }
    call->afterFrames(1, report);
    return true;
}

Json avatarJson(Project* project, const fs::path& model) {
    const std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forModelFile(project, model);
    if (!info->sidecarError.empty()) return {{"error", info->sidecarError}};
    if (info->boneNames.empty()) return nullptr;
    Json mapped = Json::object();
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++) {
        if (!info->humanoid.bones[(size_t)b].empty()) mapped[pose::humanBoneName(pose::boneAt(b))] = info->humanoid.bones[(size_t)b];
    }
    Json missing = Json::array();
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++) {
        if (info->humanoid.bones[(size_t)b].empty()) missing.push_back(pose::humanBoneName(pose::boneAt(b)));
    }
    Json extra = Json::object();
    for (int x = 0; x < pose::HUMAN_EXTRA_BONE_COUNT; x++) {
        if (!info->extra.bones[(size_t)x].empty()) extra[pose::humanExtraBoneName(pose::extraAt(x))] = info->extra.bones[(size_t)x];
    }
    Json twist = Json::object();
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++) {
        if (info->overrides.twist[(size_t)b]) twist[pose::humanBoneName(pose::boneAt(b))] = r6(*info->overrides.twist[(size_t)b]);
    }
    Json out = {{"animation_type", pose::animationTypeName(info->overrides.animationType.value_or(pose::AnimationType::Generic))},
                {"humanoid", info->rig != nullptr}, {"mapped", mapped}, {"missing", missing}, {"extra_mapped", extra.size()},
                {"extra_bones", extra}, {"twist", twist}, {"human_scale", info->rig ? Json(r6(info->rig->humanScale())) : Json(nullptr)},
                {"notes", info->notes}};
    if (!info->error.empty()) out["error"] = info->error;
    if (!info->extraError.empty()) out["extra_error"] = info->extraError;
    return out;
}

bool clipAssetInfo(Project* project, const fs::path& file, Json& data, std::string& text) {
    const std::string rel = projectRel(project, file);
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, rel, &error);
    if (!document) {
        data["error"] = error;
        text += " (not a readable clip: " + error + ")";
        return false;
    }
    const ClipData& clip = document->data();
    Json tracks = Json::array();
    for (const ClipTrackData& track : clip.tracks) {
        if (isHumanoidChannel(track.channel)) {
            // humanoid tracks name a bone, not a path
            const char* bone = anim::humanoidTrackBoneName(track.channel, track.humanBone);
            tracks.push_back({{"bone", bone ? bone : ""}, {"channel", anim::channelName(track.channel)}, {"key_count", track.keys.size()}});
            continue;
        }
        tracks.push_back({{"path", track.path}, {"channel", anim::channelName(track.channel)}, {"key_count", track.keys.size()}});
    }
    const std::vector<ClipUser> users = findClipUsers(project, rel);
    data["clip"] = {{"version", !clip.audio.empty() ? 3 : clip.isHumanoid() ? 2 : 1}, {"guid", clip.guid}, {"length", r6(clip.effectiveLength())},
                    {"wrap", anim::wrapName(clip.wrap)}, {"frame_rate", r6(clip.frameRate)}, {"tracks", tracks},
                    {"events", eventsJson(clip.events)}, {"audio", cuesJson(project, clip.audio)}, {"warnings", warningsJson(document->loadWarnings())},
                    {"users", usersJson(project, users)}};
    if (users.size() > 50) data["clip"]["users_total"] = users.size();
    if (!clip.previewModel.empty()) data["clip"]["preview_model"] = projectPathOf(project, clip.previewModel);
    text += ", " + plural(clip.tracks.size(), "track") + ", " + plural(clip.events.size(), "event") + ", " + fmt(clip.effectiveLength()) + " s, " +
            anim::wrapName(clip.wrap) + ", " + plural(users.size(), "user") +
            (document->loadWarnings().empty() ? std::string() : ", " + plural(document->loadWarnings().size(), "warning")) +
            " (read it with get_animation {clip})";
    return true;
}

bool checkClipText(const std::string& text, std::vector<std::string>& warnings, std::string& error) {
    const anim::ClipParseResult parsed = anim::parseClip(text);
    if (!parsed.ok()) {
        error = parsed.error.toString();
        return false;
    }
    for (const ClipWarning& warning : parsed.warnings) warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    return true;
}

} // namespace jasperengine::editor::mcp::clip_tools
