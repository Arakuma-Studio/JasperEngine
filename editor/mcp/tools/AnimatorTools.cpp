// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_animator, get_animation {mask | controller} and the layer / controller parts of
// get_animation {entity} (anim layers spec 10, 11.5). The op grammar is in
// docs/mcp/guide/animation.md ("Animator Controllers").

#include "AnimatorTools.h"
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "Scene.h"
#include "animation/AnimAssetDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorEdits.h"
#include "animation/AvatarMask.h"
#include "animation/ClipBinding.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "window/InspectorAnimLayers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp::animator_tools {

namespace {

constexpr size_t MAX_COVERAGE = 200;

double r6(float value) {
    return std::round((double)value * 1e6) / 1e6;
}

Json warningsJson(const std::vector<ClipWarning>& warnings) {
    Json out = Json::array();
    for (const ClipWarning& warning : warnings) out.push_back({{"path", warning.path}, {"message", warning.message}});
    return out;
}

std::string projectRel(Project* project, const fs::path& absolute) {
    return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
}

std::string assetJson(Project* project, const std::string& stored) {
    return stored.empty() ? std::string() : McpValues::assetPathToJson(project, stored);
}

// a controller's clip resolved for validation through its document (.anim or a model clip)
anim::ControllerClipInfo clipInfo(Project* project, const std::string& clip) {
    anim::ControllerClipInfo info;
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, clip, &error);
    info.loads = document != nullptr;
    info.error = error;
    if (document) info.loops = document->data().wrap == ClipWrapMode::Loop || document->data().wrap == ClipWrapMode::PingPong;
    return info;
}

std::vector<ClipWarning> validate(Project* project, const anim::ControllerData& data) {
    return anim::validateController(data, [project](const std::string& clip) { return clipInfo(project, clip); });
}

Json usersJson(Project* project, const std::vector<AnimAssetUser>& users) {
    Json out = Json::array();
    for (const AnimAssetUser& user : users) {
        Json item = Json::object();
        if (!user.controllerFile.empty()) {
            item["controller"] = user.controllerFile;
        } else if (user.prefabAsset) {
            item["prefab"] = user.prefabPath;
        } else if (SceneProject* scene = project->getScene(user.sceneId)) {
            item["scene"] = scene->id;
            if (scene->scene && scene->scene->isEntityCreated(user.entity)) item["entity"] = McpRefs::entityJson(scene, user.entity);
        }
        if (user.layer >= 0) item["layer"] = user.layer;
        out.push_back(item);
    }
    return out;
}

// ---- argument helpers ----

bool fail(std::string& error, const std::string& where, const std::string& message) {
    error = where + ": " + message;
    return false;
}

std::string field(const std::string& op, const char* name) {
    return op + "." + name;
}

bool readString(const Json& op, const char* name, std::string& out, const std::string& where, std::string& error, bool required = false) {
    if (!op.contains(name)) {
        if (required) return fail(error, field(where, name), "is required");
        return true;
    }
    if (!op[name].is_string()) return fail(error, field(where, name), "expected a string");
    out = op[name].get<std::string>();
    return true;
}

// a finite number (as a float) in [min, max]: the ranges the controller's reader clamps to
// (anim::normalizeController) are refused here, as set_properties refuses
bool readNumber(const Json& op, const char* name, float& out, const std::string& where, std::string& error,
                float min = -std::numeric_limits<float>::max(), float max = std::numeric_limits<float>::max()) {
    if (!op.contains(name)) return true;
    if (!op[name].is_number() || !std::isfinite(op[name].get<double>()) || !std::isfinite(op[name].get<float>())) {
        return fail(error, field(where, name), "expected a number");
    }
    const float value = op[name].get<float>();
    if (value < min || value > max) {
        const bool unit = min == 0.0f && max == 1.0f;
        return fail(error, field(where, name), unit ? "must be from 0 to 1" : min == 0.0f ? "must be 0 or more" : "is out of range");
    }
    out = value;
    return true;
}

// the keys an object may have: the others are refused with the allowed ones (a misspelled key
// would else be ignored)
bool checkKeys(const Json& object, const std::vector<std::string>& allowed, const std::string& where, std::string& error) {
    if (!object.is_object()) return true;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end()) continue;
        std::string list;
        for (const std::string& key : allowed) list += (list.empty() ? "" : ", ") + key;
        return fail(error, field(where, it.key().c_str()), "unknown key (allowed: " + list + ")");
    }
    return true;
}

std::vector<std::string> keysOf(std::initializer_list<std::vector<std::string>> groups) {
    std::vector<std::string> out;
    for (const std::vector<std::string>& group : groups) out.insert(out.end(), group.begin(), group.end());
    return out;
}

const std::vector<std::string> STATE_KEYS = {"clip", "blend_tree", "speed", "speed_parameter", "wrap", "cycle_offset", "mirror",
                                             "foot_ik", "retarget", "position"};
const std::vector<std::string> TRANSITION_KEYS = {"conditions", "has_exit_time", "exit_time", "duration", "fixed_duration", "offset",
                                                  "interruption", "can_transition_to_self"};
const std::vector<std::string> LAYER_KEYS = {"weight", "blend", "mask"};

// each op's keys
const std::vector<std::string>* opKeys(const std::string& kind) {
    static const std::map<std::string, std::vector<std::string>> keys = {
        {"add_parameter", {"op", "name", "type", "default"}},
        {"update_parameter", {"op", "name", "type", "default", "rename"}},
        {"remove_parameter", {"op", "name"}},
        {"add_layer", keysOf({{"op", "name"}, LAYER_KEYS})},
        {"update_layer", keysOf({{"op", "layer", "name"}, LAYER_KEYS, {"any_state_position"}})},
        {"remove_layer", {"op", "layer"}},
        {"move_layer", {"op", "layer", "to"}},
        {"add_state", keysOf({{"op", "layer", "name"}, STATE_KEYS})},
        {"update_state", keysOf({{"op", "layer", "state", "name", "rename"}, STATE_KEYS})},
        {"remove_state", {"op", "layer", "state", "name"}},
        {"set_default_state", {"op", "layer", "state", "name"}},
        {"add_transition", keysOf({{"op", "layer", "from", "to"}, TRANSITION_KEYS})},
        {"update_transition", keysOf({{"op", "layer", "index", "from", "to"}, TRANSITION_KEYS})},
        {"remove_transition", {"op", "layer", "index"}},
    };
    const auto found = keys.find(kind);
    return found == keys.end() ? nullptr : &found->second;
}

bool readBool(const Json& op, const char* name, bool& out, const std::string& where, std::string& error) {
    if (!op.contains(name)) return true;
    if (!op[name].is_boolean()) return fail(error, field(where, name), "expected true or false");
    out = op[name].get<bool>();
    return true;
}

bool readPosition(const Json& op, bool& has, float* out, const std::string& where, std::string& error) {
    if (!op.contains("position")) return true;
    const Json& value = op["position"];
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number() ||
        !std::isfinite(value[0].get<float>()) || !std::isfinite(value[1].get<float>())) {
        return fail(error, field(where, "position"), "expected [x, y] (graph pixels)");
    }
    has = true;
    out[0] = value[0].get<float>();
    out[1] = value[1].get<float>();
    return true;
}

// `layer`: an index or a name
bool findLayer(const anim::ControllerData& data, const Json& op, const std::string& where, int& out, std::string& error) {
    if (!op.contains("layer")) {
        out = 0;
        return true;
    }
    const Json& value = op["layer"];
    if (value.is_number_integer()) {
        out = value.get<int>();
        if (out >= 0 && (size_t)out < data.layers.size()) return true;
    } else if (value.is_string()) {
        out = data.findLayer(value.get<std::string>());
        if (out >= 0) return true;
    } else {
        return fail(error, field(where, "layer"), "expected a layer index or name");
    }
    std::string names;
    for (const anim::ControllerLayer& layer : data.layers) names += (names.empty() ? "" : ", ") + ("\"" + layer.name + "\"");
    return fail(error, field(where, "layer"), "no layer " + value.dump() + " (layers: " + names + ")");
}

bool findState(const anim::ControllerLayer& layer, const std::string& name, const std::string& where, int& out, std::string& error) {
    out = layer.findState(name);
    if (out >= 0) return true;
    std::string names;
    for (const anim::ControllerState& state : layer.states) names += (names.empty() ? "" : ", ") + ("\"" + state.name + "\"");
    return fail(error, where, "no state \"" + name + "\" in layer \"" + layer.name + "\"" + (names.empty() ? " (it has none)" : " (states: " + names + ")"));
}

bool readClip(Project* project, const Json& op, const char* name, std::string& clip, std::string& guid, const std::string& where, std::string& error) {
    if (!op.contains(name)) return true;
    if (op[name].is_null()) {
        clip.clear();
        guid.clear();
        return true;
    }
    if (!op[name].is_string()) return fail(error, field(where, name), "expected a clip path (assets/anim/walk.anim or assets/models/hero.gltf#Walk)");
    std::string why;
    if (!McpValues::clipReferenceFromJson(project, op[name].get<std::string>(), clip, guid, why)) return fail(error, field(where, name), why);
    return true;
}

bool readBlendTree(Project* project, const Json& value, anim::BlendTree1D& tree, const std::string& where, std::string& error) {
    if (!value.is_object()) return fail(error, where, "expected {parameter, children: [{clip, threshold, speed?}], automate_thresholds?}");
    if (!checkKeys(value, {"parameter", "children", "automate_thresholds"}, where, error)) return false;
    tree = anim::BlendTree1D();
    if (!readString(value, "parameter", tree.parameter, where, error, true)) return false;
    if (!readBool(value, "automate_thresholds", tree.automateThresholds, where, error)) return false;
    if (!value.contains("children") || !value["children"].is_array()) return fail(error, field(where, "children"), "expected an array of {clip, threshold}");
    for (size_t c = 0; c < value["children"].size(); c++) {
        const Json& item = value["children"][c];
        const std::string at = where + ".children[" + std::to_string(c) + "]";
        if (!item.is_object()) return fail(error, at, "expected {clip, threshold, speed?}");
        if (!checkKeys(item, {"clip", "threshold", "speed", "mirror"}, at, error)) return false;
        anim::BlendTreeChild child;
        if (!item.contains("clip")) return fail(error, field(at, "clip"), "is required");
        if (!readClip(project, item, "clip", child.clip, child.clipGuid, at, error)) return false;
        if (!readNumber(item, "threshold", child.threshold, at, error)) return false;
        if (!readNumber(item, "speed", child.speed, at, error)) return false;
        if (!readBool(item, "mirror", child.mirror, at, error)) return false;
        tree.children.push_back(child);
    }
    return true;
}

// the state fields add_state / update_state take
bool readState(Project* project, const Json& op, anim::ControllerState& state, const std::string& where, std::string& error) {
    if (op.contains("clip") && op.contains("blend_tree")) return fail(error, where, "give clip or blend_tree, not both");
    if (op.contains("clip")) {
        // a clip replaces the blend tree; null or "" leaves the state empty (no clip, no tree)
        const bool clear = op["clip"].is_null() || (op["clip"].is_string() && op["clip"].get<std::string>().empty());
        if (clear) {
            state.clip.clear();
            state.clipGuid.clear();
        } else if (!readClip(project, op, "clip", state.clip, state.clipGuid, where, error)) {
            return false;
        }
        state.hasBlendTree = false;
        state.blendTree = anim::BlendTree1D();
    }
    if (op.contains("blend_tree")) {
        if (op["blend_tree"].is_null()) {
            state.hasBlendTree = false;
            state.blendTree = anim::BlendTree1D();
        } else {
            if (!readBlendTree(project, op["blend_tree"], state.blendTree, field(where, "blend_tree"), error)) return false;
            state.hasBlendTree = true;
            state.clip.clear();
            state.clipGuid.clear();
        }
    }
    if (!readNumber(op, "speed", state.speed, where, error)) return false;
    if (!readString(op, "speed_parameter", state.speedParameter, where, error)) return false;
    if (op.contains("wrap")) {
        if (!op["wrap"].is_string() || !anim::wrapFromName(op["wrap"].get<std::string>(), state.wrap)) {
            return fail(error, field(where, "wrap"), "expected default, once, loop, ping_pong or clamp_forever");
        }
    }
    if (!readNumber(op, "cycle_offset", state.cycleOffset, where, error, 0.0f, 1.0f)) return false;
    if (!readBool(op, "mirror", state.mirror, where, error)) return false;
    if (!readBool(op, "foot_ik", state.footIK, where, error)) return false;
    if (!readBool(op, "retarget", state.retarget, where, error)) return false;
    return readPosition(op, state.hasPosition, state.position, where, error);
}

bool readConditions(const Json& value, std::vector<anim::ControllerCondition>& out, const std::string& where, std::string& error) {
    if (!value.is_array()) return fail(error, where, "expected an array of {parameter, mode, threshold?}");
    out.clear();
    for (size_t c = 0; c < value.size(); c++) {
        const Json& item = value[c];
        const std::string at = where + "[" + std::to_string(c) + "]";
        if (!item.is_object()) return fail(error, at, "expected {parameter, mode, threshold?}");
        if (!checkKeys(item, {"parameter", "mode", "threshold"}, at, error)) return false;
        anim::ControllerCondition condition;
        if (!readString(item, "parameter", condition.parameter, at, error, true)) return false;
        std::string mode;
        if (!readString(item, "mode", mode, at, error, true)) return false;
        if (!anim::conditionModeFromName(mode, condition.mode)) {
            return fail(error, field(at, "mode"), "expected greater, less (float, int), equals, not_equal (int), if, if_not (bool; a trigger uses if)");
        }
        if (!readNumber(item, "threshold", condition.threshold, at, error)) return false;
        out.push_back(condition);
    }
    return true;
}

bool readTransition(const Json& op, anim::ControllerTransition& transition, const std::string& where, std::string& error) {
    if (op.contains("conditions") && !readConditions(op["conditions"], transition.conditions, field(where, "conditions"), error)) return false;
    if (!readBool(op, "has_exit_time", transition.hasExitTime, where, error)) return false;
    if (!readNumber(op, "exit_time", transition.exitTime, where, error, 0.0f)) return false;
    if (!readNumber(op, "duration", transition.duration, where, error, 0.0f)) return false;
    if (!readBool(op, "fixed_duration", transition.fixedDuration, where, error)) return false;
    if (!readNumber(op, "offset", transition.offset, where, error, 0.0f, 1.0f)) return false;
    if (op.contains("interruption")) {
        if (!op["interruption"].is_string() || !anim::interruptionFromName(op["interruption"].get<std::string>(), transition.interruption)) {
            return fail(error, field(where, "interruption"), "expected none, current_state or next_state");
        }
    }
    return readBool(op, "can_transition_to_self", transition.canTransitionToSelf, where, error);
}

bool readLayer(Project* project, const Json& op, anim::ControllerLayer& layer, const std::string& where, std::string& error) {
    if (!readNumber(op, "weight", layer.weight, where, error, 0.0f, 1.0f)) return false;
    if (op.contains("blend")) {
        if (!op["blend"].is_string() || !anim::layerBlendFromName(op["blend"].get<std::string>(), layer.blend)) {
            return fail(error, field(where, "blend"), "expected override or additive");
        }
    }
    if (op.contains("mask")) {
        if (op["mask"].is_null() || (op["mask"].is_string() && op["mask"].get<std::string>().empty())) {
            layer.mask.clear();
            layer.maskGuid.clear();
        } else if (!op["mask"].is_string()) {
            return fail(error, field(where, "mask"), "expected a .mask path");
        } else {
            std::string why;
            if (!McpValues::animAssetReferenceFromJson(project, op["mask"].get<std::string>(), true, layer.mask, layer.maskGuid, why)) {
                return fail(error, field(where, "mask"), why);
            }
        }
    }
    return true;
}

// the next free grid cell of a layer (new states without a position)
void gridPosition(const anim::ControllerLayer& layer, anim::ControllerState& state) {
    if (state.hasPosition) return;
    std::set<std::pair<int, int>> taken;
    for (const anim::ControllerState& other : layer.states) {
        if (other.hasPosition) taken.insert({(int)std::lround(other.position[0]), (int)std::lround(other.position[1])});
    }
    for (int k = 0;; k++) {
        const int x = 260 + 220 * (k % 4), y = 120 + 100 * (k / 4);
        if (!taken.count({x, y})) {
            state.hasPosition = true;
            state.position[0] = (float)x;
            state.position[1] = (float)y;
            return;
        }
    }
}

bool readParameterDefault(const Json& value, anim::ControllerParameterType type, float& out, const std::string& where, std::string& error) {
    const bool flag = type == anim::ControllerParameterType::Bool || type == anim::ControllerParameterType::Trigger;
    if (value.is_boolean()) {
        if (!flag) return fail(error, where, "expected a number (a " + std::string(anim::parameterTypeName(type)) + " parameter)");
        out = value.get<bool>() ? 1.0f : 0.0f;
        return true;
    }
    if (flag) return fail(error, where, "expected true or false");
    if (!value.is_number() || !std::isfinite(value.get<double>()) || !std::isfinite(value.get<float>())) return fail(error, where, "expected a number");
    const double number = value.get<double>();
    if (type == anim::ControllerParameterType::Int && (number != std::floor(number) || std::fabs(number) > 16777216.0)) {
        return fail(error, where, "expected a whole number (an int parameter)");
    }
    out = (float)number;
    return true;
}

// parameter names are Lua identifiers (scripts read them; the reader drops the others)
std::string notIdentifier(const std::string& name) {
    return "\"" + name + "\" is not a Lua identifier (letters, digits and _, not starting with a digit, not a keyword, e.g. Speed or is_grounded)";
}

// every reference to a parameter renamed (conditions, blend trees, speed parameters)
void renameParameter(anim::ControllerData& data, const std::string& from, const std::string& to) {
    for (anim::ControllerLayer& layer : data.layers) {
        for (anim::ControllerState& state : layer.states) {
            if (state.speedParameter == from) state.speedParameter = to;
            if (state.hasBlendTree && state.blendTree.parameter == from) state.blendTree.parameter = to;
        }
        for (anim::ControllerTransition& transition : layer.transitions) {
            for (anim::ControllerCondition& condition : transition.conditions) {
                if (condition.parameter == from) condition.parameter = to;
            }
        }
    }
}

bool applyOp(Project* project, const Json& op, const std::string& where, anim::ControllerData& data, std::string& error) {
    if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) return fail(error, where, "expected {op: \"add_state\", ...}");
    const std::string kind = op["op"].get<std::string>();
    if (const std::vector<std::string>* keys = opKeys(kind)) {
        if (!checkKeys(op, *keys, where, error)) return false;
    }

    // ---- parameters
    if (kind == "add_parameter" || kind == "update_parameter" || kind == "remove_parameter") {
        std::string name;
        if (!readString(op, "name", name, where, error, true)) return false;
        const int index = data.findParameter(name);
        if (kind == "add_parameter") {
            if (name.empty()) return fail(error, field(where, "name"), "is empty");
            if (!anim::isLuaIdentifier(name)) return fail(error, field(where, "name"), notIdentifier(name));
            if (index >= 0) return fail(error, field(where, "name"), "\"" + name + "\" is already a parameter");
            anim::ControllerParameter parameter;
            parameter.name = name;
            std::string type;
            if (!readString(op, "type", type, where, error, true)) return false;
            if (!anim::parameterTypeFromName(type, parameter.type)) return fail(error, field(where, "type"), "expected float, int, bool or trigger");
            if (op.contains("default") && !readParameterDefault(op["default"], parameter.type, parameter.defaultValue, field(where, "default"), error)) return false;
            data.parameters.push_back(parameter);
            return true;
        }
        if (index < 0) return fail(error, field(where, "name"), "no parameter \"" + name + "\"");
        if (kind == "remove_parameter") {
            // the conditions reading it go; speed parameters and blend trees lose it (validation
            // says so), as in the Animator window
            animator::removeParameter(data, index);
            return true;
        }
        anim::ControllerParameter& parameter = data.parameters[(size_t)index];
        if (op.contains("type")) {
            std::string type;
            if (!readString(op, "type", type, where, error)) return false;
            if (!anim::parameterTypeFromName(type, parameter.type)) return fail(error, field(where, "type"), "expected float, int, bool or trigger");
        }
        if (op.contains("default") && !readParameterDefault(op["default"], parameter.type, parameter.defaultValue, field(where, "default"), error)) return false;
        if (op.contains("rename")) {
            std::string rename;
            if (!readString(op, "rename", rename, where, error)) return false;
            if (rename.empty()) return fail(error, field(where, "rename"), "is empty");
            if (!anim::isLuaIdentifier(rename)) return fail(error, field(where, "rename"), notIdentifier(rename));
            if (rename != name && data.findParameter(rename) >= 0) return fail(error, field(where, "rename"), "\"" + rename + "\" is already a parameter");
            parameter.name = rename;
            renameParameter(data, name, rename);
        }
        return true;
    }

    // ---- layers
    if (kind == "add_layer") {
        anim::ControllerLayer layer;
        if (!readString(op, "name", layer.name, where, error, true)) return false;
        if (layer.name.empty()) return fail(error, field(where, "name"), "is empty");
        if (data.findLayer(layer.name) >= 0) return fail(error, field(where, "name"), "\"" + layer.name + "\" is already a layer");
        if (!readLayer(project, op, layer, where, error)) return false;
        data.layers.push_back(layer);
        return true;
    }
    if (kind == "update_layer" || kind == "remove_layer" || kind == "move_layer") {
        if (!op.contains("layer")) return fail(error, field(where, "layer"), "is required (an index or a name)");
        int index = 0;
        if (!findLayer(data, op, where, index, error)) return false;
        anim::ControllerLayer& layer = data.layers[(size_t)index];
        if (kind == "remove_layer") {
            if (index == 0) return fail(error, field(where, "layer"), "the base layer cannot be removed");
            data.layers.erase(data.layers.begin() + index);
            return true;
        }
        if (kind == "move_layer") {
            if (!op.contains("to") || !op["to"].is_number_integer()) return fail(error, field(where, "to"), "expected the new index");
            const int to = op["to"].get<int>();
            if (index == 0 || to <= 0 || (size_t)to >= data.layers.size()) {
                return fail(error, where, "layers move between index 1 and " + std::to_string(data.layers.size() - 1) + " (the base layer stays first)");
            }
            anim::ControllerLayer moved = layer;
            data.layers.erase(data.layers.begin() + index);
            data.layers.insert(data.layers.begin() + to, moved);
            return true;
        }
        if (op.contains("name")) {
            std::string name;
            if (!readString(op, "name", name, where, error)) return false;
            if (name.empty()) return fail(error, field(where, "name"), "is empty");
            if (name != layer.name && data.findLayer(name) >= 0) return fail(error, field(where, "name"), "\"" + name + "\" is already a layer");
            layer.name = name;
        }
        if (index == 0 && (op.contains("weight") || op.contains("blend"))) {
            return fail(error, where, "the base layer always has weight 1 and blends as override");
        }
        if (!readLayer(project, op, layer, where, error)) return false;
        return readPosition(op.contains("any_state_position") ? Json{{"position", op["any_state_position"]}} : Json::object(),
                            layer.hasAnyStatePosition, layer.anyStatePosition, field(where, "any_state_position"), error);
    }

    // ---- states and transitions: in a layer
    int layerIndex = 0;
    if (!findLayer(data, op, where, layerIndex, error)) return false;
    anim::ControllerLayer& layer = data.layers[(size_t)layerIndex];

    if (kind == "add_state") {
        anim::ControllerState state;
        if (!readString(op, "name", state.name, where, error, true)) return false;
        if (state.name.empty() || state.name == anim::ANY_STATE) return fail(error, field(where, "name"), "must be a name other than \"any\"");
        if (layer.findState(state.name) >= 0) return fail(error, field(where, "name"), "\"" + state.name + "\" is already a state of layer \"" + layer.name + "\"");
        if (!readState(project, op, state, where, error)) return false;
        gridPosition(layer, state);
        layer.states.push_back(state);
        return true;
    }
    if (kind == "update_state" || kind == "remove_state" || kind == "set_default_state") {
        std::string name;
        const char* key = op.contains("state") ? "state" : "name";
        if (!readString(op, key, name, where, error, true)) return false;
        int index = -1;
        if (!findState(layer, name, field(where, key), index, error)) return false;
        if (kind == "set_default_state") {
            layer.defaultState = name;
            return true;
        }
        if (kind == "remove_state") {
            layer.states.erase(layer.states.begin() + index);
            layer.transitions.erase(std::remove_if(layer.transitions.begin(), layer.transitions.end(), [&](const anim::ControllerTransition& t) {
                                        return t.from == name || t.to == name;
                                    }), layer.transitions.end());
            if (layer.defaultState == name) layer.defaultState.clear();
            return true;
        }
        anim::ControllerState& state = layer.states[(size_t)index];
        if (!readState(project, op, state, where, error)) return false;
        if (op.contains("rename")) {
            std::string rename;
            if (!readString(op, "rename", rename, where, error)) return false;
            if (rename.empty() || rename == anim::ANY_STATE) return fail(error, field(where, "rename"), "must be a name other than \"any\"");
            if (rename != name && layer.findState(rename) >= 0) return fail(error, field(where, "rename"), "\"" + rename + "\" is already a state");
            state.name = rename;
            for (anim::ControllerTransition& transition : layer.transitions) {
                if (transition.from == name) transition.from = rename;
                if (transition.to == name) transition.to = rename;
            }
            if (layer.defaultState == name) layer.defaultState = rename;
        }
        return true;
    }
    if (kind == "add_transition") {
        anim::ControllerTransition transition;
        if (!readString(op, "from", transition.from, where, error, true)) return false;
        if (!readString(op, "to", transition.to, where, error, true)) return false;
        int index = -1;
        if (transition.from != anim::ANY_STATE && !findState(layer, transition.from, field(where, "from"), index, error)) return false;
        if (!findState(layer, transition.to, field(where, "to"), index, error)) return false;
        if (!readTransition(op, transition, where, error)) return false;
        layer.transitions.push_back(transition);
        return true;
    }
    if (kind == "update_transition" || kind == "remove_transition") {
        if (!op.contains("index") || !op["index"].is_number_integer()) return fail(error, field(where, "index"), "expected the transition's index in its layer");
        const int index = op["index"].get<int>();
        if (index < 0 || (size_t)index >= layer.transitions.size()) {
            return fail(error, field(where, "index"), "layer \"" + layer.name + "\" has " + std::to_string(layer.transitions.size()) + " transition(s)");
        }
        if (kind == "remove_transition") {
            layer.transitions.erase(layer.transitions.begin() + index);
            return true;
        }
        anim::ControllerTransition& transition = layer.transitions[(size_t)index];
        int state = -1;
        if (op.contains("from")) {
            if (!readString(op, "from", transition.from, where, error)) return false;
            if (transition.from != anim::ANY_STATE && !findState(layer, transition.from, field(where, "from"), state, error)) return false;
        }
        if (op.contains("to")) {
            if (!readString(op, "to", transition.to, where, error)) return false;
            if (!findState(layer, transition.to, field(where, "to"), state, error)) return false;
        }
        return readTransition(op, transition, where, error);
    }
    return fail(error, field(where, "op"), "unknown op \"" + kind + "\" (add_parameter, update_parameter, remove_parameter, add_layer, update_layer, "
                "remove_layer, move_layer, add_state, update_state, remove_state, set_default_state, add_transition, update_transition, remove_transition)");
}

// ---- summaries ----

Json stateJson(Project* project, const anim::ControllerState& state, size_t index) {
    Json out = {{"index", index}, {"name", state.name}};
    if (!state.clip.empty()) out["clip"] = McpValues::clipReferenceToJson(project, state.clip);
    if (state.hasBlendTree) {
        Json children = Json::array();
        for (const anim::BlendTreeChild& child : state.blendTree.children) {
            Json item = {{"clip", McpValues::clipReferenceToJson(project, child.clip)}, {"threshold", r6(child.threshold)}};
            if (child.speed != 1.0f) item["speed"] = r6(child.speed);
            children.push_back(item);
        }
        out["blend_tree"] = {{"parameter", state.blendTree.parameter}, {"children", children}};
        if (state.blendTree.automateThresholds) out["blend_tree"]["automate_thresholds"] = true;
    }
    if (state.isEmpty()) out["empty"] = true;
    if (state.speed != 1.0f) out["speed"] = r6(state.speed);
    if (!state.speedParameter.empty()) out["speed_parameter"] = state.speedParameter;
    if (state.wrap != ClipWrapMode::Default) out["wrap"] = anim::wrapName(state.wrap);
    if (state.cycleOffset != 0.0f) out["cycle_offset"] = r6(state.cycleOffset);
    if (state.hasPosition) out["position"] = Json::array({r6(state.position[0]), r6(state.position[1])});
    return out;
}

Json transitionJson(const anim::ControllerTransition& transition, size_t index) {
    Json conditions = Json::array();
    for (const anim::ControllerCondition& condition : transition.conditions) {
        Json item = {{"parameter", condition.parameter}, {"mode", anim::conditionModeName(condition.mode)}};
        if (condition.mode != anim::ConditionMode::If && condition.mode != anim::ConditionMode::IfNot) item["threshold"] = r6(condition.threshold);
        conditions.push_back(item);
    }
    Json out = {{"index", index}, {"from", transition.from}, {"to", transition.to}, {"conditions", conditions},
                {"has_exit_time", transition.hasExitTime}, {"exit_time", r6(transition.exitTime)}, {"duration", r6(transition.duration)},
                {"fixed_duration", transition.fixedDuration}, {"offset", r6(transition.offset)},
                {"interruption", anim::interruptionName(transition.interruption)}};
    if (transition.from == anim::ANY_STATE) out["can_transition_to_self"] = transition.canTransitionToSelf;
    return out;
}

std::string summaryText(const std::string& path, const anim::ControllerData& data, const std::vector<ClipWarning>& warnings) {
    std::string text = path + ": " + std::to_string(data.parameters.size()) + " parameter(s), " + std::to_string(data.layers.size()) + " layer(s)";
    for (const anim::ControllerLayer& layer : data.layers) {
        const int def = layer.defaultStateIndex();
        text += "\n" + layer.name + ": " + std::to_string(layer.states.size()) + " state(s)" +
                (def >= 0 ? " (default " + layer.states[(size_t)def].name + ")" : std::string()) + ", " +
                std::to_string(layer.transitions.size()) + " transition(s)";
    }
    for (size_t i = 0; i < warnings.size() && i < 12; i++) text += "\nwarning: " + warnings[i].path + ": " + warnings[i].message;
    if (warnings.size() > 12) text += "\n... " + std::to_string(warnings.size() - 12) + " more warning(s)";
    return text;
}

// the controller path argument: a project-relative .controller
// the clips a player's tracks come from: with a controller its states' clips and blend-tree
// children (on its layer `layer`, -1: every layer), else its entries (on `layer`, -1: all)
std::vector<std::string> playedClips(Project* project, const AnimationPlayerComponent& player, int layer) {
    std::vector<std::string> clips;
    if (!player.controller.empty()) {
        std::shared_ptr<AnimatorControllerDocument> document = AnimatorControllerDocument::open(project, assetJson(project, player.controller), nullptr);
        if (!document) return clips;
        const anim::ControllerData& data = document->data();
        for (size_t l = 0; l < data.layers.size(); l++) {
            if (layer >= 0 && (int)l != layer) continue;
            for (const anim::ControllerState& state : data.layers[l].states) {
                if (!state.clip.empty()) clips.push_back(state.clip);
                for (const anim::BlendTreeChild& child : state.blendTree.children) {
                    if (state.hasBlendTree && !child.clip.empty()) clips.push_back(child.clip);
                }
            }
        }
        return clips;
    }
    for (const AnimationPlayerClip& entry : player.clips) {
        if ((layer < 0 || entry.layer == layer) && !entry.clip.empty()) clips.push_back(entry.clip);
    }
    return clips;
}

// the entities those clips' transform tracks bind below `root`, once per path
std::vector<std::pair<std::string, Entity>> boundTargets(Project* project, Scene* scene, Entity root, const std::vector<std::string>& clips) {
    std::vector<std::pair<std::string, Entity>> targets;
    std::set<std::string> seen;
    for (const std::string& clip : clips) {
        std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, clip, nullptr);
        if (!document) continue;
        for (const ClipTrackData& track : document->data().tracks) {
            if (track.channel == ClipChannel::Property || !seen.insert(track.path).second) continue;
            const Entity target = track.path.empty() ? root : anim::resolvePath(scene, root, track.path);
            if (target != NULL_ENTITY) targets.push_back({track.path, target});
        }
    }
    return targets;
}

// a masked layer's coverage of what its clips animate (the Inspector's Bindings line)
Json maskCoverageEntry(Project* project, Scene* scene, Entity root, const std::string& layer, const std::string& mask,
                       const std::vector<std::string>& clips) {
    Json entry = {{"layer", layer}};
    std::string error;
    std::shared_ptr<AvatarMaskDocument> document = AvatarMaskDocument::open(project, mask, &error);
    if (!document) {
        entry["error"] = error + " (the layer contributes nothing)";
        return entry;
    }
    std::vector<Entity> targets;
    for (const std::pair<std::string, Entity>& target : boundTargets(project, scene, root, clips)) targets.push_back(target.second);
    const animlayersui::MaskCoverage result = animlayersui::maskCoverage(scene, root, *anim::AvatarMask::build(document->data()), targets);
    entry["covered"] = result.covered;
    entry["total"] = result.total;
    Json warnings = warningsJson(document->loadWarnings());
    if (result.humanoidWithoutRig) {
        warnings.push_back({{"path", "humanoid"}, {"message", "the root has no humanoid rig (" + result.rigError + "): the body parts do not apply"}});
    }
    entry["warnings"] = warnings;
    return entry;
}

bool controllerArg(const CallPtr& call, std::string& path, fs::path& full) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    if (!args.contains("controller") || !args["controller"].is_string() || args["controller"].get<std::string>().empty()) {
        call->fail("controller is required: a project-relative .controller path, e.g. assets/anim/hero.controller.");
        return false;
    }
    path = args["controller"].get<std::string>();
    if (!Util::isAnimatorControllerFile(path)) {
        call->fail("controller must end in .controller (\"" + path + "\").");
        return false;
    }
    std::string stored, why;
    if (!McpValues::assetPathFromJson(project, path, stored, why)) {
        call->fail("controller: " + why);
        return false;
    }
    full = project->resolveAssetPath(stored);
    path = projectRel(project, full);
    return true;
}

} // namespace

bool applyOps(Project* project, const Json& ops, anim::ControllerData& data, std::string& error) {
    if (ops.is_null()) return true;
    if (!ops.is_array()) {
        error = "ops must be an array of {op, ...} (jasper://docs/animation, Animator Controllers)";
        return false;
    }
    anim::ControllerData edited = data;
    for (size_t i = 0; i < ops.size(); i++) {
        if (!applyOp(project, ops[i], "ops[" + std::to_string(i) + "]", edited, error)) return false;
    }
    data = std::move(edited);
    return true;
}

Json controllerSummary(Project* project, const std::string& path, const anim::ControllerData& input, const std::vector<ClipWarning>& loadWarnings) {
    anim::ControllerData data = input;
    anim::normalizeController(data, nullptr);
    Json parameters = Json::array();
    for (const anim::ControllerParameter& parameter : data.parameters) {
        Json item = {{"name", parameter.name}, {"type", anim::parameterTypeName(parameter.type)}};
        switch (parameter.type) {
            case anim::ControllerParameterType::Float: item["default"] = r6(parameter.defaultValue); break;
            case anim::ControllerParameterType::Int: item["default"] = (int)parameter.defaultValue; break;
            case anim::ControllerParameterType::Bool: item["default"] = parameter.defaultValue != 0.0f; break;
            case anim::ControllerParameterType::Trigger: break;
        }
        parameters.push_back(item);
    }
    Json layers = Json::array();
    for (size_t l = 0; l < data.layers.size(); l++) {
        const anim::ControllerLayer& layer = data.layers[l];
        Json states = Json::array();
        for (size_t s = 0; s < layer.states.size(); s++) states.push_back(stateJson(project, layer.states[s], s));
        Json transitions = Json::array();
        for (size_t t = 0; t < layer.transitions.size(); t++) transitions.push_back(transitionJson(layer.transitions[t], t));
        const int def = layer.defaultStateIndex();
        layers.push_back({{"index", l}, {"name", layer.name}, {"weight", r6(layer.weight)}, {"blend", anim::layerBlendName(layer.blend)},
                          {"mask", assetJson(project, layer.mask)}, {"default_state", def >= 0 ? layer.states[(size_t)def].name : std::string()},
                          {"states", states}, {"transitions", transitions}});
    }
    std::vector<ClipWarning> warnings = loadWarnings;
    for (const ClipWarning& warning : validate(project, data)) warnings.push_back(warning);
    Json out = {{"controller", path}, {"parameters", parameters}, {"layers", layers}, {"warnings", warningsJson(warnings)},
                {"users", usersJson(project, findControllerUsers(project, path))}};
    if (!data.guid.empty()) out["guid"] = data.guid;
    return out;
}

// ---- get_animation {mask} / {controller} ----

void getMask(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    const std::string given = args["mask"].is_string() ? args["mask"].get<std::string>() : std::string();
    std::string stored, guid, why;
    if (!McpValues::animAssetReferenceFromJson(project, given, true, stored, guid, why) || stored.empty()) {
        call->fail("mask " + (why.empty() ? std::string("must be a .mask path, e.g. assets/anim/arm.mask") : why));
        return;
    }
    const std::string path = projectRel(project, project->resolveAssetPath(stored));
    std::string error;
    std::shared_ptr<AvatarMaskDocument> document = AvatarMaskDocument::open(project, path, &error);
    if (!document) {
        call->fail("mask: " + error);
        return;
    }
    const anim::AvatarMaskData& data = document->data();
    Json humanoid = nullptr;
    if (data.hasHumanoid) {
        humanoid = Json::object();
        for (int p = 0; p < anim::AVATAR_MASK_PART_COUNT; p++) {
            if (data.parts[(size_t)p]) humanoid[anim::avatarMaskPartName((anim::AvatarMaskPart)p)] = r6(*data.parts[(size_t)p]);
        }
    }
    Json transforms = Json::array();
    for (const anim::AvatarMaskTransform& transform : data.transforms) {
        transforms.push_back({{"path", transform.path}, {"weight", r6(transform.weight)}, {"children", transform.children}});
    }
    Json out = {{"mask", path}, {"guid", data.guid}, {"default", r6(data.defaultWeight)}, {"humanoid", humanoid}, {"transforms", transforms},
                {"warnings", warningsJson(document->loadWarnings())}, {"users", usersJson(project, findMaskUsers(project, path))}};
    if (!data.previewModel.empty()) out["preview_model"] = assetJson(project, data.previewModel);
    char defaultText[32];
    std::snprintf(defaultText, sizeof(defaultText), "%g", r6(data.defaultWeight));
    std::string text = path + ": default " + defaultText + ", " +
                       (data.hasHumanoid ? std::to_string(humanoid.size()) + " body part(s), " : std::string()) +
                       std::to_string(data.transforms.size()) + " transform(s), " + std::to_string(out["users"].size()) + " user(s)";

    if (args.contains("root") && !args["root"].is_null()) {
        std::string sceneError;
        SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(nullptr), sceneError);
        if (!scene) {
            call->fail(sceneError);
            return;
        }
        const Entity root = McpRefs::resolveEntity(scene, args["root"], sceneError);
        if (root == NULL_ENTITY) {
            call->fail("root: " + sceneError);
            return;
        }
        Scene* engineScene = scene->scene;
        std::shared_ptr<const anim::AvatarMask> mask = anim::AvatarMask::build(data);
        anim::HumanoidSlots slots;
        if (mask->hasHumanoid()) slots = anim::resolveHumanoidSlots(engineScene, root);
        const std::vector<Entity> entries = mask->bindTransforms(engineScene, root);
        // the entities the root's player binds (its controller's clips when it has one);
        // without a player every descendant
        std::vector<std::pair<std::string, Entity>> entities;
        if (const AnimationPlayerComponent* player = engineScene->findComponent<AnimationPlayerComponent>(root)) {
            entities = boundTargets(project, engineScene, root, playedClips(project, *player, -1));
        }
        if (entities.empty()) entities = anim::descendantPaths(engineScene, root);
        Json coverage = Json::array();
        size_t covered = 0;
        for (const std::pair<std::string, Entity>& entity : entities) {
            const anim::AvatarMaskWeight weight = mask->weightOf(engineScene, root, entity.second, entries, slots.valid ? &slots : nullptr);
            if (weight.weight > 0.0f) covered++;
            if (coverage.size() < MAX_COVERAGE) coverage.push_back({{"path", entity.first}, {"weight", r6(weight.weight)}, {"rule", weight.ruleName()}});
        }
        out["root"] = McpRefs::entityJson(scene, root);
        out["coverage"] = coverage;
        out["coverage_total"] = entities.size();
        out["covered"] = covered;
        if (mask->hasHumanoid()) {
            out["humanoid_rig"] = slots.valid;
            if (!slots.valid) out["humanoid_rig_error"] = slots.error;
        }
        text += "; on \"" + engineScene->getEntityName(root) + "\": " + std::to_string(covered) + "/" + std::to_string(entities.size()) +
                " entities covered" + (mask->hasHumanoid() && !slots.valid ? " (no humanoid rig: the body parts do not apply)" : "");
    }
    for (const Json& warning : out["warnings"]) text += "\nwarning: " + warning["path"].get<std::string>() + ": " + warning["message"].get<std::string>();
    call->ok(text, out);
}

void getController(const CallPtr& call) {
    Project* project = call->ctx().project();
    std::string path;
    fs::path full;
    if (!controllerArg(call, path, full)) return;
    std::string error;
    std::shared_ptr<AnimatorControllerDocument> document = AnimatorControllerDocument::open(project, path, &error);
    if (!document) {
        call->fail("controller: " + error + (Util::isAnimatorControllerFile(path) ? " (create one with edit_animator {controller, create: {}})" : ""));
        return;
    }
    Json out = controllerSummary(project, path, document->data(), document->loadWarnings());
    std::vector<ClipWarning> warnings;
    for (const Json& warning : out["warnings"]) warnings.push_back({warning["path"], warning["message"]});
    call->ok(summaryText(path, document->data(), warnings), out);
}

// ---- get_animation {entity} ----

void addPlayerLayers(Project* project, SceneProject* scene, Entity root, Json& item) {
    Scene* engineScene = scene->scene;
    const AnimationPlayerComponent* found = engineScene->findComponent<AnimationPlayerComponent>(root);
    if (!found) return;
    const AnimationPlayerComponent& player = *found;
    const bool playing = scene->playState != ScenePlayState::STOPPED && player.runtime;
    const std::vector<std::string> names = anim::playerStateNames(player.clips);

    if (!player.controller.empty()) {
        const std::string path = assetJson(project, player.controller);
        Json controller = {{"path", path}};
        std::string error;
        std::shared_ptr<AnimatorControllerDocument> document = AnimatorControllerDocument::open(project, path, &error);
        if (document) {
            std::vector<ClipWarning> warnings = document->loadWarnings();
            for (const ClipWarning& warning : validate(project, document->data())) warnings.push_back(warning);
            controller["warnings"] = warningsJson(warnings);
        } else {
            controller["error"] = error;
        }
        if (!player.clips.empty()) controller["ignored_clips"] = player.clips.size();
        // the controller's masked layers: what their states' clips animate
        if (document) {
            Json coverage = Json::array();
            const anim::ControllerData& data = document->data();
            for (size_t l = 0; l < data.layers.size(); l++) {
                if (data.layers[l].mask.empty()) continue;
                coverage.push_back(maskCoverageEntry(project, engineScene, root, data.layers[l].name, data.layers[l].mask,
                                                     playedClips(project, player, (int)l)));
            }
            if (!coverage.empty()) item["mask_coverage"] = coverage;
        }
        if (playing && player.runtime->controller && player.runtime->controller->controller) {
            const anim::ControllerRuntime& runtime = *player.runtime->controller;
            const anim::ControllerData& data = runtime.controller->getData();
            Json parameters = Json::object();
            for (size_t p = 0; p < data.parameters.size() && p < runtime.params.size(); p++) {
                const float value = runtime.params[p].value;
                switch (data.parameters[p].type) {
                    case anim::ControllerParameterType::Float: parameters[data.parameters[p].name] = r6(value); break;
                    case anim::ControllerParameterType::Int: parameters[data.parameters[p].name] = (int)value; break;
                    default: parameters[data.parameters[p].name] = value != 0.0f; break;
                }
            }
            Json layers = Json::array();
            for (size_t l = 0; l < data.layers.size(); l++) {
                const anim::ControllerStateView current = anim::controllerStateInfo(player, (int)l, false);
                Json layer = {{"name", data.layers[l].name}, {"weight", r6(player.runtime->layerWeight((int)l))},
                              {"current", current.valid ? Json(current.name) : Json(nullptr)}, {"normalized_time", r6(current.normalizedTime)}};
                const anim::ControllerStateView next = anim::controllerStateInfo(player, (int)l, true);
                if (next.valid && l < runtime.layers.size()) {
                    const anim::ControllerTransitionRuntime& transition = runtime.layers[l].transition;
                    layer["transition"] = {{"to", next.name},
                                           {"progress", r6(transition.duration > 0.0f ? std::clamp(transition.elapsed / transition.duration, 0.0f, 1.0f) : 1.0f)}};
                }
                layers.push_back(layer);
            }
            controller["parameters"] = parameters;
            controller["layers"] = layers;
            controller["stopped"] = runtime.stopped;
        }
        item["controller"] = controller;
        return;
    }

    // layers (the effective list) and their states; play mode: live weights and queues
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    Json layersJson = Json::array();
    for (size_t l = 0; l < layers.size(); l++) {
        Json states = Json::array();
        for (size_t i = 0; i < player.clips.size() && i < names.size(); i++) {
            const int layer = player.clips[i].layer >= 0 && (size_t)player.clips[i].layer < layers.size() ? player.clips[i].layer : 0;
            if ((size_t)layer == l) states.push_back(names[i]);
        }
        Json layer = {{"index", l}, {"name", layers[l].name}, {"weight", r6(l == 0 ? 1.0f : layers[l].weight)},
                      {"blend", anim::layerBlendName(l == 0 ? AnimationLayerBlend::Override : layers[l].blend)},
                      {"mask", assetJson(project, layers[l].mask)}, {"states", states}};
        if (playing && l < player.runtime->layers.size()) {
            const anim::LayerRuntime& runtime = player.runtime->layers[l];
            layer["live_weight"] = r6(player.runtime->layerWeight((int)l));
            if (runtime.maskPath != layers[l].mask) layer["live_mask"] = assetJson(project, runtime.maskPath);
            if (runtime.blend != layers[l].blend && l > 0) layer["live_blend"] = anim::layerBlendName(runtime.blend);
            Json queue = Json::array();
            for (const anim::QueuedState& queued : runtime.queue) {
                if (queued.state < player.runtime->states.size()) queue.push_back(player.runtime->states[queued.state].name);
            }
            layer["queue"] = queue;
        }
        layersJson.push_back(layer);
    }
    item["layers"] = layersJson;

    // the masked layers' coverage (the Inspector's Bindings lines)
    Json coverage = Json::array();
    for (size_t l = 0; l < layers.size(); l++) {
        if (!layers[l].mask.empty()) coverage.push_back(maskCoverageEntry(project, engineScene, root, layers[l].name, layers[l].mask,
                                                                          playedClips(project, player, (int)l)));
    }
    if (!coverage.empty()) item["mask_coverage"] = coverage;
}

// ---- write_file ----

bool checkMaskText(const std::string& text, std::vector<std::string>& warnings, std::string& error) {
    const anim::AvatarMaskParseResult parsed = anim::parseAvatarMask(text);
    if (!parsed.ok()) {
        error = parsed.error.toString();
        return false;
    }
    for (const ClipWarning& warning : parsed.warnings) warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    return true;
}

bool checkControllerText(Project* project, const std::string& text, std::vector<std::string>& warnings, std::string& error) {
    const anim::ControllerParseResult parsed = anim::parseController(text);
    if (!parsed.ok()) {
        error = parsed.error.toString();
        return false;
    }
    for (const ClipWarning& warning : parsed.warnings) warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    for (const ClipWarning& warning : validate(project, *parsed.data)) warnings.push_back(warning.path + ": " + warning.message);
    return true;
}

} // namespace jasperengine::editor::mcp::animator_tools

// ---- edit_animator ----

namespace jasperengine::editor::mcp {

namespace {

void editAnimatorTool(const CallPtr& call) {
    using namespace animator_tools;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string path;
    fs::path full;
    if (!controllerArg(call, path, full)) return;
    std::error_code ec;
    const bool exists = fs::exists(full, ec);
    const bool create = args.contains("create") && !args["create"].is_null();
    if (create && (!args["create"].is_object() || (!args["create"].empty() && (args["create"].size() > 1 || !args["create"].contains("from_entity"))))) {
        std::string key;
        for (auto it = args["create"].begin(); args["create"].is_object() && it != args["create"].end(); ++it) {
            if (it.key() != "from_entity") key = it.key();
        }
        call->fail((key.empty() ? std::string() : "create." + key + ": unknown key (allowed: from_entity). ") +
                   "create must be {} (an empty controller with a Base Layer) or {from_entity} (one state per clip of that Animation Player).");
        return;
    }
    if (create && exists) {
        call->fail(path + " exists: edit it without create (get_animation {controller} reads it).");
        return;
    }
    if (!create && !exists) {
        call->fail(path + " does not exist: pass create: {} (or {from_entity}) to make it.");
        return;
    }

    // the starting data: the file, an empty controller, or the player's clips
    anim::ControllerData data;
    std::shared_ptr<AnimatorControllerDocument> document;
    SceneProject* fromScene = nullptr;
    Entity fromEntity = NULL_ENTITY;
    std::string error;
    if (!create) {
        document = AnimatorControllerDocument::open(project, path, &error);
        if (!document) {
            call->fail("controller: " + error);
            return;
        }
        data = document->data();
    } else if (args["create"].contains("from_entity")) {
        fromScene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(nullptr), error);
        if (!fromScene) {
            call->fail(error);
            return;
        }
        if (fromScene->playState != ScenePlayState::STOPPED) {
            call->fail("create.from_entity sets the player's controller: stop play mode first.");
            return;
        }
        fromEntity = McpRefs::resolveEntity(fromScene, args["create"]["from_entity"], error);
        if (fromEntity == NULL_ENTITY) {
            call->fail("create.from_entity: " + error);
            return;
        }
        const AnimationPlayerComponent* player = fromScene->scene->findComponent<AnimationPlayerComponent>(fromEntity);
        if (!player) {
            call->fail("create.from_entity: \"" + fromScene->scene->getEntityName(fromEntity) + "\" has no Animation Player.");
            return;
        }
        data = animlayersui::controllerFromClips(*player);
    } else {
        anim::ControllerLayer base;
        base.name = anim::BASE_LAYER_NAME;
        data.layers.push_back(base);
    }

    if (!applyOps(project, args.contains("ops") ? args["ops"] : Json(nullptr), data, error)) {
        call->fail(error + " (nothing was changed).");
        return;
    }

    std::string how;
    if (!create) {
        if (!document->commit(data, "Edit Animator Controller", "", &error)) {
            call->fail("edit_animator: " + error);
            return;
        }
        how = "one project undo step";
    } else if (fromEntity != NULL_ENTITY) {
        data.guid = anim::generateClipGuid();
        if (!animlayersui::createControllerFile(project, fromScene->id, fromEntity, path, data, error)) {
            call->fail("edit_animator: " + error);
            return;
        }
        how = "one scene undo step with the player's controller";
    } else {
        document = AnimatorControllerDocument::create(project, path, data, &error);
        if (!document) {
            call->fail("edit_animator: " + error);
            return;
        }
        how = "one project undo step";
    }
    // what the file holds now
    std::shared_ptr<AnimatorControllerDocument> written = AnimatorControllerDocument::open(project, path, &error);
    const anim::ControllerData& result = written ? written->data() : data;
    Json out = controllerSummary(project, path, result, written ? written->loadWarnings() : std::vector<ClipWarning>());
    out["created"] = create;
    std::vector<ClipWarning> warnings;
    for (const Json& warning : out["warnings"]) warnings.push_back({warning["path"], warning["message"]});
    const size_t ops = args.contains("ops") && args["ops"].is_array() ? args["ops"].size() : 0;
    call->ok(std::string(create ? "Created " : "Edited ") + path + " (" + std::to_string(ops) + " op(s), " + how + ").\n" +
             summaryText(path, result, warnings), out);
}

} // namespace

void registerAnimatorTools(McpRegistry& registry) {
    using namespace schema;
    const Json layerRef = Json{{"type", Json::array({"integer", "string"})}};
    const Json opItem = object({
        {"op", stringEnum("", {"add_parameter", "update_parameter", "remove_parameter", "add_layer", "update_layer", "remove_layer",
                               "move_layer", "add_state", "update_state", "remove_state", "set_default_state", "add_transition",
                               "update_transition", "remove_transition"})},
        {"layer", layerRef},
        {"name", string("")}, {"rename", string("")}, {"type", stringEnum("", {"float", "int", "bool", "trigger"})}, {"default", any("")},
        {"state", string("")}, {"clip", string("")}, {"blend_tree", any("{parameter, children: [{clip, threshold}]}")},
        {"speed", number("")}, {"speed_parameter", string("")}, {"wrap", string("")}, {"cycle_offset", number("")}, {"position", any("")},
        {"weight", number("")}, {"blend", stringEnum("", {"override", "additive"})}, {"mask", string("")}, {"to", any("")},
        {"from", string("State or \"any\".")}, {"index", integer("")},
        {"conditions", array(object({{"parameter", string("")}, {"mode", stringEnum("", {"greater", "less", "equals", "not_equal", "if", "if_not"})},
                                     {"threshold", number("")}}, {"parameter", "mode"}), "")},
        {"has_exit_time", boolean("")}, {"exit_time", number("")}, {"duration", number("")}, {"fixed_duration", boolean("")},
        {"offset", number("")}, {"interruption", stringEnum("", {"none", "current_state", "next_state"})}, {"can_transition_to_self", boolean("")}
    }, {"op"}, true);

    ToolDef tool;
    tool.name = "edit_animator";
    tool.title = "Edit Animator Controller";
    tool.description =
        "Edit an Animator Controller (.controller: parameters, layers with state machines, Any State, transitions, 1D blend "
        "trees) with ops, all or nothing, one undo step. create: {} (a Base Layer) or {from_entity} (one state per clip "
        "of its Animation Player, set as its controller). Returns the summary with indices and validation warnings "
        "(dead ends, unreachable states, parameter types). Play it: set_properties {animationplayer: {controller}}. "
        "Ops and fields: jasper://docs/animation (Animator Controllers).";
    tool.inputSchema = object({
        {"controller", assetPath("")},
        {"create", object({{"from_entity", entityRef("")}})},
        {"scene", sceneRef()},
        {"ops", array(opItem, "")}
    }, {"controller"});
    tool.annotations.readOnly = false;
    tool.annotations.idempotent = false;
    tool.allowedWhilePlaying = true;
    tool.handler = editAnimatorTool;
    registry.addTool(std::move(tool));
}

} // namespace jasperengine::editor::mcp
