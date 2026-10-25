// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The clip-file mode of edit_animation / get_animation / preview_animation (animation clips
// spec 5, 9.R4/R28/R29). A clip is an .anim asset edited through AnimationClipDocument (one
// project undo step per call, all or nothing; create {from: <entity Animation>} is one scene step
// with the Animation Player it adds), its tracks bound by path relative to the GameObject that
// plays it. The key grammar is edit_animation's (AnimationKeys.h); results report the bindings
// against a root (ClipValidation), the components sharing the file and the undo scope.
#include "AnimationClipTools.h"
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
#include "animation/ClipBinding.h"
#include "animation/ClipConverters.h"
#include "animation/ClipValidation.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/Transform.h"
#include "subsystem/ActionSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include "command/type/ClipSceneActions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
        {"morph", ClipChannel::Morph}, {"lattice", ClipChannel::Lattice}};
    return table;
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
bool resolveClipArg(Project* project, const Json& value, const std::string& what, bool allowSubClip,
                    std::string& relative, fs::path& absolute, bool& subClip, std::string& error) {
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
    if (!subClip && lower(fs::u8path(file).extension().string()) != ".anim") {
        error = what + ": \"" + given + "\" must end in .anim" +
                std::string(allowSubClip ? " (or name a model clip: \"assets/models/hero.gltf#Walk\")" : "") + ".";
        return false;
    }
    if (!McpRefs::resolveProjectPath(project, file, absolute, error, subClip)) {
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

Json keyJson(const ClipTrackData& track, const ClipKey& key) {
    Json item = {{"time", r6(key.time)}};
    if (track.channel == ClipChannel::Lattice) item["points"] = valueJson(track, key.value);
    else item["value"] = valueJson(track, key.value);
    if (key.ease != EaseType::LINEAR) item["ease"] = anim::easeName(key.ease);
    if (key.hasTangents() && track.channel != ClipChannel::Lattice) {
        item["in"] = floatsJson(key.inTangent, 0, key.inTangent.size());
        item["out"] = floatsJson(key.outTangent, 0, key.outTangent.size());
    }
    return item;
}

Json trackJson(const ClipTrackData& track, size_t index, size_t maxKeys) {
    Json item = {{"index", index}, {"path", track.path}, {"channel", anim::channelName(track.channel)}};
    if (track.channel == ClipChannel::Property) {
        item["component"] = track.component;
        item["property"] = track.property;
        item["sampled"] = false;
        return item;
    }
    Json keys = Json::array();
    for (size_t k = 0; k < track.keys.size() && k < maxKeys; k++) keys.push_back(keyJson(track, track.keys[k]));
    item["key_count"] = track.keys.size();
    item["keys"] = keys;
    item["loop"] = track.loop;
    item["relative"] = track.relative;
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
    if (!data.previewModel.empty()) out["preview_model"] = projectPathOf(project, data.previewModel);
    if (!data.source.empty()) out["source"] = {{"kind", data.source.kind}, {"path", data.source.path}, {"name", data.source.name}};
    return out;
}

// ------------------------------------------------------------------ users and bindings

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
        case TrackBindingStatus::Unchecked: return "unchecked";
    }
    return "unchecked";
}

// {root, resolved, total, missing: [{path, channel, status, nearest, message}], ...}
Json bindingsJson(SceneProject* scene, const ClipValidationReport& report) {
    Json missing = Json::array();
    Json ambiguous = Json::array();
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
        if (!track.badPoints.empty()) item["points"] = track.badPoints;
        if (!track.message.empty()) item["message"] = track.message;
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
        out["legacy_fix"] = "add the root's Animation Player (add_component animationplayer; one it already has: remove_component, "
                            "then add_component with its clips): its models reload with their node hierarchy in that scene step, then the paths bind";
    }
    if (unchecked) out["unchecked"] = true;
    if (!report.warnings.empty()) out["warnings"] = warningsJson(report.warnings);
    return out;
}

std::string bindingsText(const Json& bindings) {
    std::string text = "bindings on \"" + (bindings["root"].is_object() ? bindings["root"].value("name", std::string()) : std::string()) +
                       "\": " + std::to_string(bindings["resolved"].get<size_t>()) + "/" + std::to_string(bindings["total"].get<size_t>()) +
                       " track(s) bound";
    if (bindings.contains("unchecked")) return text + " (not checked)";
    for (const Json& m : bindings["missing"]) {
        text += "\n  " + m["path"].get<std::string>() + " : " + m["channel"].get<std::string>() + " " + m["status"].get<std::string>();
        if (!m["nearest"].empty()) text += " (did you mean \"" + m["nearest"][0].get<std::string>() + "\"?)";
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
    return text;
}

// The binding report against `root`, else against the first user of the clip in an open scene
Json bindingsFor(Project* project, SceneProject* rootScene, Entity root, const ClipData& data, const std::vector<ClipUser>& users) {
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
    return bindingsJson(scene, ClipValidation::validateInScene(project, scene, data, target));
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
        if (wrap != data.wrap) ctx.changes.push_back(std::string("wrap ") + anim::wrapName(data.wrap) + " -> " + anim::wrapName(wrap));
        data.wrap = wrap;
    }
    if (has(args, "frame_rate")) {
        float rate = 0.0f;
        if (!readNumber(args["frame_rate"], "frame_rate", rate, error)) return false;
        if (rate < anim::ANIM_MIN_FRAME_RATE || rate > anim::ANIM_MAX_FRAME_RATE) {
            error = "frame_rate must be 1 to 240 samples per second (keys stay in seconds; it is the snapping grid).";
            return false;
        }
        if (rate != data.frameRate) ctx.changes.push_back("frame_rate " + fmt(data.frameRate) + " -> " + fmt(rate));
        data.frameRate = rate;
    }
    if (has(args, "length")) {
        float length = 0.0f;
        if (!readNumber(args["length"], "length", length, error)) return false;
        if (length < 0.0f) length = -1.0f;
        if (length != data.length) ctx.changes.push_back(length < 0.0f ? std::string("length automatic (last key or event)") : "length " + fmt(length) + " s");
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

bool applyTrack(EditContext& ctx, const Json& item, size_t t, ClipData& data, std::string& error) {
    const std::string where = "tracks[" + std::to_string(t) + "]";
    if (!item.is_object()) {
        error = where + " must be an object {path, channel, keys, mode, loop, relative}.";
        return false;
    }
    if (has(item, "start_time")) {
        error = where + ".start_time: clip tracks start at their first key: shift the keys' times.";
        return false;
    }
    if (has(item, "name")) {
        error = where + ".name: a clip track is named by its path and channel.";
        return false;
    }
    // the binding path
    std::string path;
    if (has(item, "path") && has(item, "target")) {
        error = where + ": give path or target (with root), not both.";
        return false;
    }
    if (has(item, "path")) {
        if (!item["path"].is_string()) {
            error = where + ".path must be a string: names from the root joined by '/' (\"\" = the root itself), e.g. \"Hips/Spine\".";
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
        error = where + ".path is required: names from the root joined by '/' (\"\" = the root itself), e.g. \"Hips/Spine\".";
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
    if (channelName == "property") {
        error = where + ".channel: property tracks are reserved (not supported yet): use position, rotation, scale, morph or lattice.";
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
        error = where + ".channel \"" + channelName + "\" is not valid. Use one of: position, rotation, scale, morph, lattice.";
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
        std::vector<ClipKey> newKeys;
        for (const Key& key : keys) {
            ClipKey clipKey;
            clipKey.time = key.time;
            clipKey.ease = key.ease;
            clipKey.value = keyValue(channel, key);
            newKeys.push_back(clipKey);
        }
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

bool isLuaIdentifier(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool applyEvents(EditContext& ctx, const Json& args, ClipData& data, std::string& error) {
    if (!has(args, "events")) {
        if (has(args, "events_mode")) {
            error = "events_mode needs events: [{time, function, args}].";
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
        if (!item.contains("function") || !item["function"].is_string() || !isLuaIdentifier(item["function"].get<std::string>())) {
            error = where + ".function must be a Lua identifier: the script method it calls (e.g. \"onFootstep\").";
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

bool applyEdits(EditContext& ctx, const Json& args, ClipData& data, std::string& error) {
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
    return applyEvents(ctx, args, data, error);
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
    std::vector<ClipWarning> warnings = report.warnings;
    ClipData normalized = data;
    anim::normalize(normalized, &warnings);

    std::vector<Command*> commands;
    ClipActionResult result;
    ClipSceneActions::appendPlayerCommands(project, scene->id, root, {ClipConverters::playerEntry(project, relative)}, commands, result);
    const std::string content = ClipSceneActions::newClipText(project, data);
    if (!ClipSceneActions::runStep(project, scene->id, {{relative, content}}, commands, root, result, error)) {
        call->fail("create.from: " + error);
        return false;
    }
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, relative, &error);
    if (!document) {
        call->fail("create.from: the clip was written but can't be read back: " + error);
        return false;
    }
    (void)absolute;
    out = clipJson(project, document->path(), document->data(), false, 20);
    out["warnings"] = warningsJson(warnings);
    out["player"] = McpRefs::entityJson(scene, root);
    out["added_player"] = result.addedPlayer;
    text = " (one scene undo step: the file and the Animation Player of \"" + scene->scene->getEntityName(root) + "\"; undo scope scene)";
    return true;
}

} // namespace

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
    if (!resolveClipArg(project, args["clip"], "clip", true, relative, absolute, subClip, error)) {
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
    const bool creating = has(args, "create");
    std::error_code ec;
    const bool exists = fs::exists(absolute, ec);
    Json from;
    if (creating) {
        const Json& create = args["create"];
        if (create.is_boolean() && create.get<bool>()) {
            // create: true reads as create: {}
        } else if (!create.is_object()) {
            call->fail("create must be an object {from?}: from an entity Animation GameObject (Convert) or a model clip \"assets/models/hero.gltf#Walk\" (Extract); {} makes an empty clip.");
            return;
        } else {
            for (auto it = create.begin(); it != create.end(); ++it) {
                if (it.key() != "from") {
                    call->fail("create: unknown key \"" + it.key() + "\" (create is {from?}).");
                    return;
                }
            }
            if (create.contains("from")) from = create["from"];
        }
        if (exists) {
            call->fail("create: " + relative + " already exists; leave out create to edit it.");
            return;
        }
        if (!fs::is_directory(absolute.parent_path(), ec)) {
            call->fail("create: the folder " + projectRel(project, absolute.parent_path()) + " does not exist; create it with create_folder.");
            return;
        }
    }
    const size_t maxKeys = static_cast<size_t>(std::max(1, args.contains("max_keys") && args["max_keys"].is_number() ? args["max_keys"].get<int>() : 20));

    std::shared_ptr<AnimationClipDocument> document;
    ClipData data;
    std::vector<ClipWarning> sourceWarnings;
    if (creating && !from.is_null()) {
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
            ctx.changes.push_back("extracted " + modelRelative + " (" + std::to_string(data.tracks.size()) + " track(s))");
        } else {
            std::string text;
            Json out;
            if (!createFromEntity(call, ctx, from, relative, absolute, text, out)) return;
            out["created"] = true;
            out["changes"] = ctx.changes;
            const std::vector<ClipUser> users = findClipUsers(project, relative);
            out["users"] = users.size();
            const Json bindings = bindingsFor(project, ctx.scene, ctx.root, AnimationClipDocument::open(project, relative, nullptr)->data(), users);
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
    if (creating) {
        document = AnimationClipDocument::create(project, relative, data, from.is_null() ? "Create Animation Clip" : "Extract to .anim", &error);
        if (!document) {
            call->fail("create: " + error);
            return;
        }
    } else if (!document->commit(data, "Edit Animation Clip", "", &error)) {
        call->fail(error);
        return;
    }
    const bool changed = creating || document->revision() != before;

    const std::vector<ClipUser> users = findClipUsers(project, relative);
    Json out = clipJson(project, document->path(), document->data(), false, maxKeys);
    out["created"] = creating;
    out["changes"] = ctx.changes;
    Json warningList = warningsJson(warnings);
    if (!insideAssets(project, absolute)) {
        warningList.push_back(relative + " is outside the assets folder: an Animation Player in a game cannot load it");
    }
    out["warnings"] = warningList;
    out["users"] = users.size();
    const Json bindings = bindingsFor(project, ctx.scene, ctx.root, document->data(), users);
    if (!bindings.is_null()) out["bindings"] = bindings;

    std::string text = document->path() + ": " + (creating ? "created, " : "") + plural(document->data().tracks.size(), "track") + ", " +
                       fmt(document->data().effectiveLength()) + " s, " + anim::wrapName(document->data().wrap);
    text += users.empty() ? std::string("; no Animation Player uses it yet (set_properties clips)") :
                            "; shared by " + plural(users.size(), "component") + " (the edit changes every one)";
    text += changed ? " (one project undo step: undo scope project)" : " (no change: nothing written)";
    for (const std::string& line : ctx.changes) text += "\n" + line;
    for (const Json& warning : warningList) text += "\nwarning: " + warning.get<std::string>();
    if (!bindings.is_null()) text += "\n" + bindingsText(bindings);
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
    if (!document->isReadOnly()) out["guid"] = data.guid;

    // sampled values at the given seconds, through the runtime sampler (headless)
    std::string sampled;
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
        for (float t : at) {
            std::vector<anim::TrackSample> samples;
            ActionSystem::sampleClip(*clip, anim::wrapClipTime(t, clip->getLength(), clip->getWrap()), samples);
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
                    default:
                        values.push_back(nullptr);
                }
            }
            tracks.push_back({{"path", track.path}, {"channel", anim::channelName(track.channel)}, {"relative", track.relative}, {"values", values}});
        }
        Json timesOut = Json::array();
        for (float t : at) timesOut.push_back(r6(t));
        out["samples"] = {{"times", timesOut}, {"tracks", tracks}};
        sampled = "; sampled at " + std::to_string(at.size()) + " time(s) (samples)";
    }

    const Json bindings = root != NULL_ENTITY ? bindingsJson(scene, ClipValidation::validateInScene(project, scene, data, root)) : Json(nullptr);
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
            Json b = bindingsJson(scene, ClipValidation::validateInScene(project, scene, document->data(), root));
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
    } else if (!controller.getDocument()) {
        call->fail("\"" + engineScene->getEntityName(root) + "\"'s clip \"" + controller.getStateName() +
                   "\" can't be opened (missing file?): pass state, or fix clips with set_properties.");
        return true;
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
        Json bound = Json::array();
        for (const AnimationClipController::BoundTransform& t : c.sampledBoundTransforms(8)) {
            bound.push_back({{"path", t.path}, {"position", McpValues::toJson(t.position)}, {"rotation", McpValues::toJson(t.rotation)},
                             {"scale", McpValues::toJson(t.scale)}});
        }
        data["bound"] = bound;
        data["unresolved"] = c.getUnresolvedPaths();
        std::string text = "Clip preview: " + action + " \"" + c.getStateName() + "\" (" + c.getClipPath() + ") at " + fmt(c.getTime()) + " s" +
                           (c.isPreviewing() ? "" : ", not previewing (the scene shows its saved pose)") + "; " +
                           plural(bound.size(), "bound transform") + " sampled";
        if (!c.getUnresolvedPaths().empty()) text += "; unresolved paths: " + std::to_string(c.getUnresolvedPaths().size());
        call->ok(text, data);
    };
    const int waitMs = std::clamp(args.contains("wait_ms") && args["wait_ms"].is_number() ? args["wait_ms"].get<int>() : 0, 0, 30000);
    if (waitMs > 0 && action == "play") {
        // the preview advances each frame for the wait (the Animation window drives it while it
        // shows; this does it only when nothing else moved the playhead since the last frame)
        auto start = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto last = std::make_shared<std::chrono::steady_clock::time_point>(*start);
        auto lastTime = std::make_shared<float>(controller.getTime());
        call->waitUntil([project, start, last, lastTime, waitMs]() {
            AnimationClipController& c = AnimationClipController::shared(project);
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - *last).count();
            *last = now;
            if (c.isPlaying() && c.getTime() == *lastTime) c.update(dt);
            *lastTime = c.getTime();
            return now - *start >= std::chrono::milliseconds(waitMs);
        }, waitMs + 5000, [report](bool) { report(); });
        return true;
    }
    call->afterFrames(1, report);
    return true;
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
        tracks.push_back({{"path", track.path}, {"channel", anim::channelName(track.channel)}, {"keys", track.keys.size()}});
    }
    const std::vector<ClipUser> users = findClipUsers(project, rel);
    data["clip"] = {{"version", anim::ANIM_VERSION}, {"guid", clip.guid}, {"length", r6(clip.effectiveLength())},
                    {"wrap", anim::wrapName(clip.wrap)}, {"frame_rate", r6(clip.frameRate)}, {"tracks", tracks},
                    {"events", eventsJson(clip.events)}, {"warnings", warningsJson(document->loadWarnings())},
                    {"users", usersJson(project, users)}};
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
